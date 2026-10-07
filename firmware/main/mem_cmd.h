/*
 * Console command for memory headroom. Described in mem_cmd.c, which also
 * lists what to delete to remove it.
 */
#pragma once

#include "esp_err.h"

/* Registers the `mem` command. Called by console_start(). */
esp_err_t mem_cmd_register(void);
