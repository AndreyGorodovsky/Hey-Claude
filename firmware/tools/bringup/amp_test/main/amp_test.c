/* Throwaway stage-0 check: MAX98357A on I2S1 (BCLK=15, LRC=16, DIN=7, SD=17).
 * 440 Hz beeps at 5 %, 10 % and 20 % of full scale, 0.5 s on, 1.5 s off. */
#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_system.h"

#define RATE  16000
#define FRAMES 160                     /* 10 ms */

static int16_t buf[FRAMES * 2];

void app_main(void)
{
    printf("AMP reset reason %d\n", esp_reset_reason());

    gpio_config_t sd = { .pin_bit_mask = 1ULL << 17, .mode = GPIO_MODE_OUTPUT };
    ESP_ERROR_CHECK(gpio_config(&sd));
    gpio_set_level(17, 1);             /* amplifier enabled */

    i2s_chan_handle_t tx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx, NULL));
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, .bclk = GPIO_NUM_15, .ws = GPIO_NUM_16,
            .dout = GPIO_NUM_7, .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx));

    const int levels[] = {5, 10, 20};
    double phase = 0;
    size_t w;
    for (int n = 0; ; n++) {
        int pct = levels[n % 3];
        double amp = 32767.0 * pct / 100.0;
        printf("AMP beep %d at %d%%\n", n, pct);
        for (int blk = 0; blk < 50; blk++) {            /* 0.5 s tone */
            for (int i = 0; i < FRAMES; i++) {
                int16_t v = (int16_t)(amp * sin(phase));
                phase += 2 * M_PI * 440.0 / RATE;
                buf[i * 2] = buf[i * 2 + 1] = v;         /* same on both slots */
            }
            i2s_channel_write(tx, buf, sizeof(buf), &w, portMAX_DELAY);
        }
        for (int i = 0; i < FRAMES * 2; i++) buf[i] = 0;
        for (int blk = 0; blk < 150; blk++)              /* 1.5 s silence */
            i2s_channel_write(tx, buf, sizeof(buf), &w, portMAX_DELAY);
    }
}
