/* Throwaway stage-0 check: read INMP441 on I2S0 (SCK=4, WS=5, SD=6) in stereo
 * and print per-channel level, so wiring and the L/R strap can be verified. */
#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"

#define N_FRAMES 1600   /* 100 ms at 16 kHz */

static int32_t buf[N_FRAMES * 2];

void app_main(void)
{
    i2s_chan_handle_t rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, .bclk = GPIO_NUM_4, .ws = GPIO_NUM_5,
            .dout = I2S_GPIO_UNUSED, .din = GPIO_NUM_6,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx));

    size_t got;
    i2s_channel_read(rx, buf, sizeof(buf), &got, portMAX_DELAY);   /* discard start-up */

    for (int n = 0; ; n++) {
        i2s_channel_read(rx, buf, sizeof(buf), &got, portMAX_DELAY);
        int frames = got / 8;
        double sum[2] = {0}, mean[2] = {0};
        int32_t mn[2] = {INT32_MAX, INT32_MAX}, mx[2] = {INT32_MIN, INT32_MIN};
        for (int i = 0; i < frames; i++) {
            for (int c = 0; c < 2; c++) {
                int32_t v = buf[i * 2 + c] >> 8;            /* 24-bit sample */
                mean[c] += v;
                if (v < mn[c]) mn[c] = v;
                if (v > mx[c]) mx[c] = v;
            }
        }
        for (int c = 0; c < 2; c++) mean[c] /= frames;
        for (int i = 0; i < frames; i++) {
            for (int c = 0; c < 2; c++) {
                double d = (buf[i * 2 + c] >> 8) - mean[c];
                sum[c] += d * d;
            }
        }
        printf("MIC %4d  L: rms=%8.0f min=%8ld max=%8ld | R: rms=%8.0f min=%8ld max=%8ld\n",
               n, sqrt(sum[0] / frames), (long)mn[0], (long)mx[0],
               sqrt(sum[1] / frames), (long)mn[1], (long)mx[1]);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}
