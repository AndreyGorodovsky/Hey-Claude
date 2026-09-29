/*
 * Audio in and out over I2S.
 *
 * I2S is a three-wire serial bus for audio: a bit clock (BCLK, called SCK on
 * the microphone), a word-select line (WS, called LRC on the amplifier) that
 * switches between the left and right channel once per sample, and one data
 * line. Each word-select period carries one "frame": a left and a right
 * "slot", one sample each. The sample rate is frames per second: 16 kHz means
 * 16,000 frames every second.
 *
 * The ESP32-S3 has two independent I2S controllers. The microphone gets
 * controller 0 and the amplifier controller 1, so capture and playback never
 * share a bus and can run at different sample rates. ESP-IDF calls one
 * direction of a controller a "channel": here a receive channel on 0 and a
 * transmit channel on 1. The chip is the bus "master": it generates both
 * clocks, and the modules follow them.
 *
 * Data moves between the controllers and RAM by DMA (direct memory access):
 * the I2S hardware reads or writes a ring of small buffers in internal RAM on
 * its own, without the CPU. i2s_channel_read() and i2s_channel_write() only
 * copy between those DMA buffers and the caller's memory, blocking while the
 * ring is empty (read) or full (write). That blocking is what paces capture
 * and playback in real time.
 *
 * Capture. The INMP441 sends each sample as 24 bits at the top of a 32-bit
 * slot, in the left slot only because its L/R pin is tied to ground; the
 * right slot reads zero. The controller reads both slots ("stereo") and the
 * left one is kept. Its "mono" mode, which the ESP-IDF documentation says
 * reads the left slot alone, was measured on 2026-09-28 to deliver both
 * slots anyway at 32 bits, interleaving every sample with a zero, which
 * halved the audio and made speech sound robotic. Of each kept 32-bit word,
 * the top 16 bits are used: the 16-bit format the rest of the system uses.
 *
 * The microphone runs continuously from audio_init() on. It powers down
 * whenever its clock stops, and after each power-up its output takes about
 * 2 s to settle: measured on 2026-09-28 in a quiet room, the level fell from
 * about -32 dBFS to its resting -68 dBFS over that time. Stopping it between
 * recordings would put that disturbance at the start of every one. While no
 * task reads, the driver keeps only the newest DMA_DESC - 1 buffers and
 * silently drops older ones.
 *
 * Playback. The MAX98357A's SD pin is its on/off switch: low is off, and high
 * from a 3.3 V GPIO (a general-purpose digital pin that the program sets high
 * or low) turns it on playing the left slot. Each 16-bit sample is
 * sent in both slots ("mono" with both slots selected), so the slot choice
 * does not matter. The amplifier is switched on only while something plays,
 * and always while the I2S clock is already running with silence, which avoids
 * the click it makes when it wakes up without a clock.
 *
 * Context. A task is a FreeRTOS thread: an independent piece of the program
 * that the scheduler runs, pausing and resuming it, on one of the chip's two
 * CPU cores. Every function here runs in the task that calls it, except the
 * set-up inside audio_init(), which runs in a short-lived task on core 1.
 * The I2S driver moves buffers in an interrupt handler: a short function the
 * hardware runs immediately when a DMA buffer completes, pausing whatever
 * task was running on that core. ESP-IDF places an interrupt handler on the
 * core that set it up; doing the set-up on core 1 keeps the audio interrupts
 * off core 0, where the WiFi driver's interrupts run (ARCHITECTURE.md, task
 * and core allocation).
 */
#include "audio.h"

#include <stdbool.h>
#include "board.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio";

/* DMA ring for each direction: DMA_DESC buffers of DMA_FRAMES frames each.
 * These are ESP-IDF's defaults, written out because audio_play_stop() depends
 * on them. At 16 kHz one buffer is 15 ms. The driver hands finished buffers to
 * readers through a queue of DMA_DESC - 1 entries, so at most 5 buffers,
 * 75 ms of capture, wait to be read. */
#define DMA_DESC            6
#define DMA_FRAMES          240

/* Playback rate used until the first audio_play_start() sets the real one.
 * Any valid rate would do; this is the rate of spoken replies. */
#define PLAY_RATE_DEFAULT   24000

/* Time allowed for the amplifier to come out of shutdown after SD goes high,
 * before real audio is sent, so the first sound is not cut short. Chosen
 * generously rather than taken from the datasheet. The scheduler counts time
 * in 10 ms ticks (CONFIG_FREERTOS_HZ=100) and a delay can end up to one tick
 * early, so 20 ms guarantees at least 10. */
#define AMP_WAKE_MS         20

/* Playback rates accepted by audio_play_start(). The rate arrives from the
 * server in stage 6, so it is checked rather than trusted; this range covers
 * the rates speech services produce. */
#define PLAY_RATE_MIN       8000
#define PLAY_RATE_MAX       48000

/* Right shift that turns the microphone's 32-bit word into a 16-bit sample.
 * 16 keeps the top 16 bits, the range for the loudest sounds. Speech at
 * 0.5-1 m measured about -52 dBFS with this value (2026-09-28), which is
 * quiet; a smaller shift would add gain but would need clipping. Whether to
 * change it is decided with real speech-to-text results (KNOWN-ISSUES R12). */
#define CAPTURE_SHIFT       16

/* The set-up runs on AUDIO_CORE (audio.h), and so do the I2S interrupts */
#define INIT_TASK_STACK     3072    /* bytes; the set-up calls need little, this leaves margin */

/* Handles for the two I2S channels, created once by audio_init() and never
 * freed. */
static i2s_chan_handle_t s_rx;
static i2s_chan_handle_t s_tx;

/* Set only when audio_init() has completed every step. A failure part-way
 * leaves it false, so no function uses a half-configured channel. Written
 * once at boot by the set-up task, before audio_init() returns and so before
 * any other task calls in, so it needs no lock. */
static bool s_ready;

/* Hand-over between audio_init() and its set-up task: the result, and the
 * task to wake when it is ready. Each is written by one task only. */
static esp_err_t s_init_result;
static TaskHandle_t s_init_caller;

/* Scratch buffer for capture: one DMA buffer of raw frames, each a left and a
 * right 32-bit word, is copied here, then the left words are reduced to 16
 * bits into the caller's buffer. Static, so it lives in internal RAM and costs
 * no stack. Safe without a lock because capture has one user at a time (see
 * audio.h). */
static int32_t s_raw[DMA_FRAMES * 2];

/* One DMA buffer's worth of silence for audio_play_stop(). `const` places it
 * in flash rather than RAM; that is fine because i2s_channel_write() copies it
 * into the DMA buffers rather than having the DMA read it directly. */
static const int16_t s_silence[DMA_FRAMES];

/* The set-up itself, run by init_task on core 1. A failure part-way leaves
 * the channels already created allocated; at boot that costs a few KB once,
 * and s_ready stays false, so they are never used. */
static esp_err_t init_on_this_core(void)
{
    /* ESP_RETURN_ON_ERROR(call, TAG, message): if the call does not return
     * ESP_OK, log the message under TAG and return that error from this
     * function. ESP_RETURN_ON_FALSE does the same when a condition is false.
     * They keep the error handling to one line per step.
     *
     * Amplifier off first. Until this line runs the pin is floating (not
     * driven either way), which is harmless while no I2S clock is running,
     * because the amplifier then has nothing to play. The level is set before
     * the pin becomes an output, so it never briefly drives high. */
    gpio_set_level(BOARD_AMP_SD, 0);
    const gpio_config_t sd = {
        .pin_bit_mask = 1ULL << BOARD_AMP_SD,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&sd), TAG, "amp SD pin");

    /* Microphone: receive channel on controller 0. The channel handle, s_rx,
     * is how every later driver call names this channel. */
    i2s_chan_config_t rx_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    rx_chan.dma_desc_num = DMA_DESC;
    rx_chan.dma_frame_num = DMA_FRAMES;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&rx_chan, NULL, &s_rx), TAG, "rx channel");
    const i2s_std_config_t rx_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_CAPTURE_RATE),
        /* Philips framing (the standard I2S timing), 32-bit slots, both
         * slots read; see the top of this file for why not mono */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,    /* the INMP441 needs no master clock */
            .bclk = BOARD_MIC_SCK,
            .ws   = BOARD_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = BOARD_MIC_SD,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &rx_cfg), TAG, "rx init");

    /* Amplifier: transmit channel on controller 1 */
    i2s_chan_config_t tx_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    tx_chan.dma_desc_num = DMA_DESC;
    tx_chan.dma_frame_num = DMA_FRAMES;
    /* If the writer falls behind, send silence instead of repeating the last
     * buffer, which would sound like a buzz */
    tx_chan.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&tx_chan, &s_tx, NULL), TAG, "tx channel");
    i2s_std_config_t tx_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(PLAY_RATE_DEFAULT),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,    /* the MAX98357A needs no master clock */
            .bclk = BOARD_AMP_BCLK,
            .ws   = BOARD_AMP_LRC,
            .dout = BOARD_AMP_DIN,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    /* Mono defaults to the left slot only, leaving the right slot silent.
     * Selecting both makes the controller send each sample in both slots. */
    tx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &tx_cfg), TAG, "tx init");

    /* The microphone starts now and runs from here on (see the top of this
     * file). The amplifier's channel stays disabled, with no clock on its
     * pins, until something plays. */
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "rx enable");
    s_ready = true;
    return ESP_OK;
}

static void init_task(void *arg)
{
    s_init_result = init_on_this_core();
    xTaskNotifyGive(s_init_caller);     /* wake audio_init() */
    vTaskDelete(NULL);                  /* NULL: this task; does not return */
}

esp_err_t audio_init(void)
{
    ESP_RETURN_ON_FALSE(s_init_caller == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "already initialised");

    /* Run the set-up on core 1 and wait for it. The set-up task gets the
     * caller's own priority, so it neither delays nor is delayed by anything
     * the caller would not be. A task notification is a lightweight signal
     * sent straight to one task; ulTaskNotifyTake() sleeps until it arrives. */
    s_init_caller = xTaskGetCurrentTaskHandle();
    if (xTaskCreatePinnedToCore(init_task, "audio_init", INIT_TASK_STACK, NULL,
                                uxTaskPriorityGet(NULL), NULL, AUDIO_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return s_init_result;
}

esp_err_t audio_capture_flush(void)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    /* Each read with a zero timeout takes one waiting buffer, or returns an
     * error at once when none is left. The oldest waiting buffer is also the
     * next one the DMA fills, so it may already be partly overwritten with
     * new audio; it is discarded with the rest. The limit only guards
     * against looping forever; at most DMA_DESC - 1 buffers wait. */
    for (int i = 0; i < 2 * DMA_DESC; i++) {
        size_t got = 0;
        if (i2s_channel_read(s_rx, s_raw, sizeof(s_raw), &got, 0) != ESP_OK) {
            break;
        }
    }
    return ESP_OK;
}

esp_err_t audio_capture_read(int16_t *out, size_t frames, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    while (frames > 0) {
        size_t n = frames < DMA_FRAMES ? frames : DMA_FRAMES;
        size_t got = 0;   /* bytes; may be less than asked if the read times out */
        esp_err_t err = i2s_channel_read(s_rx, s_raw, n * 2 * sizeof(s_raw[0]), &got,
                                         timeout_ms);
        /* Each frame is two 32-bit words, left then right */
        size_t done = got / (2 * sizeof(s_raw[0]));   /* whole frames received */
        /* Word 2i is frame i's left slot. An arithmetic shift preserves
         * the sign, so negative samples stay negative. */
        for (size_t i = 0; i < done; i++) {
            out[i] = (int16_t)(s_raw[2 * i] >> CAPTURE_SHIFT);
        }
        out += done;
        frames -= done;
        if (err != ESP_OK) {
            return err;   /* usually ESP_ERR_TIMEOUT: no audio within timeout_ms */
        }
    }
    return ESP_OK;
}

esp_err_t audio_play_start(uint32_t sample_rate)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(sample_rate >= PLAY_RATE_MIN && sample_rate <= PLAY_RATE_MAX,
                        ESP_ERR_INVALID_ARG, TAG, "rate %lu out of range",
                        (unsigned long)sample_rate);

    /* The clock can only be changed while the channel is disabled, which it
     * is between playbacks */
    const i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_tx, &clk), TAG, "rate %lu",
                        (unsigned long)sample_rate);

    /* Clocks first: with nothing written yet, auto_clear makes the DMA send
     * silence. Only then is the amplifier woken, so it starts on a running,
     * silent signal. */
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "tx enable");
    gpio_set_level(BOARD_AMP_SD, 1);
    vTaskDelay(pdMS_TO_TICKS(AMP_WAKE_MS));
    return ESP_OK;
}

esp_err_t audio_play_write(const int16_t *samples, size_t frames, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    size_t bytes = frames * sizeof(samples[0]);
    size_t done = 0;
    esp_err_t err = i2s_channel_write(s_tx, samples, bytes, &done, timeout_ms);
    if (err == ESP_OK && done < bytes) {
        err = ESP_ERR_TIMEOUT;
    }
    return err;
}

esp_err_t audio_play_stop(void)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    /* Real audio may still be waiting in the DMA ring. Writing one ring's
     * worth of silence, plus one buffer, blocks until every earlier buffer
     * has been sent to the amplifier, so nothing is cut off. Worst case this
     * waits (DMA_DESC + 1) * DMA_FRAMES samples: 105 ms at 16 kHz. */
    esp_err_t err = ESP_OK;
    for (int i = 0; i < DMA_DESC + 1 && err == ESP_OK; i++) {
        size_t done = 0;
        /* 1000 ms: far longer than one buffer takes to play, so this fails
         * only if the hardware has stalled */
        err = i2s_channel_write(s_tx, s_silence, sizeof(s_silence), &done, 1000);
    }

    /* Amplifier off while the clock still carries silence, then stop the
     * clock. Done even if the flush failed, so the amplifier never stays on. */
    gpio_set_level(BOARD_AMP_SD, 0);
    esp_err_t dis = i2s_channel_disable(s_tx);
    return err != ESP_OK ? err : dis;
}
