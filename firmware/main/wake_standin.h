/*
 * A temporary reaction to the wake word, until the state machine exists.
 * Details in wake_standin.c.
 */
#pragma once

#include "esp_err.h"

/* Starts reacting to wake-word detections. Call once at boot, after
 * app_state_init(). */
esp_err_t wake_standin_start(void);
