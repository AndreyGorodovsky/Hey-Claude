/*
 * WiFi station ("client") mode: join a network and stay joined.
 *
 * The ESP-IDF WiFi driver is event-driven. Calls such as esp_wifi_start() and
 * esp_wifi_connect() return immediately; the outcome arrives later as an
 * event on the default event loop, a background task that calls the handlers
 * registered for each event. The sequence on a good day:
 *
 *   esp_wifi_start()
 *     -> WIFI_EVENT_STA_START         driver ready; we call esp_wifi_connect()
 *     -> (association and WPA2 handshake with the router)
 *     -> IP_EVENT_STA_GOT_IP          router assigned an address via DHCP
 *
 * On any failure, or when an established connection drops, the driver posts
 * WIFI_EVENT_STA_DISCONNECTED with a reason code. This component then waits
 * and calls esp_wifi_connect() again, doubling the wait each time (1 s, 2 s,
 * 4 s ... up to 30 s) so that a router that is off for a while is not
 * hammered. A successful connection resets the wait to 1 s.
 *
 * Threading: event handlers run in the event loop task; the retry callback
 * runs in the esp_timer task. The notes on each static variable below explain
 * why they need no locks.
 */
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

#define BACKOFF_MIN_MS      1000    /* first retry delay */
#define BACKOFF_MAX_MS      30000   /* retry delay stops growing here */

/* Guards against net_start() being called twice. Set once at boot. */
static bool s_started;

/* One-shot software timer that triggers the next connection attempt.
 * esp_timer callbacks run in a dedicated high-priority task. */
static esp_timer_handle_t s_retry_timer;

/* Current retry delay. Normally touched only from the event loop task. The
 * timer task also updates it, on the rare path where a connect request is
 * refused outright. A 32-bit write is a single, indivisible instruction on this
 * CPU, so the worst a collision could do is give one retry the wrong delay. */
static uint32_t s_backoff_ms = BACKOFF_MIN_MS;

/* Set when the device is restarting, so the disconnect that shutdown causes
 * is not treated as a lost connection. `volatile` because it is written by the
 * restarting task and read by others. */
static volatile bool s_stopping;

/*
 * Turns the driver's numeric disconnect reason into advice. The numbers are
 * wifi_err_reason_t in ESP-IDF's esp_wifi_types_generic.h; the raw number is
 * logged alongside, so unlisted reasons can still be looked up.
 */
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
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:   /* a wrong WPA2 password usually shows as this */
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "authentication failed (check wifi_pass)";
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_ASSOC_EXPIRE:
        return "access point did not respond";
    case WIFI_REASON_BEACON_TIMEOUT:            /* router went quiet: switched off, or out of range */
        return "signal lost";
    case WIFI_REASON_ASSOC_LEAVE:
        return "disassociated";
    case WIFI_REASON_CONNECTION_FAIL:
        return "connection failed";
    default:
        return "see wifi_err_reason_t in esp_wifi_types_generic.h";
    }
}

/* Arms the retry timer with the current delay, then doubles the delay for
 * next time, up to the maximum. */
static void schedule_retry(void)
{
    if (s_stopping) {
        return;
    }
    /* esp_timer works in microseconds, hence * 1000 */
    esp_err_t err = esp_timer_start_once(s_retry_timer, (uint64_t)s_backoff_ms * 1000);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {    /* INVALID_STATE: already armed */
        ESP_LOGE(TAG, "cannot schedule retry: %s", esp_err_to_name(err));
    }
    s_backoff_ms = s_backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : s_backoff_ms * 2;
}

/* Starts a connection attempt. Called when the retry timer fires (timer
 * task), and directly on WIFI_EVENT_STA_START (event loop task). */
static void retry_cb(void *arg)
{
    if (s_stopping) {
        return;
    }
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        /* The attempt was refused before it began, so no disconnect event will
         * follow to schedule the next one. Re-arm here, or retrying stops. */
        ESP_LOGW(TAG, "connect request failed: %s; retrying in %lu s",
                 esp_err_to_name(err), (unsigned long)(s_backoff_ms / 1000));
        schedule_retry();
    }
}

/* Handler for all WIFI_EVENT events; runs in the event loop task. */
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START) {
        retry_cb(NULL);     /* first attempt, straight away */
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        if (s_stopping) {
            /* Caused by our own restart; do not retry */
            ESP_LOGI(TAG, "disconnected for shutdown");
            return;
        }
        ESP_LOGW(TAG, "disconnected, reason %d: %s; retrying in %lu s",
                 ev->reason, reason_str(ev->reason), (unsigned long)(s_backoff_ms / 1000));
        schedule_retry();
    }
}

/* Handler for IP_EVENT_STA_GOT_IP: the connection is fully usable. */
static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *ev = data;
    wifi_ap_record_t ap = { 0 };
    esp_wifi_sta_get_ap_info(&ap);      /* zeroed fields on failure are harmless in a log */

    /* RSSI is received signal strength in dBm: about -30 is excellent,
     * -67 is fine for streaming, below -80 is unreliable. */
    ESP_LOGI(TAG, "connected: ip " IPSTR ", rssi %d dBm, channel %d",
             IP2STR(&ev->ip_info.ip), ap.rssi, ap.primary);
    s_backoff_ms = BACKOFF_MIN_MS;      /* the next outage starts again at 1 s */
}

/* Runs on esp_restart() before the WiFi driver's own shutdown handler,
 * because shutdown handlers run in reverse order of registration and this
 * one is registered after the driver's (see net_start). */
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

    /* Creates the network interface WiFi attaches to. As a side effect this
     * registers the driver's own shutdown handler, so ours, registered next,
     * runs before it. */
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(netif, ESP_FAIL, TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_register_shutdown_handler(on_shutdown), TAG, "shutdown handler");
    /* The name the router shows in its client list */
    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(netif, hostname), TAG, "hostname");

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL),
                        TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL),
                        TAG, "ip handler");

    wifi_config_t wc = { 0 };
    /* The driver's ssid and password fields are fixed-size byte arrays, and a
     * value that fills one completely (a 32-character SSID, a 64-digit key)
     * has no terminating NUL. strnlen caps the copy at the field size. */
    memcpy(wc.sta.ssid, ssid, strnlen(ssid, sizeof(wc.sta.ssid)));
    memcpy(wc.sta.password, pass, strnlen(pass, sizeof(wc.sta.password)));
    /* Weakest security accepted: WPA2 when a password is set. WPA3 networks
     * are stronger and therefore also accepted; WEP and WPA1 are refused. */
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    /* WPA3 has two ways of deriving its key; allow both. Leaving this at zero
     * would allow only the older one, which some WPA3-only routers refuse. */
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    /* Protected management frames: use them if the router supports them */
    wc.sta.pmf_cfg.capable = true;

    /* By default the driver saves its configuration to NVS as well. The
     * credentials are already stored by app_config; keep a second copy out. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "config");
    /* Returns at once; WIFI_EVENT_STA_START follows and starts connecting */
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");

    ESP_LOGI(TAG, "joining '%s' as %s", ssid, hostname);
    return ESP_OK;
}
