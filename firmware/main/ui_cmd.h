/*
 * Console commands for the device state and the display. Commands are listed
 * in ui_cmd.c.
 */
#pragma once

#include "esp_err.h"

/* Registers the `state` and `display` commands. Called by console_start(). */
esp_err_t ui_cmd_register(void);
