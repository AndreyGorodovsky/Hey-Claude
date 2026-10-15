/*
 * The board's BOOT button, as a way into and out of setup mode. Private to
 * the setup component.
 *
 * The button connects GPIO 0 to ground when pressed. GPIO 0 is a strapping
 * pin: the chip reads it at power-on and at reset, and if it is low then,
 * the chip waits to be flashed instead of running this firmware. That is
 * what the button is for on a development board, and it still works so.
 * Once the firmware is running, the pin is an ordinary input, and reading it
 * changes nothing about how the chip next starts.
 */
#pragma once

#include "esp_err.h"

/* Calls `on_hold` once the button has been held down for 3 s without a
 * break. A press must be let go before it can count again. `on_hold` runs
 * in the esp_timer task, ESP-IDF's task for software timers, so it must be
 * short; restarting the chip is short enough. Call once. */
esp_err_t button_watch(void (*on_hold)(void));
