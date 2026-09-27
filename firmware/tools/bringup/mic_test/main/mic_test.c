/*
 * Bring-up test: INMP441 microphone on I2S0 (SCK=4, WS=5, SD=6).
 *
 * Reads audio continuously and prints, a few times a second, the level of
 * each stereo channel. This verifies the wiring and the L/R pin without any
 * audio tools: a working microphone shows a changing level on exactly one
 * channel, and noises in the room make it jump.
 *
 * Background: I2S is a serial audio bus. The chip drives two clocks, SCK (one
 * pulse per data bit) and WS (word select: low for the left channel, high for
 * the right), and the microphone sends data bits on SD. Each sample occupies
 * a 32-bit "slot"; the INMP441 puts its 24-bit sample in the top 24 bits. It
 * transmits in only one slot, chosen by its L/R pin: tied to ground = left.
 * The other slot is left undriven and reads as zero.
 *
 * This is a standalone ESP-IDF project, not part of the device firmware.
 */
#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"

#define SAMPLE_RATE 16000           /* samples per second per channel */
#define N_FRAMES    1600            /* 100 ms at 16 kHz; a frame = one left + one right sample */

/* Receive buffer: N_FRAMES frames, each a left and a right 32-bit slot.
 * Static rather than on the stack, because 12.8 KB would overflow the main
 * task's stack. */
static int32_t buf[N_FRAMES * 2];

void app_main(void)
{
    /* An I2S "channel" is one direction (receive here) of an I2S controller.
     * MASTER: the chip generates the clocks; the microphone follows them. */
    i2s_chan_handle_t rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx));   /* NULL: no transmit channel */

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        /* Philips (standard I2S) framing, 32-bit slots. Stereo, so both slots
         * are read and the silent one can be seen reading zero. */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,    /* the INMP441 needs no master clock */
            .bclk = GPIO_NUM_4,         /* SCK */
            .ws   = GPIO_NUM_5,
            .dout = I2S_GPIO_UNUSED,
            .din  = GPIO_NUM_6,         /* SD */
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx, &std_cfg));
    /* From here the I2S hardware fills internal buffers by DMA (direct memory
     * access: data moves into RAM without the CPU). i2s_channel_read() copies
     * out of them, blocking until enough data has arrived. */
    ESP_ERROR_CHECK(i2s_channel_enable(rx));

    size_t got;   /* bytes actually read */
    /* The microphone outputs garbage for its first few milliseconds */
    i2s_channel_read(rx, buf, sizeof(buf), &got, portMAX_DELAY);

    for (int n = 0; ; n++) {
        i2s_channel_read(rx, buf, sizeof(buf), &got, portMAX_DELAY);
        int frames = got / 8;   /* 8 bytes per frame: two 4-byte slots */

        /* Per channel (c = 0 left, 1 right): mean, minimum, maximum, then RMS. */
        double sum[2] = {0}, mean[2] = {0};
        int32_t mn[2] = {INT32_MAX, INT32_MAX}, mx[2] = {INT32_MIN, INT32_MIN};
        for (int i = 0; i < frames; i++) {
            for (int c = 0; c < 2; c++) {
                /* Arithmetic shift right by 8 moves the 24-bit sample down,
                 * keeping its sign. Range: -8388608 to +8388607. */
                int32_t v = buf[i * 2 + c] >> 8;
                mean[c] += v;
                if (v < mn[c]) mn[c] = v;
                if (v > mx[c]) mx[c] = v;
            }
        }
        for (int c = 0; c < 2; c++) mean[c] /= frames;
        /* RMS (root mean square) around the mean measures loudness. Removing
         * the mean first ignores any constant offset the microphone has. */
        for (int i = 0; i < frames; i++) {
            for (int c = 0; c < 2; c++) {
                double d = (buf[i * 2 + c] >> 8) - mean[c];
                sum[c] += d * d;
            }
        }
        printf("MIC %4d  L: rms=%8.0f min=%8ld max=%8ld | R: rms=%8.0f min=%8ld max=%8ld\n",
               n, sqrt(sum[0] / frames), (long)mn[0], (long)mx[0],
               sqrt(sum[1] / frames), (long)mn[1], (long)mx[1]);
        /* Slow the printing down. Audio arriving meanwhile is held in the
         * DMA buffers, and whatever overflows them is simply lost, which is
         * fine for a level meter. */
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}
