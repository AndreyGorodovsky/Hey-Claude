/*
 * Console command for the playback volume. Described in volume_cmd.c.
 */
#pragma once

#include "esp_err.h"

/* Registers the `volume` command. Called by console_start(). */
esp_err_t volume_cmd_register(void);
