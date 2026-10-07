/*
 * Console command for the chip's temperature. Described in temp_cmd.c,
 * which also lists what to delete to remove it.
 */
#pragma once

#include "esp_err.h"

/* Registers the `temp` command. Called by console_start(). */
esp_err_t temp_cmd_register(void);
