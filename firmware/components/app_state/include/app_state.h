/*
 * The device's state: what it is doing right now, as the user sees it.
 *
 * One value from the state machine in ARCHITECTURE.md, plus, for SETUP and
 * ERROR, a short detail text saying why. This component only stores the
 * state and announces changes; it does not decide them.
 *
 * One writer. Transitions belong to the state machine that stage 6 builds:
 * it alone calls app_state_set(), and other components (wake-word
 * detection, the server connection) report what happened to it, never set
 * states themselves. Otherwise two tasks setting states at the same moment
 * would leave the screen on whichever came last. Until stage 6, main.c's
 * boot outcome and the console's `state` command stand in for that writer.
 *
 * Changes are announced as APP_STATE_EVENT / APP_STATE_CHANGED on the default
 * event loop, the same way the WiFi driver announces its events. The event
 * data is an app_state_event_t. Subscribers, such as the display, register a
 * handler with esp_event_handler_register().
 *
 * Threading: every function may be called from any task, after
 * app_state_init(). Changes are announced in the order they were made.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_event.h"

typedef enum {
    APP_STATE_BOOT,         /* powered on, starting up */
    APP_STATE_SETUP,        /* cannot continue until settings are entered or
                               corrected; unlike ERROR, retrying cannot help */
    APP_STATE_CONNECTING,   /* joining WiFi and the server */
    APP_STATE_IDLE,         /* waiting for the wake word */
    APP_STATE_CAPTURING,    /* recording the request */
    APP_STATE_THINKING,     /* waiting for the reply */
    APP_STATE_SPEAKING,     /* playing the reply */
    APP_STATE_ERROR,        /* a transport or server failure; retried */
    APP_STATE_COUNT         /* number of states, not a state */
} app_state_t;

/* Longest detail text, including the terminating NUL; longer text is cut.
 * The screen fits about 38 characters on the detail line. */
#define APP_STATE_DETAIL_MAX    40

ESP_EVENT_DECLARE_BASE(APP_STATE_EVENT);

enum {
    APP_STATE_CHANGED,      /* event data: app_state_event_t */
};

typedef struct {
    app_state_t state;
    char detail[APP_STATE_DETAIL_MAX];  /* empty string when none was given */
} app_state_event_t;

/* Sets the state to APP_STATE_BOOT. Call once, after the default event loop
 * exists and before any other function here. */
esp_err_t app_state_init(void);

/* Changes the state and announces it. `detail` is for SETUP and ERROR and
 * may be NULL; it is ignored for other states. Setting the current state
 * again with a different detail is announced too; setting exactly what is
 * already there is not. If the announcement fails, the state is left as it
 * was, so a retry is not mistaken for a repeat. */
esp_err_t app_state_set(app_state_t state, const char *detail);

/* Copies the current state and its detail into `out`. Before
 * app_state_init(), reports BOOT. */
void app_state_get(app_state_event_t *out);

/* Lower-case name of a state, as used by the console ("idle"), or "?" */
const char *app_state_name(app_state_t state);

/* Looks a state up by its lower-case name; false if there is none */
bool app_state_from_name(const char *name, app_state_t *out);
