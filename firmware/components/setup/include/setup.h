/*
 * Setup mode: the device becomes a WiFi hotspot of its own and serves a
 * settings page to a phone, for entering the settings without a USB cable.
 *
 * Setup is a way of starting up, not something switched on while the device
 * runs. Whatever wants it calls setup_restart(), which notes why and
 * restarts the chip; the boot sequence (main.c) asks setup_needed() and, if
 * setup is needed, calls setup_run() in place of the state machine, the
 * server link and WiFi. setup_run() ends in a restart too, back into normal
 * operation. So nothing that is running ever has to be stopped, and every
 * way out of setup leaves the chip exactly as a fresh start does.
 *
 * Ways in:
 *   - the settings needed to work are missing (setup_needed());
 *   - the `setup` console command;
 *   - holding the board's BOOT button for 3 s (setup_button_start());
 *   - WiFi settings that have never worked keep failing (main.c, from net);
 *   - the server refuses the device's token (main.c, from the state machine).
 *
 * Ways out, each a restart: the page's Save; holding BOOT again; or, when
 * setup was entered on purpose (the console or the button), 5 minutes with
 * nothing saved. Setup entered because the settings are missing or failed
 * has no deadline: going back to them would only fail again.
 *
 * Context: setup_restart() may be called from any task. The rest runs in
 * the boot sequence's task (main), which setup_run() keeps until its
 * restart.
 */
#pragma once

#include "esp_err.h"

/* Why setup was entered. Shown on the screen, so that a device that enters
 * it by itself says what it found wrong. */
typedef enum {
    SETUP_WHY_NONE,             /* not needed */
    SETUP_WHY_NO_WIFI,          /* no WiFi network set */
    SETUP_WHY_NO_TOKEN,         /* no device token set */
    SETUP_WHY_ASKED,            /* the `setup` console command */
    SETUP_WHY_BUTTON,           /* the BOOT button held */
    SETUP_WHY_WIFI_REFUSED,     /* the WiFi network refused the password */
    SETUP_WHY_WIFI_NOT_FOUND,   /* the WiFi network was not found */
    SETUP_WHY_TOKEN_REFUSED,    /* the server refused the device's token */
    SETUP_WHY_COUNT             /* number of reasons, not a reason */
} setup_why_t;

/* Restarts the chip into setup mode, remembering `why`. Does not return. */
void setup_restart(setup_why_t why);

/* Whether this boot is to run setup mode, and why: because the restart that
 * started it asked for it, or because the settings needed to work are
 * missing (app_config_complete()). SETUP_WHY_NONE if neither. A request
 * across the restart is used up by asking. Call once, after the settings
 * have been loaded. */
setup_why_t setup_needed(void);

/* Runs setup mode: raises the hotspot, shows how to reach the page on the
 * screen, and serves it. Never returns; it ends in a restart. Call from the
 * boot sequence in place of the state machine, the server link and WiFi,
 * after the settings, the console and the display have started. */
void setup_run(setup_why_t why);

/* Starts watching the BOOT button: held for 3 s in normal operation, it
 * restarts into setup mode. (setup_run() watches it too, and there holding
 * it leaves setup mode.) Call once, at boot, in normal operation only. */
esp_err_t setup_button_start(void);
