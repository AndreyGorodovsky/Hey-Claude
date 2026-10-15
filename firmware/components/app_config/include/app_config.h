/*
 * Device configuration, persisted in NVS (flash-backed key-value storage).
 *
 * The running configuration is loaded once at boot and is read-only; edits
 * made through app_config_set() and app_config_unset() change what is stored
 * and take effect after a reboot. Nothing running ever sees a setting change
 * underneath it.
 *
 * This component has no user interface of its own: the serial console and
 * the setup page (the setup component) sit on top of it and share its
 * validation.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Maximum length of each setting in characters, excluding the terminating NUL */
#define APP_CFG_WIFI_SSID_MAX     32      /* limit set by the WiFi standard */
#define APP_CFG_WIFI_PASS_MAX     64      /* 63-character passphrase or 64 hex digits */
#define APP_CFG_SERVER_URL_MAX    127
#define APP_CFG_DEVICE_ID_MAX     32      /* also the network hostname limit */
#define APP_CFG_DEVICE_TOKEN_MAX  64      /* PROTO_DEVICE_TOKEN_MAX, checked in app_config.c */
#define APP_CFG_VALUE_MAX         127     /* longest of the above; size buffers with it */

/* The settings, each a NUL-terminated string. Empty means not set. */
typedef struct {
    char wifi_ssid[APP_CFG_WIFI_SSID_MAX + 1];
    char wifi_pass[APP_CFG_WIFI_PASS_MAX + 1];       /* empty for an open network */
    char server_url[APP_CFG_SERVER_URL_MAX + 1];     /* empty: discover the server on the LAN */
    char device_id[APP_CFG_DEVICE_ID_MAX + 1];       /* never empty: defaults to one derived from the MAC */
    char device_token[APP_CFG_DEVICE_TOKEN_MAX + 1]; /* credential for the server, from stage 6 */
} app_config_t;

/* ---- running configuration ---- */

/* Reads the configuration from NVS into memory. Call once at boot, after
 * nvs_flash_init() and before anything calls app_config_get(). */
esp_err_t app_config_load(void);

/* The configuration loaded at boot. Safe to read from any task. */
const app_config_t *app_config_get(void);

/* True when every value needed to join the network is present. */
bool app_config_has_wifi(void);

/* True when every value the device needs to work is present: the network,
 * and the token the server asks for. */
bool app_config_complete(void);

/* ---- stored configuration ----
 * These read and change NVS directly and do not affect the running
 * configuration. Every function below returns ESP_ERR_INVALID_ARG for an
 * unknown key. */

/* Number of keys, and the name of key i (NULL if out of range). */
size_t app_config_key_count(void);
const char *app_config_key_name(size_t i);

/* Whether `key` exists; if so, `secret` (may be NULL) reports whether its
 * value must never be displayed. */
bool app_config_key_info(const char *key, bool *secret);

/* What a setting is called in a sentence, e.g. "The WiFi password", for
 * reporting a rejected value; the key itself if it is unknown. */
const char *app_config_key_label(const char *key);

/* Reads the stored value of a key into buf (size in bytes, including the NUL).
 * ESP_ERR_NOT_FOUND if it is not set. */
esp_err_t app_config_read_stored(const char *key, char *buf, size_t size);

/* One change to the stored settings: `value` is stored under `key`, or
 * the key is removed when `value` is NULL */
typedef struct {
    const char *key;
    const char *value;
} app_config_edit_t;

/* Checks every edit, then stores them all; if any is rejected, nothing is
 * stored. A rejection returns ESP_ERR_INVALID_ARG with *bad_key and *reason
 * (either may be NULL) set to the key and an explanation. Removing a key
 * that is not set succeeds. */
esp_err_t app_config_store(const app_config_edit_t *edits, size_t count,
                           const char **bad_key, const char **reason);

/* app_config_store() for one value. If it is rejected, returns
 * ESP_ERR_INVALID_ARG with *reason (may be NULL) set to an explanation. */
esp_err_t app_config_set(const char *key, const char *value, const char **reason);

/* Removes a stored value. Removing one that is not set succeeds. */
esp_err_t app_config_unset(const char *key);
