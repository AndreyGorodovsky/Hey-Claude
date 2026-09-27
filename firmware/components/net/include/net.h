/*
 * WiFi station. Joins the configured network and keeps rejoining it with
 * exponential backoff whenever the connection is lost.
 */
#pragma once

#include "esp_err.h"

/* Starts WiFi and the first connection attempt; does not wait for it.
 * `pass` is empty for an open network. `hostname` is announced over DHCP.
 * Requires the default event loop and esp_netif to be initialised.
 *
 * Connection state is published on the default event loop as the standard
 * IP_EVENT_STA_GOT_IP and WIFI_EVENT_STA_DISCONNECTED events. */
esp_err_t net_start(const char *ssid, const char *pass, const char *hostname);
