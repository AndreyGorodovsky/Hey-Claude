/*
 * Console commands for the device state and the display.
 *
 *   state                        print the current state, and for setup and
 *                                error the reason shown with it
 *   display test [seconds]       show the display test pattern (default
 *                                10 s) and print how fast frames are sent
 *   display preview <state> [detail] [seconds]
 *                                draw a state's screen for a while (default
 *                                5 s) without changing the device's state.
 *                                A detail with spaces goes in double quotes.
 *
 * `state` only reads. The state has one writer, the state machine (the
 * state_machine component), and a console command that set it would fight that writer.
 * Until stage 6 this file had such commands, standing in for the state
 * machine; they went when it arrived.
 *
 * Context: the handlers run in the console task. The display test's
 * results are printed from the display task, through a callback.
 */
#include "ui_cmd.h"

#include <stdio.h>
#include <string.h>
#include "app_state.h"
#include "console.h"
#include "display.h"
#include "esp_check.h"
#include "esp_console.h"

static const char *TAG = "ui_cmd";

#define TEST_SECONDS_DEFAULT    10
#define TEST_SECONDS_MAX        60
#define PREVIEW_SECONDS_DEFAULT 5

/* Handles `state`. Returns 0 on success, like a shell command. */
static int cmd_state(int argc, char **argv)
{
    if (argc != 1) {
        printf("usage: state\n");
        return 1;
    }
    app_state_event_t st;
    app_state_get(&st);
    printf("state: %s%s%s\n", app_state_name(st.state),
           st.detail[0] ? ", " : "", st.detail);
    return 0;
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

/* Handles `display preview <state> [detail] [seconds]`. The state's screen
 * is drawn; the state itself, which only the state machine sets, is left
 * alone. */
static int cmd_preview(int argc, char **argv)
{
    app_state_t state;
    if (argc < 3 || argc > 5 || !app_state_from_name(argv[2], &state)) {
        printf("usage: display preview <state> [detail] [seconds 1-%d]\nStates:",
               TEST_SECONDS_MAX);
        for (int i = 0; i < APP_STATE_COUNT; i++) {
            printf(" %s", app_state_name((app_state_t)i));
        }
        printf("\n");
        return 1;
    }
    /* The last word is the time if it is a number; before it comes the
     * detail. So `preview error 10` shows no detail for 10 s. */
    int seconds = PREVIEW_SECONDS_DEFAULT;
    const char *detail = NULL;
    int last = argc - 1;
    if (last >= 3 && console_parse_int(argv[last], 1, TEST_SECONDS_MAX, &seconds)) {
        last--;
    }
    if (last == 3) {
        detail = argv[3];
    } else if (last > 3) {
        printf("too many words; put a detail with spaces in double quotes\n");
        return 1;
    }
    esp_err_t err = display_preview(state, detail, (uint32_t)seconds);
    if (err != ESP_OK) {
        printf("failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Showing '%s' for %d s.\n", app_state_name(state), seconds);
    return 0;
}

/* Handles `display ...` */
static int cmd_display(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "preview") == 0) {
        return cmd_preview(argc, argv);
    }
    int seconds = TEST_SECONDS_DEFAULT;
    if (argc < 2 || argc > 3 || strcmp(argv[1], "test") != 0
        || (argc == 3 && !console_parse_int(argv[2], 1, TEST_SECONDS_MAX, &seconds))) {
        printf("usage: display test [seconds 1-%d]\n"
               "       display preview <state> [detail] [seconds 1-%d]\n",
               TEST_SECONDS_MAX, TEST_SECONDS_MAX);
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
    const esp_console_cmd_t state = {
        .command = "state",
        .help = "Show the device state",
        .func = cmd_state,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&state), TAG, "state");

    const esp_console_cmd_t display = {
        .command = "display",
        .help = "Show a test pattern that checks the panel's edges and wiring and "
                "measures how long a full frame takes to send, or draw one "
                "state's screen for a while without changing the state",
        .hint = "test [seconds] | preview <state> [detail] [seconds]",
        .func = cmd_display,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&display), TAG, "display");
    return ESP_OK;
}
