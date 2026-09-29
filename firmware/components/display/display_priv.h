/*
 * Shared inside the display component only: the screen's size and the
 * interfaces between its three files.
 *
 *   display.c        the panel, backlight and display task
 *   display_ui.c     what is drawn for each device state
 *   display_test.c   the `display test` pattern and its measurements
 *
 * Every function here runs in the display task, the only task that uses
 * LVGL, except test_request(), which any task may call.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "app_state.h"
#include "board.h"
#include "display.h"
#include "lvgl.h"

/* SPI clock: 80 MHz, the most SPI2 allows, and only on its dedicated pins,
 * which GPIO 10-12 are. Measured on 2026-09-29 on the breadboard with
 * `display test`: a full frame takes 12.5 ms (98.7 ms at the 10 MHz used in
 * bring-up), and neither the test pattern nor the state animations showed a
 * corrupted pixel. No margin is taken below the fastest clean speed because
 * a breadboard's long wires are the worst case for fast signals; a soldered
 * circuit can only be cleaner. If corruption appears, 40 MHz is next. */
#define SPI_CLOCK_HZ        (80 * 1000 * 1000)

/* Landscape: the panel's 428 rows become the screen's width */
#define SCREEN_W            BOARD_LCD_HEIGHT
#define SCREEN_H            BOARD_LCD_WIDTH
#define FRAME_BYTES         (SCREEN_W * SCREEN_H * 2)   /* 2 bytes per RGB565 pixel: 121,552 */

/* ---- display_ui.c ---- */

/* Builds the state view and the test pattern and loads the state view */
void ui_init(void);

/* Shows `st` on the state view, replacing whatever animation was running */
void ui_show_state(const app_state_event_t *st);

/* Backlight brightness for a state, in percent */
int ui_brightness(app_state_t state);

/* Switches to the test pattern; ui_test_end() switches back */
void ui_test_begin(void);
void ui_test_end(void);

/* Moves the test pattern's bar one step and marks the whole screen for
 * redrawing, so the next refresh sends a full frame */
void ui_test_step(void);

/* ---- display_test.c ---- */

/* Records a request from any task. False if a test is already requested or
 * running. The display task is woken separately, by the caller. */
bool test_request(uint32_t seconds, display_test_done_cb_t done);

/* Starts a requested test, if there is one */
void test_begin(void);

/* True while a test runs; the display task then redraws continuously */
bool test_running(void);

/* Advances a running test by one frame, or ends it when its time is up:
 * the state view returns and the results go to the requester's callback.
 * Returns true if the test ended. */
bool test_tick(void);

/* Called for every completed pixel transfer, with its size and duration */
void test_frame_sent(size_t bytes, int64_t us);
