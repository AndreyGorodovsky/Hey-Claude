/*
 * Console command for the button and the switch. Described in controls_cmd.c.
 */
#pragma once

#include "esp_err.h"

/* Registers the `controls` command. Called by console_start(). */
esp_err_t controls_cmd_register(void);
