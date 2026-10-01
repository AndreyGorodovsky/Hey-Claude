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
 *   2. Audio          - microphone started (it runs from here on), amplifier
 *                       switched off. Early, because until then the
 *                       amplifier's on/off pin floats.
 *   3. Event loop     - delivers system events; the state and WiFi use it.
 *   4. State          - the device state, starting at BOOT.
 *   5. Display        - backlight off at once (its pin floats until then),
 *                       then the panel starts in the background and shows
 *                       the state.
 *   6. Listening      - the capture task starts filling the audio ring, and
 *                       wake-word detection starts reading it. Needs audio
 *                       and the event loop, which detections are posted to.
 *   7. NVS            - flash-backed key-value storage that holds the settings.
 *   8. Network stack  - TCP/IP layer, needed by WiFi.
 *   9. Settings       - read from NVS into memory, once.
 *  10. Console        - serial command line for settings and tests.
 *  11. WiFi           - only if a network has been configured. The state
 *                       moves to CONNECTING, or to SETUP if there are no
 *                       usable WiFi settings.
 *
 * The server connection is added in a later stage (see STATUS.md).
 */
#include "app_config.h"
#include "app_state.h"
#include "audio.h"
#include "audio_ring.h"
#include "console.h"
#include "display.h"
#include "net.h"
#include "wake_standin.h"
#include "wakeword.h"

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
 * line: the USB power meter shows only averages and misses short dips
 * (KNOWN-ISSUES R9), so a brownout reset in this log is the decisive evidence
 * of a weak supply or cable.
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

    /* Not ESP_ERROR_CHECK: without audio the device can still join WiFi and
     * take console commands, which is more useful for diagnosing the fault
     * than a restart loop. The audio functions refuse to run afterwards. */
    esp_err_t audio_err = audio_init();
    if (audio_err != ESP_OK) {
        ESP_LOGE(TAG, "audio not available: %s", esp_err_to_name(audio_err));
    }

    /* The default event loop is a background task that delivers system events
     * (WiFi connected, IP address received, state changed, ...) to handlers
     * registered for them. Everything that posts events needs it first. */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(app_state_init());
    /* Not ESP_ERROR_CHECK, for the same reason as audio: the device is still
     * usable, and diagnosable, with a dark screen */
    esp_err_t err = display_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display not available: %s", esp_err_to_name(err));
    }

    /* Listening needs a working microphone. Failures are logged and not
     * fatal, like audio's: the rest of the device stays usable. */
    if (audio_err == ESP_OK) {
        err = audio_ring_start();
        if (err == ESP_OK) {
            err = wakeword_start();
        }
        if (err == ESP_OK) {
            err = wake_standin_start();
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wake word not available: %s", esp_err_to_name(err));
        }
    }

    init_nvs();
    /* esp_netif is ESP-IDF's layer over the TCP/IP stack; WiFi attaches to it. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(app_config_load());
    /* The console runs in its own task from here on, independently of app_main */
    ESP_ERROR_CHECK(console_start());

    const app_config_t *cfg = app_config_get();
    if (!app_config_has_wifi()) {
        /* First boot, or settings erased: wait for them to be entered over serial */
        ESP_LOGW(TAG, "WiFi not configured. Enter: config set wifi_ssid \"<name>\", "
                      "config set wifi_pass \"<password>\", then reboot");
        app_state_set(APP_STATE_SETUP, "WiFi not set up");
        return;
    }

    /* Not ESP_ERROR_CHECK: a failure here comes from stored settings (a
     * password the driver rejects, for example). Aborting would restart the
     * chip into the same failure forever, before the console could be used to
     * fix it. Logging and carrying on keeps the console available. */
    err = net_start(cfg->wifi_ssid, cfg->wifi_pass, cfg->device_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi not started: %s. Check the settings with: config show",
                 esp_err_to_name(err));
        app_state_set(APP_STATE_SETUP, "WiFi settings rejected");
        return;
    }
    /* Boot is complete. The state stays CONNECTING until the server
     * connection, added in stage 6, moves it on. */
    app_state_set(APP_STATE_CONNECTING, NULL);
    /* app_main returns here. WiFi keeps connecting in the background, driven
     * by events, and the console task keeps accepting commands. */
}
