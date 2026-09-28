/*
 * Console commands for testing audio on the hardware. Commands are listed in
 * audio_cmd.c.
 */
#pragma once

#include "esp_err.h"

/* Registers the `audio` command. Called by console_start(). If audio_init()
 * failed at boot, the tests report ESP_ERR_INVALID_STATE. */
esp_err_t audio_cmd_register(void);
