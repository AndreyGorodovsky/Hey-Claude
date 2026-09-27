/*
 * Serial console on the board's UART port, for entering settings.
 * Commands are listed in console.c.
 */
#pragma once

#include "esp_err.h"

/* Starts the console in its own task and registers every command. Returns
 * once the task is running. The app_config settings must already be loaded. */
esp_err_t console_start(void);
