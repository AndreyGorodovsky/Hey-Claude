/*
 * The captive-portal half of setup mode: a DNS server that answers every
 * name with the device's own address. Private to the setup component.
 * Ported from the author's earlier project (token_monitor).
 *
 * What it is for. A phone that joins a WiFi network at once asks the
 * internet a question it already knows the answer to: Android fetches
 * http://connectivitycheck.gstatic.com/generate_204, Apple fetches
 * http://captive.apple.com/hotspot-detect.html, only to find out whether the
 * network really reaches the internet. If something else comes back, the
 * phone decides it is behind a sign-in page and opens it, as on hotel WiFi.
 * DNS (the Domain Name System) is how a name becomes an address; answering
 * every name with this device's address sends the phone's question to the
 * device's web server, whose redirect (provision.c) makes the setup page
 * open by itself.
 *
 * What it is not. A DNS server: it reads just enough of a question to find
 * where it ends, and answers every request for an IPv4 address with one
 * fixed address. It exists only during setup mode, on a network this device
 * made.
 *
 * Everything works without it: the address is on the screen and can be
 * typed. A failure to start it is logged, not acted on.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Starts answering DNS on its own small task. `ip` is the address to give
 * for every name, in network byte order, as esp_netif_ip_info_t holds it. */
esp_err_t captive_dns_start(uint32_t ip);
