#include "app_config.h"

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"

static const char *TAG = "app_config";
static const char *NVS_NAMESPACE = "config";

/* Returns NULL if the value is acceptable, otherwise the reason it is not */
typedef const char *(*validator_t)(const char *value);

typedef struct {
    const char *key;        /* NVS key, at most 15 characters */
    size_t offset;          /* field in app_config_t */
    size_t max_len;
    bool secret;
    validator_t validate;   /* optional */
} field_t;

static const char *validate_wifi_pass(const char *v)
{
    size_t len = strlen(v);
    if (len >= 8 && len <= 63) {
        return NULL;
    }
    if (len == 64) {
        for (size_t i = 0; i < len; i++) {
            if (!isxdigit((unsigned char)v[i])) {
                return "a 64-character key must be hexadecimal";
            }
        }
        return NULL;
    }
    return "must be 8-63 characters, or 64 hexadecimal digits (unset it for an open network)";
}

static const char *validate_device_id(const char *v)
{
    /* Used as the DHCP hostname, and later the mDNS name: a valid DNS label */
    size_t len = strlen(v);
    if (len == 0 || v[0] == '-' || v[len - 1] == '-') {
        return "must not be empty or begin or end with '-'";
    }
    for (size_t i = 0; i < len; i++) {
        if (!isalnum((unsigned char)v[i]) && v[i] != '-') {
            return "only letters, digits and '-' are allowed";
        }
    }
    return NULL;
}

static const field_t FIELDS[] = {
    { "wifi_ssid",    offsetof(app_config_t, wifi_ssid),    APP_CFG_WIFI_SSID_MAX,    false, NULL },
    { "wifi_pass",    offsetof(app_config_t, wifi_pass),    APP_CFG_WIFI_PASS_MAX,    true,  validate_wifi_pass },
    { "server_url",   offsetof(app_config_t, server_url),   APP_CFG_SERVER_URL_MAX,   false, NULL },
    { "device_id",    offsetof(app_config_t, device_id),    APP_CFG_DEVICE_ID_MAX,    false, validate_device_id },
    { "device_token", offsetof(app_config_t, device_token), APP_CFG_DEVICE_TOKEN_MAX, true,  NULL },
};
#define N_FIELDS (sizeof(FIELDS) / sizeof(FIELDS[0]))

static_assert(APP_CFG_WIFI_SSID_MAX <= APP_CFG_VALUE_MAX && APP_CFG_WIFI_PASS_MAX <= APP_CFG_VALUE_MAX &&
              APP_CFG_SERVER_URL_MAX <= APP_CFG_VALUE_MAX && APP_CFG_DEVICE_ID_MAX <= APP_CFG_VALUE_MAX &&
              APP_CFG_DEVICE_TOKEN_MAX <= APP_CFG_VALUE_MAX, "APP_CFG_VALUE_MAX must cover every field");

static app_config_t s_config;

static const field_t *find_field(const char *key)
{
    for (size_t i = 0; i < N_FIELDS; i++) {
        if (strcmp(FIELDS[i].key, key) == 0) {
            return &FIELDS[i];
        }
    }
    return NULL;
}

static void default_device_id(char *out, size_t size)
{
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(out, size, "hc-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

/* ---- running configuration ---- */

esp_err_t app_config_load(void)
{
    memset(&s_config, 0, sizeof(s_config));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        for (size_t i = 0; i < N_FIELDS; i++) {
            char *dst = (char *)&s_config + FIELDS[i].offset;
            size_t len = FIELDS[i].max_len + 1;
            err = nvs_get_str(h, FIELDS[i].key, dst, &len);
            if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
                ESP_LOGW(TAG, "reading %s failed: %s", FIELDS[i].key, esp_err_to_name(err));
                dst[0] = '\0';
            }
        }
        nvs_close(h);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        /* NOT_FOUND only means nothing has been saved yet */
        return err;
    }

    if (s_config.device_id[0] == '\0') {
        default_device_id(s_config.device_id, sizeof(s_config.device_id));
    }
    return ESP_OK;
}

const app_config_t *app_config_get(void)
{
    return &s_config;
}

bool app_config_has_wifi(void)
{
    return s_config.wifi_ssid[0] != '\0';
}

/* ---- stored configuration ---- */

size_t app_config_key_count(void)
{
    return N_FIELDS;
}

const char *app_config_key_name(size_t i)
{
    return i < N_FIELDS ? FIELDS[i].key : NULL;
}

bool app_config_key_info(const char *key, bool *secret)
{
    const field_t *f = find_field(key);
    if (f && secret) {
        *secret = f->secret;
    }
    return f != NULL;
}

esp_err_t app_config_read_stored(const char *key, char *buf, size_t size)
{
    if (!find_field(key)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_str(h, key, buf, &size);
        nvs_close(h);
    }
    /* From either call, NVS NOT_FOUND means this key holds nothing */
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
}

static esp_err_t write_value(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = value ? nvs_set_str(h, key, value) : nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;               /* unsetting a key that was never set */
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t app_config_set(const char *key, const char *value, const char **reason)
{
    const field_t *f = find_field(key);
    if (!f) {
        if (reason) {
            *reason = "unknown key";
        }
        return ESP_ERR_INVALID_ARG;
    }
    const char *invalid = strlen(value) > f->max_len ? "too long"
                        : f->validate ? f->validate(value) : NULL;
    if (invalid) {
        if (reason) {
            *reason = invalid;
        }
        return ESP_ERR_INVALID_ARG;
    }
    return write_value(key, value);
}

esp_err_t app_config_unset(const char *key)
{
    if (!find_field(key)) {
        return ESP_ERR_INVALID_ARG;
    }
    return write_value(key, NULL);
}
