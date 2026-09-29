/*
 * The `display test` pattern and its measurements. See display_test_start()
 * in display.h.
 *
 * While a test runs, the display task redraws the whole screen on every
 * pass, and each full-frame transfer's duration is added up here. The
 * duration comes from display.c, measured from the moment the transfer is
 * queued to the interrupt that reports its last byte sent.
 *
 * Context: test_request() runs in whatever task asks (the console). Every
 * other function runs in the display task. The request is handed over under
 * a spinlock: a lock for very short sections, where a task on the other core
 * waits by spinning rather than sleeping.
 */
#include <string.h>
#include "display_priv.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* The request, shared between the requesting task and the display task.
 * s_busy stays true from the request until the results are delivered, so a
 * second request in between is refused. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_busy;
static uint32_t s_req_seconds;
static display_test_done_cb_t s_req_done;

/* The running test; display task only */
static bool s_running;
static uint32_t s_seconds;
static display_test_done_cb_t s_done;
static int64_t s_end_us;
static uint32_t s_frames;
static int64_t s_sum_us, s_min_us, s_max_us;

bool test_request(uint32_t seconds, display_test_done_cb_t done)
{
    bool accepted = false;
    taskENTER_CRITICAL(&s_lock);
    if (!s_busy) {
        s_busy = true;
        s_req_seconds = seconds;
        s_req_done = done;
        accepted = true;
    }
    taskEXIT_CRITICAL(&s_lock);
    return accepted;
}

void test_begin(void)
{
    if (s_running) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    bool requested = s_busy;
    s_seconds = s_req_seconds;
    s_done = s_req_done;
    taskEXIT_CRITICAL(&s_lock);
    if (!requested) {
        return;
    }
    s_frames = 0;
    s_sum_us = 0;
    s_min_us = INT64_MAX;
    s_max_us = 0;
    s_end_us = esp_timer_get_time() + (int64_t)s_seconds * 1000000;   /* s to us */
    s_running = true;
    ui_test_begin();
}

bool test_running(void)
{
    return s_running;
}

bool test_tick(void)
{
    if (!s_running) {
        return false;
    }
    if (esp_timer_get_time() < s_end_us) {
        ui_test_step();
        return false;
    }

    s_running = false;
    ui_test_end();
    display_test_result_t r = {
        .seconds = s_seconds,
        .spi_mhz = SPI_CLOCK_HZ / 1000000,
        .frames = s_frames,
        /* The stack's "high-water mark": the least free stack this task has
         * ever had, in bytes on ESP-IDF. Near zero means it is too small. */
        .stack_unused = uxTaskGetStackHighWaterMark(NULL),
    };
    if (s_frames > 0) {
        /* Microseconds to milliseconds */
        r.avg_ms = s_sum_us / 1000.0f / s_frames;
        r.min_ms = s_min_us / 1000.0f;
        r.max_ms = s_max_us / 1000.0f;
    }
    if (s_done) {
        s_done(&r);
    }
    /* Only now may another test be requested */
    taskENTER_CRITICAL(&s_lock);
    s_busy = false;
    taskEXIT_CRITICAL(&s_lock);
    return true;
}

void test_frame_sent(size_t bytes, int64_t us)
{
    if (!s_running || bytes != FRAME_BYTES) {
        return;     /* only full frames are comparable */
    }
    s_frames++;
    s_sum_us += us;
    if (us < s_min_us) s_min_us = us;
    if (us > s_max_us) s_max_us = us;
}
