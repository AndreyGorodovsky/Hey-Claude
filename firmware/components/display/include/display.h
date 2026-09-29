/*
 * The NV3007 display: panel, backlight and what is drawn on it.
 *
 * The display shows the device's state (app_state.h) and follows every
 * change by itself: nothing else calls into it for that. It is drawn with
 * LVGL, a graphics library, from one task on core 0 that owns the panel and
 * LVGL outright, so no other task ever touches either.
 *
 * The panel is used in landscape, 428 x 142 pixels.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Turns the backlight off and starts the display task, then returns without
 * waiting. The task sets up the panel, which takes about a second of
 * required pauses, draws the current state, and only then fades the
 * backlight in, so the screen never shows the random pixels a panel holds at
 * power-on. A failure there is logged, and the screen stays dark. Call once,
 * after app_state_init(). */
esp_err_t display_init(void);

/* Results of a display test */
typedef struct {
    uint32_t seconds;       /* how long the test ran */
    uint32_t spi_mhz;       /* SPI clock the panel runs at */
    uint32_t frames;        /* full frames sent */
    float avg_ms, min_ms, max_ms;   /* time to send one full frame */
    uint32_t stack_unused;  /* bytes of the display task's stack never used */
} display_test_result_t;

/* Called once when a test ends, in the display task: keep it short */
typedef void (*display_test_done_cb_t)(const display_test_result_t *result);

/* Replaces the state view for `seconds` (1-60) with a test pattern that is
 * redrawn in full on every pass of the display task: a 1-pixel frame in a
 * different colour on each edge, corner labels, and a moving bar. A missing
 * edge means a wrong offset; broken or shifted pixels mean the SPI clock is
 * too fast for the wiring. Returns at once; `done` receives the results when
 * the test ends, and the state view returns. ESP_ERR_INVALID_STATE if the
 * display is not working or a test is already running. */
esp_err_t display_test_start(uint32_t seconds, display_test_done_cb_t done);
