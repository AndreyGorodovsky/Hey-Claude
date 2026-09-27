/*
 * Hey Claude device firmware. Stage 1: boot, configuration, WiFi, logging.
 */
#include "app_config.h"
#include "console.h"
#include "net.h"

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"

static const char *TAG = "main";

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software restart";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_CPU_LOCKUP: return "CPU lockup";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB";
    case ESP_RST_JTAG:      return "JTAG";
    default:                return "unknown";
    }
}

static void log_banner(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    esp_reset_reason_t reason = esp_reset_reason();

    ESP_LOGI(TAG, "%s %s, ESP-IDF %s", app->project_name, app->version, app->idf_ver);
    ESP_LOGI(TAG, "chip rev v%d.%d, %d cores, flash %lu MB",
             chip.revision / 100, chip.revision % 100, chip.cores,
             (unsigned long)(flash_size >> 20));
    ESP_LOGI(TAG, "free heap: internal %u KB, PSRAM %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10));
    if (reason == ESP_RST_BROWNOUT || reason == ESP_RST_PWR_GLITCH) {
        ESP_LOGE(TAG, "reset reason: %s. Suspect the supply or cable first (R2)",
                 reset_reason_str(reason));
    } else {
        ESP_LOGI(TAG, "reset reason: %s", reset_reason_str(reason));
    }
}

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Layout change or full partition: stored settings are lost and must be re-entered */
        ESP_LOGW(TAG, "NVS unusable (%s), erasing", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    log_banner();

    init_nvs();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(app_config_load());
    ESP_ERROR_CHECK(console_start());

    const app_config_t *cfg = app_config_get();
    if (!app_config_has_wifi()) {
        ESP_LOGW(TAG, "WiFi not configured. Enter: config set wifi_ssid \"<name>\", "
                      "config set wifi_pass \"<password>\", then reboot");
        return;
    }
    /* A failure here comes from stored settings; aborting would boot-loop past
     * the console that can correct them */
    esp_err_t err = net_start(cfg->wifi_ssid, cfg->wifi_pass, cfg->device_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi not started: %s. Check the settings with: config show",
                 esp_err_to_name(err));
    }
}
