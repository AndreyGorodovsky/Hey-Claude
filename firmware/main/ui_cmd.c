/*
 * Console commands for the device state and the display.
 *
 *   state                        print the current state
 *   state set <name> [detail]    change the state; stops a running cycle.
 *                                The detail shows only for setup and error.
 *   state cycle [seconds]        step through every state, each shown for
 *                                that long (default 3 s), until stopped
 *   state stop                   stop cycling; the state stays where it is
 *   display test [seconds]       show the display test pattern (default
 *                                10 s) and print how fast frames are sent
 *
 * `state set` and `state cycle` stand in for the state machine that stage 6
 * builds, which will be the only code that sets the state (app_state.h).
 * They change the state through the same app_state_set(), and the display
 * follows by itself. Once the real state machine exists they would fight it,
 * so stage 6 replaces them with commands that feed it pretend inputs
 * instead. The state names are the ones app_state_name() gives.
 *
 * Context: the handlers run in the console task. The cycle is driven by an
 * esp_timer: a software timer whose callback ESP-IDF runs in its own
 * high-priority "esp_timer" task, so callbacks must be short and must not
 * block for long. Changing the state is both. The display test's results
 * are printed from the display task, through a callback.
 */
#include "ui_cmd.h"

#include <stdio.h>
#include <string.h>
#include "app_state.h"
#include "console.h"
#include "display.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "ui_cmd";

#define CYCLE_SECONDS_DEFAULT   3
#define CYCLE_SECONDS_MAX       60
#define TEST_SECONDS_DEFAULT    10
#define TEST_SECONDS_MAX        60

static esp_timer_handle_t s_cycle_timer;
/* The state the next cycle step shows. The timer task and the console task
 * both advance it, possibly on different cores at the same moment, since
 * stopping the timer does not wait for a step already running. The spinlock
 * (a lock for very short sections; a task on the other core waits by
 * spinning) makes each step take a different state. */
static portMUX_TYPE s_cycle_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_cycle_next;

static void cycle_step(void *arg)
{
    taskENTER_CRITICAL(&s_cycle_lock);
    app_state_t st = (app_state_t)s_cycle_next;
    s_cycle_next = (s_cycle_next + 1) % APP_STATE_COUNT;
    taskEXIT_CRITICAL(&s_cycle_lock);
    /* The setup and error views show a reason */
    const char *detail = st == APP_STATE_SETUP ? "Mock: settings missing"
                       : st == APP_STATE_ERROR ? "Mock error from the console" : NULL;
    app_state_set(st, detail);
}

/* Stops the cycle if it is running. A step already under way in the timer
 * task still completes, so a `state set` typed at that exact moment can be
 * overwritten by one last step; for a test command that is acceptable. */
static void cycle_stop(void)
{
    if (esp_timer_is_active(s_cycle_timer)) {
        esp_timer_stop(s_cycle_timer);
    }
}

static int state_show(void)
{
    app_state_event_t st;
    app_state_get(&st);
    printf("state: %s%s%s\n", app_state_name(st.state),
           st.detail[0] ? ", " : "", st.detail);
    return 0;
}

static void print_names(void)
{
    for (int i = 0; i < APP_STATE_COUNT; i++) {
        printf("%s%s", i ? ", " : "States: ", app_state_name((app_state_t)i));
    }
    printf("\n");
}

/* Handles `state ...`. Returns 0 on success, like a shell command. */
static int cmd_state(int argc, char **argv)
{
    if (argc == 1) {
        return state_show();
    }
    if (argc == 2 && strcmp(argv[1], "stop") == 0) {
        cycle_stop();
        return state_show();
    }
    if ((argc == 3 || argc == 4) && strcmp(argv[1], "set") == 0) {
        app_state_t st;
        if (!app_state_from_name(argv[2], &st)) {
            printf("unknown state '%s'. ", argv[2]);
            print_names();
            return 1;
        }
        cycle_stop();
        esp_err_t err = app_state_set(st, argc == 4 ? argv[3] : NULL);
        if (err != ESP_OK) {
            printf("failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        return 0;
    }
    int seconds = CYCLE_SECONDS_DEFAULT;
    if ((argc == 2 || argc == 3) && strcmp(argv[1], "cycle") == 0
        && (argc == 2 || console_parse_int(argv[2], 1, CYCLE_SECONDS_MAX, &seconds))) {
        cycle_stop();
        taskENTER_CRITICAL(&s_cycle_lock);
        s_cycle_next = 0;
        taskEXIT_CRITICAL(&s_cycle_lock);
        cycle_step(NULL);   /* show the first state now, not after one period */
        /* esp_timer counts in microseconds */
        esp_err_t err = esp_timer_start_periodic(s_cycle_timer, (uint64_t)seconds * 1000000);
        if (err != ESP_OK) {
            printf("failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("Cycling every %d s. Stop with: state stop\n", seconds);
        return 0;
    }
    printf("usage: state | state set <name> [detail] | state cycle [seconds 1-%d] | "
           "state stop\n", CYCLE_SECONDS_MAX);
    return 1;
}

/* Runs in the display task when a test ends */
static void test_done(const display_test_result_t *r)
{
    printf("display test: %lu full frames in %lu s at SPI %lu MHz\n",
           (unsigned long)r->frames, (unsigned long)r->seconds, (unsigned long)r->spi_mhz);
    if (r->frames > 0) {
        printf("  sending one frame took %.1f ms on average (min %.1f, max %.1f)\n",
               r->avg_ms, r->min_ms, r->max_ms);
    }
    printf("  display task stack: %lu bytes never used\n", (unsigned long)r->stack_unused);
}

/* Handles `display ...` */
static int cmd_display(int argc, char **argv)
{
    int seconds = TEST_SECONDS_DEFAULT;
    if (argc < 2 || argc > 3 || strcmp(argv[1], "test") != 0
        || (argc == 3 && !console_parse_int(argv[2], 1, TEST_SECONDS_MAX, &seconds))) {
        printf("usage: display test [seconds 1-%d]\n", TEST_SECONDS_MAX);
        return 1;
    }
    esp_err_t err = display_test_start((uint32_t)seconds, test_done);
    if (err == ESP_ERR_INVALID_STATE) {
        printf("display not ready, or a test is already running\n");
        return 1;
    }
    if (err != ESP_OK) {
        printf("failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Test pattern for %d s; results follow.\n", seconds);
    return 0;
}

esp_err_t ui_cmd_register(void)
{
    const esp_timer_create_args_t timer = {
        .callback = cycle_step,
        .name = "state_cycle",  /* shown in esp_timer diagnostics */
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer, &s_cycle_timer), TAG, "cycle timer");

    const esp_console_cmd_t state = {
        .command = "state",
        .help = "Show or change the device state, or cycle through all states "
                "to exercise the display",
        .hint = "[set <name> [detail] | cycle [seconds] | stop]",
        .func = cmd_state,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&state), TAG, "state");

    const esp_console_cmd_t display = {
        .command = "display",
        .help = "Show a test pattern that checks the panel's edges and wiring, "
                "and measure how long a full frame takes to send",
        .hint = "test [seconds]",
        .func = cmd_display,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&display), TAG, "display");
    return ESP_OK;
}
