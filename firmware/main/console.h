/*
 * Serial console on the board's UART port, for entering settings.
 * Commands are listed in console.c.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Starts the console in its own task and registers every command. Returns
 * once the task is running. The app_config settings must already be loaded. */
esp_err_t console_start(void);

/* Parses a whole number in [min, max] from a command argument; false if the
 * text is anything else. For command handlers. */
bool console_parse_int(const char *s, int min, int max, int *out);
