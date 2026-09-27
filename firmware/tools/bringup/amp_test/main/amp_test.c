/*
 * Bring-up test: MAX98357A amplifier on I2S1 (BCLK=15, LRC=16, DIN=7, SD=17).
 *
 * Plays a 440 Hz tone (the note A) for 0.5 s every 2 s, cycling through three
 * volumes: 5 %, 10 % and 20 % of full scale. Three clearly different volumes
 * show the audio data is arriving intact; the loudest also draws the most
 * current, so a brownout reset during it points at the supply.
 *
 * Background: the chip sends audio to the amplifier over I2S (see mic_test.c
 * for the bus itself). The amplifier's SD pin is a separate on/off input:
 * low turns it off, and when driven high from a 3.3 V GPIO it plays the left
 * channel. The tone is written to both channels so either choice works.
 *
 * This is a standalone ESP-IDF project, not part of the device firmware.
 */
#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_system.h"

#define PIN_AMP_SD  GPIO_NUM_17
#define RATE        16000           /* samples per second per channel */
#define TONE_HZ     440.0
#define FRAMES      160             /* 10 ms per write at 16 kHz */

/* One write's worth of audio: FRAMES frames of left + right 16-bit samples */
static int16_t buf[FRAMES * 2];

void app_main(void)
{
    /* 1 = power-on. A brownout shows up here as 15 (ESP_RST_BROWNOUT). */
    printf("AMP reset reason %d\n", esp_reset_reason());

    /* SD is a plain digital output, not part of I2S */
    gpio_config_t sd = { .pin_bit_mask = 1ULL << PIN_AMP_SD, .mode = GPIO_MODE_OUTPUT };
    ESP_ERROR_CHECK(gpio_config(&sd));
    gpio_set_level(PIN_AMP_SD, 1);      /* amplifier on */

    /* Transmit channel on I2S controller 1; the chip generates the clocks */
    i2s_chan_handle_t tx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    /* If the program ever falls behind, send silence rather than repeating
     * the last buffer, which would sound like a buzz */
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx, NULL));   /* NULL: no receive channel */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,    /* the MAX98357A needs no master clock */
            .bclk = GPIO_NUM_15,
            .ws   = GPIO_NUM_16,        /* LRC on the amplifier's silkscreen */
            .dout = GPIO_NUM_7,         /* DIN on the amplifier */
            .din  = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx));

    const int levels[] = {5, 10, 20};   /* percent of full scale */
    double phase = 0;                   /* position in the sine wave, radians */
    size_t w;                           /* bytes written; unused */
    for (int n = 0; ; n++) {
        int pct = levels[n % 3];
        double amp = 32767.0 * pct / 100.0;     /* 32767 = largest 16-bit sample */
        printf("AMP beep %d at %d%%\n", n, pct);

        /* 0.5 s of tone: 50 writes of 10 ms. i2s_channel_write blocks until
         * there is room in the DMA buffers, which paces the loop in real time. */
        for (int blk = 0; blk < 50; blk++) {
            for (int i = 0; i < FRAMES; i++) {
                int16_t v = (int16_t)(amp * sin(phase));
                /* Advance by one sample's worth of the tone's cycle, and wrap
                 * so the number stays small and precise */
                phase += 2 * M_PI * TONE_HZ / RATE;
                if (phase > 2 * M_PI) {
                    phase -= 2 * M_PI;
                }
                buf[i * 2] = buf[i * 2 + 1] = v;         /* same on both channels */
            }
            i2s_channel_write(tx, buf, sizeof(buf), &w, portMAX_DELAY);
        }

        /* 1.5 s of silence: 150 writes of 10 ms of zeros */
        for (int i = 0; i < FRAMES * 2; i++) buf[i] = 0;
        for (int blk = 0; blk < 150; blk++)
            i2s_channel_write(tx, buf, sizeof(buf), &w, portMAX_DELAY);
    }
}
