/*
 * The hotspot and the settings page. Private to the setup component, which
 * runs it from setup_run(). Ported from the author's earlier project
 * (token_monitor), and changed to this device's settings (app_config.h).
 *
 * A WiFi chip can be a station, a client that joins someone else's network,
 * which is what this device is in normal use, or an access point, a network
 * of its own that others join. Setup mode makes it an access point with a
 * phone as its one client, serves that phone one page, and stores what the
 * page sends.
 *
 * What is deliberately not done: trying the WiFi password before keeping
 * it. Trying it means joining the network, and an access point on this chip
 * has to move to the channel of whatever network its station side joins; the
 * hotspot would move out from under the phone that is waiting for the
 * result. The device recovers instead: settings that never work bring it
 * back to setup by itself (main.c).
 *
 * The page never sends back a stored WiFi password or device token. Their
 * fields are always empty, and empty means "keep what is stored".
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* What a person needs to reach the page, for the screen */
typedef struct {
    char ssid[33];          /* the hotspot's name, e.g. "Hey-Claude-A3F2" */
    char password[9];       /* 8 random characters, the least WPA2 allows */
    char address[16];       /* the page's address, e.g. "192.168.4.1" */
} provision_info_t;

/* Starts WiFi, looks for the networks in range (offered on the page), then
 * raises the hotspot, its captive-portal DNS and the web server. Returns
 * once all of it runs, with `out` filled in. Call once, at boot, before
 * anything else has started WiFi. A failure leaves WiFi in no known state,
 * and the caller should restart. */
esp_err_t provision_start(provision_info_t *out);

/* True once the page's settings have been stored and the reply has gone to
 * the phone, so that restarting cannot cost the phone its answer. */
bool provision_saved(void);

/* True while a form is being received or stored: a restart then could lose
 * it, or store some of its values and not others. */
bool provision_busy(void);
