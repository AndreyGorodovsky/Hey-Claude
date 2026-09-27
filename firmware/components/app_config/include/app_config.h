/*
 * Device configuration, persisted in NVS.
 *
 * The running configuration is loaded once at boot and is read-only; edits
 * made through app_config_set() and app_config_unset() change what is stored
 * and take effect after a reboot. This component has no user interface of its
 * own: the serial console, and later provisioning, sit on top of it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define APP_CFG_WIFI_SSID_MAX     32
#define APP_CFG_WIFI_PASS_MAX     64
#define APP_CFG_SERVER_URL_MAX    127
#define APP_CFG_DEVICE_ID_MAX     32
#define APP_CFG_DEVICE_TOKEN_MAX  64
#define APP_CFG_VALUE_MAX         127     /* longest of the above */

typedef struct {
    char wifi_ssid[APP_CFG_WIFI_SSID_MAX + 1];
    char wifi_pass[APP_CFG_WIFI_PASS_MAX + 1];       /* empty for an open network */
    char server_url[APP_CFG_SERVER_URL_MAX + 1];     /* empty: discover the server on the LAN */
    char device_id[APP_CFG_DEVICE_ID_MAX + 1];       /* defaults to one derived from the MAC */
    char device_token[APP_CFG_DEVICE_TOKEN_MAX + 1];
} app_config_t;

/* ---- running configuration ---- */

/* Reads the configuration from NVS. NVS must already be initialised. */
esp_err_t app_config_load(void);

/* The configuration loaded at boot. Valid after app_config_load(). */
const app_config_t *app_config_get(void);

/* True when every value needed to join the network is present. */
bool app_config_has_wifi(void);

/* ---- stored configuration ---- */

/* Every function below returns ESP_ERR_INVALID_ARG for an unknown key. */

/* Number of keys, and the name of key i (NULL if out of range). */
size_t app_config_key_count(void);
const char *app_config_key_name(size_t i);

/* Whether `key` exists; if so, `secret` (may be NULL) reports whether its
 * value must never be displayed. */
bool app_config_key_info(const char *key, bool *secret);

/* Reads the stored value of a key. ESP_ERR_NOT_FOUND if it is not set. */
esp_err_t app_config_read_stored(const char *key, char *buf, size_t size);

/* Validates and stores a value. If the value is rejected, returns
 * ESP_ERR_INVALID_ARG with *reason (may be NULL) set to an explanation. */
esp_err_t app_config_set(const char *key, const char *value, const char **reason);

/* Removes a stored value. Removing one that is not set succeeds. */
esp_err_t app_config_unset(const char *key);
