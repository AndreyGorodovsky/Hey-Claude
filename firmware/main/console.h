#pragma once

#include "esp_err.h"

/* Starts the serial console on the UART and registers every command. */
esp_err_t console_start(void);
