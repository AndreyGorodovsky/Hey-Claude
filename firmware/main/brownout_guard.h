/*
 * Brownout guard: keeps a device on a supply too weak for it from
 * restarting for ever. See brownout_guard.c.
 */
#pragma once

#include <stdbool.h>

/* Counts this boot if it follows a brownout, and says whether the guard has
 * tripped: true at the third brownout in a row, when the caller should not
 * go on to start WiFi or play anything. Call once, early in app_main(). */
bool brownout_guard_tripped(void);
