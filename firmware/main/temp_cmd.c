/*
 * Console command for the chip's temperature.
 *
 *   temp        print the temperature inside the ESP32-S3, in degrees Celsius
 *
 * The ESP32-S3 has a temperature sensor built into the chip itself. It
 * measures the silicon, which runs hotter than the chip's surface and much
 * hotter than the air around it, so the figure is higher than a finger or a
 * thermometer on the board would suggest. The chip is rated to work with
 * the air around it at up to 85 degrees; the sensor is accurate to a few
 * degrees.
 *
 * A diagnostic aid, kept apart so that it can be taken out again. Removing
 * it means deleting this file and temp_cmd.h, and four lines elsewhere:
 * the #include and the temp_cmd_register() call in console.c, and the
 * "temp_cmd.c" and esp_driver_tsens entries in this directory's
 * CMakeLists.txt.
 *
 * Context: the handler runs in the console task.
 */
#include "temp_cmd.h"

#include <stdio.h>
#include "driver/temperature_sensor.h"
#include "esp_check.h"
#include "esp_console.h"

static const char *TAG = "temp_cmd";

/* The sensor has several measuring ranges and is most accurate when told
 * which to expect. This one, 20 to 100 degrees, covers a chip at work. */
#define RANGE_MIN_C     20
#define RANGE_MAX_C     100

/* Created on first use, then kept */
static temperature_sensor_handle_t s_sensor;

static esp_err_t sensor_start(void)
{
    if (s_sensor != NULL) {
        return ESP_OK;
    }
    temperature_sensor_config_t config =
        TEMPERATURE_SENSOR_CONFIG_DEFAULT(RANGE_MIN_C, RANGE_MAX_C);
    temperature_sensor_handle_t sensor = NULL;
    ESP_RETURN_ON_ERROR(temperature_sensor_install(&config, &sensor), TAG, "install");
    esp_err_t err = temperature_sensor_enable(sensor);
    if (err != ESP_OK) {
        temperature_sensor_uninstall(sensor);
        return err;
    }
    s_sensor = sensor;
    return ESP_OK;
}

static int cmd_temp(int argc, char **argv)
{
    float celsius = 0;
    esp_err_t err = sensor_start();
    if (err == ESP_OK) {
        err = temperature_sensor_get_celsius(s_sensor, &celsius);
    }
    if (err != ESP_OK) {
        printf("temperature not available: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("chip temperature: %.1f C (inside the chip; its surface is cooler)\n", celsius);
    return 0;
}

esp_err_t temp_cmd_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "temp",
        .help = "Show the temperature inside the chip",
        .func = cmd_temp,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmd), TAG, "temp");
    return ESP_OK;
}
