/*
 * Console command for the server connection. Described in link_cmd.c.
 */
#pragma once

#include "esp_err.h"

/* Registers the `link` command. Called by console_start(). */
esp_err_t link_cmd_register(void);
