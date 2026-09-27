/*
 * WiFi station. Joins the configured network and keeps rejoining it with
 * exponential backoff whenever the connection is lost.
 */
#pragma once

#include "esp_err.h"

/* Starts WiFi and the first connection attempt; returns without waiting for it.
 *   ssid      network name, at most 32 characters
 *   pass      WPA2/WPA3 password, or empty for an open network
 *   hostname  name announced to the router over DHCP
 * Requires esp_netif_init() and esp_event_loop_create_default() to have run.
 * Call once; a second call returns ESP_ERR_INVALID_STATE.
 *
 * Connection state is published on the default event loop as the standard
 * IP_EVENT_STA_GOT_IP (connected, address assigned) and
 * WIFI_EVENT_STA_DISCONNECTED (lost) events. Code that needs to know whether
 * the network is up registers a handler for those with esp_event_handler_register(). */
esp_err_t net_start(const char *ssid, const char *pass, const char *hostname);
