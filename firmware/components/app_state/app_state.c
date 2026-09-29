/*
 * The device's state, stored and announced. See app_state.h.
 *
 * ESP-IDF's event loop is a background task that delivers "events" (a base
 * name, an ID and a small block of data) to every handler registered for
 * them. esp_event_post() copies the data, so the caller's copy can be reused
 * at once. Posting is how the WiFi driver already reports its progress; the
 * state uses the same mechanism so that subscribers need no new pattern.
 *
 * Context: functions run in whatever task calls them. A mutex ("mutual
 * exclusion" lock: only one task can hold it at a time, and others wait)
 * covers both storing the state and posting the event, so two tasks changing
 * the state at the same moment cannot have their announcements arrive in the
 * opposite order to their changes.
 */
#include "app_state.h"

#include <string.h>
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "app_state";

ESP_EVENT_DEFINE_BASE(APP_STATE_EVENT);

/* How long app_state_set() waits for room in the event loop's queue. The
 * queue holds 32 events and drains in microseconds, so waiting at all means
 * something is badly stuck; failing then is better than hanging the caller. */
#define POST_TIMEOUT_MS     100

/* Adding a state means adding it here and to the display's table in
 * display_ui.c; this check fails the build as a reminder */
_Static_assert(APP_STATE_COUNT == 8, "state added or removed: update s_names "
               "here and s_views in display_ui.c, then this count");

static const char *const s_names[APP_STATE_COUNT] = {
    [APP_STATE_BOOT]       = "boot",
    [APP_STATE_SETUP]      = "setup",
    [APP_STATE_CONNECTING] = "connecting",
    [APP_STATE_IDLE]       = "idle",
    [APP_STATE_CAPTURING]  = "capturing",
    [APP_STATE_THINKING]   = "thinking",
    [APP_STATE_SPEAKING]   = "speaking",
    [APP_STATE_ERROR]      = "error",
};

static SemaphoreHandle_t s_lock;
/* The current state and detail, written only while holding s_lock */
static app_state_event_t s_current;

esp_err_t app_state_init(void)
{
    ESP_RETURN_ON_FALSE(s_lock == NULL, ESP_ERR_INVALID_STATE, TAG, "already initialised");
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "mutex");
    s_current.state = APP_STATE_BOOT;
    s_current.detail[0] = '\0';
    return ESP_OK;
}

esp_err_t app_state_set(app_state_t state, const char *detail)
{
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(state < APP_STATE_COUNT, ESP_ERR_INVALID_ARG, TAG, "bad state %d", state);

    app_state_event_t next = { .state = state };
    bool has_detail = state == APP_STATE_SETUP || state == APP_STATE_ERROR;
    /* strlcpy copies at most size - 1 characters and always ends the copy
     * with a NUL, which is what makes cutting long text safe */
    strlcpy(next.detail, has_detail && detail ? detail : "", sizeof(next.detail));

    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (next.state != s_current.state || strcmp(next.detail, s_current.detail) != 0) {
        err = esp_event_post(APP_STATE_EVENT, APP_STATE_CHANGED, &next, sizeof(next),
                             pdMS_TO_TICKS(POST_TIMEOUT_MS));
        /* Stored only once announced: a state nobody heard about would make
         * a retry look like a repeat, and the display would never catch up */
        if (err == ESP_OK) {
            s_current = next;
        }
    }
    xSemaphoreGive(s_lock);
    ESP_RETURN_ON_ERROR(err, TAG, "announcing %s", s_names[state]);
    return ESP_OK;
}

void app_state_get(app_state_event_t *out)
{
    if (s_lock == NULL) {
        *out = (app_state_event_t){ .state = APP_STATE_BOOT };
        return;
    }
    /* Under the lock, so the state and its detail are copied as a pair */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_current;
    xSemaphoreGive(s_lock);
}

const char *app_state_name(app_state_t state)
{
    return state < APP_STATE_COUNT ? s_names[state] : "?";
}

bool app_state_from_name(const char *name, app_state_t *out)
{
    for (int i = 0; i < APP_STATE_COUNT; i++) {
        if (strcmp(name, s_names[i]) == 0) {
            *out = (app_state_t)i;
            return true;
        }
    }
    return false;
}
