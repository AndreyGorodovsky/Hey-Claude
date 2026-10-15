/*
 * WiFi station. Joins the configured network and keeps rejoining it with
 * exponential backoff whenever the connection is lost.
 *
 * It also tells settings that cannot work from a network that is away for
 * a while. Settings that have never once connected, and that the network
 * keeps refusing, or whose network is never found, are reported as
 * hopeless, so that the device can ask for new ones (setup.h) instead of
 * trying for ever. Settings that have connected before are never reported:
 * their network is only away, a router restarting or switched off, and is
 * waited for. Whether they have connected is kept in NVS, as a checksum of
 * the network's name and password, so that it survives a restart.
 */
#pragma once

#include "esp_err.h"

/* Why settings were given up */
typedef enum {
    NET_HOPELESS_REFUSED,       /* the network refused the password, 5 times in a row */
    NET_HOPELESS_NOT_FOUND,     /* no connection in 10 min: the network is not there */
} net_hopeless_t;

/* Starts WiFi and the first connection attempt; returns without waiting for it.
 *   ssid         network name, at most 32 characters
 *   pass         WPA2/WPA3 password, or empty for an open network
 *   hostname     name announced to the router over DHCP
 *   on_hopeless  called once if settings that have never connected are
 *                given up (see above), from the event loop's or the timer
 *                task. Required
 * Requires esp_netif_init() and esp_event_loop_create_default() to have run.
 * Call once; a second call returns ESP_ERR_INVALID_STATE.
 *
 * Connection state is published on the default event loop as the standard
 * IP_EVENT_STA_GOT_IP (connected, address assigned) and
 * WIFI_EVENT_STA_DISCONNECTED (lost) events. Code that needs to know whether
 * the network is up registers a handler for those with esp_event_handler_register(). */
esp_err_t net_start(const char *ssid, const char *pass, const char *hostname,
                    void (*on_hopeless)(net_hopeless_t why));
