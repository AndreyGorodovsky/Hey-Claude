/*
 * A temporary reaction to the wake word, standing in for the state machine
 * that stage 6 builds.
 *
 * When the wake word is heard while the device is IDLE, the state moves to
 * CAPTURING, and back to IDLE HOLD_MS later. That makes a detection visible
 * on the screen, with no server to send the request to yet. Outside IDLE,
 * detections are ignored, as the real state machine will ignore them: the
 * device is busy, or not ready to listen. Without a server the device never
 * reaches IDLE by itself, so it is set from the console first:
 * `state set idle`.
 *
 * Stage 6 deletes this file: the state machine receives WAKEWORD_DETECTED
 * itself, and is then the only code that sets the state (app_state.h).
 *
 * Context. The detection handler runs in the default event loop's task,
 * which delivers WAKEWORD_EVENT. The return to IDLE runs in the esp_timer
 * task, ESP-IDF's task for software timer callbacks. Both are short.
 */
#include "wake_standin.h"

#include "app_state.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "wakeword.h"

static const char *TAG = "wake_standin";

/* How long CAPTURING is shown after a detection, in milliseconds. Long
 * enough to notice; the real capture lasts until the server ends it. */
#define HOLD_MS     3000

static esp_timer_handle_t s_hold_timer;

/* Runs in the esp_timer task, HOLD_MS after a detection */
static void hold_done(void *arg)
{
    app_state_event_t now;
    app_state_get(&now);
    /* Only if nothing else changed the state meanwhile, such as a `state
     * set` typed during the hold */
    if (now.state == APP_STATE_CAPTURING) {
        app_state_set(APP_STATE_IDLE, NULL);
    }
}

/* Runs in the event loop's task for each WAKEWORD_DETECTED */
static void on_wake(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    app_state_event_t now;
    app_state_get(&now);
    if (now.state != APP_STATE_IDLE) {
        ESP_LOGI(TAG, "wake word ignored: state is %s", app_state_name(now.state));
        return;
    }
    if (app_state_set(APP_STATE_CAPTURING, NULL) == ESP_OK) {
        /* The timer can still be running if the console set IDLE during a
         * hold; stopping it first gives this detection its full hold. The
         * stop fails harmlessly when the timer is not running. esp_timer
         * counts in microseconds. */
        esp_timer_stop(s_hold_timer);
        esp_timer_start_once(s_hold_timer, (uint64_t)HOLD_MS * 1000);
    }
}

esp_err_t wake_standin_start(void)
{
    const esp_timer_create_args_t timer = {
        .callback = hold_done,
        .name = "wake_hold",    /* shown in esp_timer diagnostics */
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer, &s_hold_timer), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WAKEWORD_EVENT, WAKEWORD_DETECTED,
                                                   on_wake, NULL),
                        TAG, "handler");
    return ESP_OK;
}
