/*
 * Setup mode: how it is asked for across a restart, and the loop that runs
 * it. See setup.h for the design; provision.c has the hotspot and the page.
 *
 * In setup mode this file's loop is the device state's only writer: the
 * state machine, the usual writer, is not started on a boot that runs
 * setup mode (main.c).
 *
 * Context: setup_restart() runs in whichever task asks for setup. Everything
 * else runs in the boot sequence's task (main), which setup_run() keeps
 * until the restart that ends it.
 */
#include "setup.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "app_config.h"
#include "app_state.h"
#include "button.h"
#include "display.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "provision.h"

static const char *TAG = "setup";

/* How long the hotspot stays up with nothing saved, when setup was entered
 * on purpose: long enough to find a phone, join a network and fill in a
 * form; short enough that a press nobody followed up puts the device back
 * to work by itself. Nothing is lost when it runs out: setup deletes
 * nothing on the way in. */
#define TIMEOUT_MS          (5 * 60 * 1000)
/* How often the loop looks at the time and at whether the page saved */
#define TICK_MS             1000
/* Pause after a save before restarting, for the page's last bytes to reach
 * the phone */
#define SAVED_PAUSE_MS      1000
/* Pause before restarting after the hotspot failed to start, long enough to
 * read the screen. Longer when the device has no settings to go back to: it
 * would only come straight back here and fail again, and a slow retry keeps
 * the message readable. */
#define FAILED_PAUSE_MS             3000
#define FAILED_PAUSE_NOTHING_MS     30000

/* What the screen says for each reason, under "Setup needed". The detail
 * line fits about 38 characters (app_state.h). */
static const char *const WHY_TEXT[SETUP_WHY_COUNT] = {
    [SETUP_WHY_NONE]            = "",
    [SETUP_WHY_NO_WIFI]         = "WiFi not set up",
    [SETUP_WHY_NO_TOKEN]        = "Device token not set up",
    [SETUP_WHY_ASKED]           = "Asked for at the console",
    [SETUP_WHY_BUTTON]          = "BOOT button held",
    [SETUP_WHY_WIFI_REFUSED]    = "WiFi password refused",
    [SETUP_WHY_WIFI_NOT_FOUND]  = "WiFi network not found",
    [SETUP_WHY_TOKEN_REFUSED]   = "Server refused this device",
};

/* ---- Asking across a restart ----------------------------------------------
 *
 * RTC_NOINIT_ATTR places a variable in the chip's RTC memory, which keeps
 * its contents through a restart (but not a loss of power), and tells the
 * start-up code to leave it alone. After power-on it holds whatever the
 * memory came up with, which is what the magic number is for: only a value
 * written here, with the magic beside it, counts. The brownout guard keeps
 * its count the same way (main/brownout_guard.c). */

#define MAGIC               0x5E7B0057u
static RTC_NOINIT_ATTR uint32_t s_magic;
static RTC_NOINIT_ATTR uint32_t s_why;

static bool on_scale(uint32_t why)
{
    return why > SETUP_WHY_NONE && why < SETUP_WHY_COUNT;
}

void setup_restart(setup_why_t why)
{
    ESP_LOGW(TAG, "restarting into setup: %s", WHY_TEXT[on_scale(why) ? why : 0]);
    s_why = why;
    s_magic = MAGIC;
    esp_restart();
}

setup_why_t setup_needed(void)
{
    /* Only a restart this firmware made can have asked */
    bool asked = s_magic == MAGIC && on_scale(s_why) && esp_reset_reason() == ESP_RST_SW;
    s_magic = 0;            /* used up */
    if (asked) {
        return (setup_why_t)s_why;
    }
    if (!app_config_has_wifi()) {
        return SETUP_WHY_NO_WIFI;       /* first boot, or settings erased */
    }
    if (!app_config_complete()) {
        return SETUP_WHY_NO_TOKEN;
    }
    return SETUP_WHY_NONE;
}

/* ---- The BOOT button -------------------------------------------------- */

static void on_hold_normal(void)
{
    setup_restart(SETUP_WHY_BUTTON);
}

esp_err_t setup_button_start(void)
{
    return button_watch(on_hold_normal);
}

/* In setup mode the button leaves it, by a plain restart */
static void on_hold_in_setup(void)
{
    ESP_LOGI(TAG, "BOOT button held: leaving setup");
    esp_restart();
}

/* ---- The loop ---------------------------------------------------------- */

void setup_run(setup_why_t why)
{
    if (!on_scale(why)) {
        why = SETUP_WHY_ASKED;
    }
    ESP_LOGI(TAG, "setup mode: %s", WHY_TEXT[why]);
    app_state_set(APP_STATE_SETUP, "Starting the hotspot");

    /* A deadline only when setup was entered on purpose, with settings that
     * may well work. Entered because they are missing or failed, a deadline
     * would only restart into them, fail, and come back here, with a new
     * hotspot password each time. */
    bool deadline = why == SETUP_WHY_ASKED || why == SETUP_WHY_BUTTON;

    provision_info_t info;
    esp_err_t err = provision_start(&info);
    if (err != ESP_OK) {
        app_state_set(APP_STATE_SETUP, "Hotspot failed; restarting");
        vTaskDelay(pdMS_TO_TICKS(app_config_complete() ? FAILED_PAUSE_MS
                                                       : FAILED_PAUSE_NOTHING_MS));
        esp_restart();
    }

    int64_t closes_at_us = deadline ? esp_timer_get_time() + (int64_t)TIMEOUT_MS * 1000 : 0;
    display_setup_t screen = { .closes_at_us = closes_at_us };
    strlcpy(screen.ssid, info.ssid, sizeof(screen.ssid));
    strlcpy(screen.password, info.password, sizeof(screen.password));
    strlcpy(screen.address, info.address, sizeof(screen.address));
    display_show_setup(&screen);
    /* The state is set once; the screen counts the minutes by itself */
    app_state_set(APP_STATE_SETUP, WHY_TEXT[why]);

    if (button_watch(on_hold_in_setup) != ESP_OK) {
        ESP_LOGW(TAG, "the BOOT button is not watched; setup ends by saving%s",
                 deadline ? " or timing out" : "");
    }

    for (;;) {
        if (provision_saved()) {
            ESP_LOGI(TAG, "settings saved: restarting");
            vTaskDelay(pdMS_TO_TICKS(SAVED_PAUSE_MS));
            break;
        }
        /* Not while a form is being received or stored: a restart then
         * could lose it, or leave some of its values stored and not others */
        if (deadline && esp_timer_get_time() >= closes_at_us && !provision_busy()) {
            ESP_LOGI(TAG, "nothing saved in %d min: restarting", TIMEOUT_MS / 60000);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
    esp_restart();
}
