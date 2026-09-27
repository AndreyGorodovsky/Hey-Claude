#include "net.h"

#include <stdbool.h>
#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"

static const char *TAG = "net";

#define BACKOFF_MIN_MS      1000
#define BACKOFF_MAX_MS      30000

static bool s_started;
static esp_timer_handle_t s_retry_timer;
static uint32_t s_backoff_ms = BACKOFF_MIN_MS;
static volatile bool s_stopping;

static const char *reason_str(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        return "network not found (check wifi_ssid, or 2.4 GHz availability)";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "network security not supported (WPA2 or newer required)";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "signal too weak";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "authentication failed (check wifi_pass)";
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_ASSOC_EXPIRE:
        return "access point did not respond";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "signal lost";
    case WIFI_REASON_ASSOC_LEAVE:
        return "disassociated";
    case WIFI_REASON_CONNECTION_FAIL:
        return "connection failed";
    default:
        return "see wifi_err_reason_t in esp_wifi_types_generic.h";
    }
}

static void schedule_retry(void)
{
    if (s_stopping) {
        return;
    }
    esp_err_t err = esp_timer_start_once(s_retry_timer, (uint64_t)s_backoff_ms * 1000);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {    /* INVALID_STATE: already armed */
        ESP_LOGE(TAG, "cannot schedule retry: %s", esp_err_to_name(err));
    }
    s_backoff_ms = s_backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : s_backoff_ms * 2;
}

static void retry_cb(void *arg)
{
    if (s_stopping) {
        return;
    }
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        /* No disconnect event will follow, so re-arm here or the chain ends */
        ESP_LOGW(TAG, "connect request failed: %s; retrying in %lu s",
                 esp_err_to_name(err), (unsigned long)(s_backoff_ms / 1000));
        schedule_retry();
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START) {
        retry_cb(NULL);
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        if (s_stopping) {
            ESP_LOGI(TAG, "disconnected for shutdown");
            return;
        }
        ESP_LOGW(TAG, "disconnected, reason %d: %s; retrying in %lu s",
                 ev->reason, reason_str(ev->reason), (unsigned long)(s_backoff_ms / 1000));
        schedule_retry();
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *ev = data;
    wifi_ap_record_t ap = { 0 };
    esp_wifi_sta_get_ap_info(&ap);      /* zeroed fields on failure are harmless in a log */

    ESP_LOGI(TAG, "connected: ip " IPSTR ", rssi %d dBm, channel %d",
             IP2STR(&ev->ip_info.ip), ap.rssi, ap.primary);
    s_backoff_ms = BACKOFF_MIN_MS;
}

/* Runs on esp_restart() before the WiFi driver's own shutdown handler,
 * because handlers run in reverse order of registration. */
static void on_shutdown(void)
{
    s_stopping = true;
    esp_timer_stop(s_retry_timer);
}

esp_err_t net_start(const char *ssid, const char *pass, const char *hostname)
{
    ESP_RETURN_ON_FALSE(!s_started, ESP_ERR_INVALID_STATE, TAG, "already started");
    s_started = true;

    /* Resources are not released on failure: net_start runs once at boot, and a
     * failure leaves the device on the console for the settings to be fixed. */
    const esp_timer_create_args_t timer_args = { .callback = retry_cb, .name = "wifi_retry" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_retry_timer), TAG, "retry timer");

    esp_netif_t *netif = esp_netif_create_default_wifi_sta();   /* registers the driver's shutdown handler */
    ESP_RETURN_ON_FALSE(netif, ESP_FAIL, TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_register_shutdown_handler(on_shutdown), TAG, "shutdown handler");
    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(netif, hostname), TAG, "hostname");

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL),
                        TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL),
                        TAG, "ip handler");

    wifi_config_t wc = { 0 };
    /* Both fields are fixed-size and need not be NUL-terminated at full length */
    memcpy(wc.sta.ssid, ssid, strnlen(ssid, sizeof(wc.sta.ssid)));
    memcpy(wc.sta.password, pass, strnlen(pass, sizeof(wc.sta.password)));
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;     /* zero would restrict WPA3 to hunt-and-peck */
    wc.sta.pmf_cfg.capable = true;

    /* Credentials are owned by the caller; keep the driver from storing a copy */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");

    ESP_LOGI(TAG, "joining '%s' as %s", ssid, hostname);
    return ESP_OK;
}
