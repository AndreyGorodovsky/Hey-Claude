/*
 * Hey Claude device firmware: entry point.
 *
 * ESP-IDF starts FreeRTOS (the real-time operating system that schedules
 * tasks on the chip's two cores) and then calls app_main() from a task named
 * "main". Everything the device does is started from here. When app_main()
 * returns, only that task ends; tasks and services it started keep running.
 *
 * Start-up order matters, because later steps depend on earlier ones:
 *   1. Boot banner    - diagnostic summary, printed first so it survives
 *                       any later failure.
 *   2. NVS            - flash-backed key-value storage that holds the settings.
 *   3. Network stack  - TCP/IP layer and the default event loop, needed by WiFi.
 *   4. Settings       - read from NVS into memory, once.
 *   5. Console        - serial command line for entering settings.
 *   6. WiFi           - only if a network has been configured.
 *
 * Stage 1 stops here. Audio, display and the server connection are added in
 * later stages (see STATUS.md).
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

/* Every log line from this file is prefixed with this tag, e.g. "I (905) main: ..."
 * The number in brackets is milliseconds since boot. */
static const char *TAG = "main";

/* Human-readable name for the reason the chip last restarted. */
static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:    return "power-on";
    case ESP_RST_EXT:        return "external pin";         /* RST button or EN pin */
    case ESP_RST_SW:         return "software restart";     /* esp_restart(), e.g. the reboot command */
    case ESP_RST_PANIC:      return "panic";                /* crash; see the core dump */
    case ESP_RST_INT_WDT:    return "interrupt watchdog";   /* an interrupt handler ran too long */
    case ESP_RST_TASK_WDT:   return "task watchdog";        /* a task hogged a CPU core */
    case ESP_RST_WDT:        return "other watchdog";
    case ESP_RST_DEEPSLEEP:  return "deep sleep wake";
    case ESP_RST_BROWNOUT:   return "brownout";             /* supply voltage dropped too low */
    case ESP_RST_PWR_GLITCH: return "power glitch";         /* brief supply disturbance */
    case ESP_RST_CPU_LOCKUP: return "CPU lockup";
    case ESP_RST_SDIO:       return "SDIO";
    case ESP_RST_USB:        return "USB";                  /* reset requested over native USB */
    case ESP_RST_JTAG:       return "JTAG";                 /* reset requested by a debugger */
    default:                 return "unknown";
    }
}

/*
 * Prints a short summary at every boot. The reset reason is the most useful
 * line: the project has no power meter yet (KNOWN-ISSUES R9), so a brownout
 * reset in this log is the main evidence of a weak supply or cable.
 */
static void log_banner(void)
{
    /* Project name and version are embedded in the binary at build time; the
     * version comes from `git describe`, so it identifies the exact commit. */
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);  /* NULL = the chip's main flash */
    esp_reset_reason_t reason = esp_reset_reason();

    ESP_LOGI(TAG, "%s %s, ESP-IDF %s", app->project_name, app->version, app->idf_ver);
    /* chip.revision is encoded as major * 100 + minor, e.g. 2 means v0.2 */
    ESP_LOGI(TAG, "chip rev v%d.%d, %d cores, flash %lu MB",
             chip.revision / 100, chip.revision % 100, chip.cores,
             (unsigned long)(flash_size >> 20));  /* bytes to MB */
    /* The chip has two kinds of RAM. Internal RAM (~512 KB) is fast and is
     * the only kind DMA and interrupt code can use. PSRAM (8 MB here) is a
     * separate RAM chip inside the same package: larger but slower, used
     * later for audio buffers and the display frame buffer. If the PSRAM
     * figure reads 0, PSRAM failed to start. */
    ESP_LOGI(TAG, "free heap: internal %u KB, PSRAM %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10),  /* bytes to KB */
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10));
    if (reason == ESP_RST_BROWNOUT || reason == ESP_RST_PWR_GLITCH) {
        /* Logged as an error so it stands out; see KNOWN-ISSUES R2 */
        ESP_LOGE(TAG, "reset reason: %s. Suspect the supply or cable first (R2)",
                 reset_reason_str(reason));
    } else {
        ESP_LOGI(TAG, "reset reason: %s", reset_reason_str(reason));
    }
}

/*
 * Prepares NVS (non-volatile storage): a small key-value store kept in its
 * own flash partition (see partitions.csv). It survives power loss and
 * reflashing the application, which is why settings live there.
 */
static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    /* These two errors mean the partition cannot be used as it is: it is full,
     * or it was written by an incompatible NVS version. The only way out is to
     * erase it, which loses stored settings; they must then be re-entered. */
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS unusable (%s), erasing", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    /* ESP_ERROR_CHECK aborts and restarts the chip if the result is not ESP_OK.
     * It is used only for failures that leave the firmware unable to run at all. */
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    log_banner();

    init_nvs();
    /* esp_netif is ESP-IDF's layer over the TCP/IP stack; WiFi attaches to it. */
    ESP_ERROR_CHECK(esp_netif_init());
    /* The default event loop is a background task that delivers system events
     * (WiFi connected, IP address received, ...) to handlers registered for
     * them. The WiFi driver posts its events here, so it must exist first. */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(app_config_load());
    /* The console runs in its own task from here on, independently of app_main */
    ESP_ERROR_CHECK(console_start());

    const app_config_t *cfg = app_config_get();
    if (!app_config_has_wifi()) {
        /* First boot, or settings erased: wait for them to be entered over serial */
        ESP_LOGW(TAG, "WiFi not configured. Enter: config set wifi_ssid \"<name>\", "
                      "config set wifi_pass \"<password>\", then reboot");
        return;
    }

    /* Not ESP_ERROR_CHECK: a failure here comes from stored settings (a
     * password the driver rejects, for example). Aborting would restart the
     * chip into the same failure forever, before the console could be used to
     * fix it. Logging and carrying on keeps the console available. */
    esp_err_t err = net_start(cfg->wifi_ssid, cfg->wifi_pass, cfg->device_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi not started: %s. Check the settings with: config show",
                 esp_err_to_name(err));
    }
    /* app_main returns here. WiFi keeps connecting in the background, driven
     * by events, and the console task keeps accepting commands. */
}
