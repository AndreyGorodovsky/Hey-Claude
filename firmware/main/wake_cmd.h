/*
 * Console command for watching and tuning wake-word detection. Commands are
 * listed in wake_cmd.c.
 */
#pragma once

#include "esp_err.h"

/* Registers the `wake` command. Called by console_start(). */
esp_err_t wake_cmd_register(void);
