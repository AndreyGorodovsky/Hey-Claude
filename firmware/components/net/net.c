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
 * Settings that have never connected are given up after 5 refusals of the
 * password in a row, or 10 minutes without a connection (net.h). The two
 * limits differ because the evidence does. A refused password says the
 * password is wrong, plainly, so a few refusals are enough. "Network not
 * found" is what a mistyped name looks like, and equally what a router
 * still starting after a power cut looks like, so it is waited out far
 * longer than a router takes to start.
 *
 * Threading: event handlers run in the event loop task; the retry and
 * give-up callbacks run in the esp_timer task. The notes on each static
 * variable below explain why they need no locks.
 */
#include "net.h"

#include <stdbool.h>
#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_rom_crc.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"

static const char *TAG = "net";

#define BACKOFF_MIN_MS      1000    /* first retry delay */
#define BACKOFF_MAX_MS      30000   /* retry delay stops growing here */

/* When settings that have never connected are given up (see the top of
 * this file). AUTH_EXPIRE is not counted as a refusal: some routers send it
 * routinely at the first attempt and then let the device in. */
#define REFUSALS_MAX        5
#define NEVER_CONNECTED_MS  (10 * 60 * 1000)

/* Where the checksum of the settings that last connected is kept */
#define NVS_NAMESPACE       "net"
#define NVS_KEY_PROVEN      "proven"

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

/* For giving up on settings that never work. Set at start and then written
 * only in the event loop task, except s_given_up, which the give-up timer
 * also writes; a bool is written in one instruction, and the worst a
 * collision could do is report twice, which only restarts the chip sooner. */
static void (*s_on_hopeless)(net_hopeless_t why);
static esp_timer_handle_t s_give_up_timer;
static uint32_t s_checksum;         /* of these settings */
static bool s_proven;               /* these settings have connected, on some boot */
static bool s_connected;            /* they have connected on this boot */
static int s_refusals;              /* password refusals in a row */
static bool s_last_refused;         /* the last failure was a refusal */
static volatile bool s_given_up;

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

/* Whether a disconnect reason says the password was wrong */
static bool is_refusal(uint8_t reason)
{
    return reason == WIFI_REASON_AUTH_FAIL
        || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
        || reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
}

static void give_up(net_hopeless_t why)
{
    if (s_given_up) {
        return;
    }
    s_given_up = true;
    ESP_LOGW(TAG, "giving up on settings that have never connected: %s",
             why == NET_HOPELESS_REFUSED ? "the password keeps being refused"
                                         : "no connection in 10 minutes");
    s_on_hopeless(why);
}

/* The give-up timer: runs out NEVER_CONNECTED_MS after start unless a
 * connection stops it first. esp_timer task. */
static void give_up_cb(void *arg)
{
    give_up(s_last_refused ? NET_HOPELESS_REFUSED : NET_HOPELESS_NOT_FOUND);
}

/* Remembers that these settings work, once: a write to flash at the first
 * connection with new settings, and never again for them */
static void remember_proven(void)
{
    if (s_proven) {
        return;
    }
    s_proven = true;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u32(h, NVS_KEY_PROVEN, s_checksum);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        /* Only costs this: after a restart, settings that do work could be
         * given up if their network were away for 10 minutes */
        ESP_LOGW(TAG, "could not note that the settings work: %s", esp_err_to_name(err));
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
        s_last_refused = is_refusal(ev->reason);
        s_refusals = s_last_refused ? s_refusals + 1 : 0;
        if (!s_proven && !s_connected && s_refusals >= REFUSALS_MAX) {
            give_up(NET_HOPELESS_REFUSED);
            return;
        }
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
    s_connected = true;
    s_refusals = 0;
    esp_timer_stop(s_give_up_timer);    /* an error if not running, which is fine */
    remember_proven();
}

/* Runs on esp_restart() before the WiFi driver's own shutdown handler,
 * because shutdown handlers run in reverse order of registration and this
 * one is registered after the driver's (see net_start). */
static void on_shutdown(void)
{
    s_stopping = true;
    esp_timer_stop(s_retry_timer);
}

/* A checksum of the settings, so that NVS can say whether these ones have
 * connected without holding a second copy of the password. CRC32 is a
 * checksum, not a secret-keeping hash: it tells settings apart, which is
 * all that is asked of it. */
static uint32_t settings_checksum(const char *ssid, const char *pass)
{
    uint32_t crc = esp_rom_crc32_le(0, (const uint8_t *)ssid, strlen(ssid) + 1);
    return esp_rom_crc32_le(crc, (const uint8_t *)pass, strlen(pass));
}

/* Whether these settings have connected on an earlier boot */
static bool load_proven(uint32_t checksum)
{
    nvs_handle_t h;
    uint32_t stored = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;                   /* nothing stored yet */
    }
    esp_err_t err = nvs_get_u32(h, NVS_KEY_PROVEN, &stored);
    nvs_close(h);
    return err == ESP_OK && stored == checksum;
}

esp_err_t net_start(const char *ssid, const char *pass, const char *hostname,
                    void (*on_hopeless)(net_hopeless_t why))
{
    ESP_RETURN_ON_FALSE(!s_started, ESP_ERR_INVALID_STATE, TAG, "already started");
    ESP_RETURN_ON_FALSE(on_hopeless != NULL, ESP_ERR_INVALID_ARG, TAG, "no on_hopeless");
    s_started = true;
    s_on_hopeless = on_hopeless;
    s_checksum = settings_checksum(ssid, pass);
    s_proven = load_proven(s_checksum);

    /* Resources are not released on failure: net_start runs once at boot, and a
     * failure leaves the device on the console for the settings to be fixed. */
    const esp_timer_create_args_t timer_args = { .callback = retry_cb, .name = "wifi_retry" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_retry_timer), TAG, "retry timer");
    const esp_timer_create_args_t give_up_args = { .callback = give_up_cb, .name = "wifi_give_up" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&give_up_args, &s_give_up_timer), TAG, "give-up timer");
    if (!s_proven) {
        /* esp_timer counts in microseconds */
        ESP_RETURN_ON_ERROR(esp_timer_start_once(s_give_up_timer,
                                                 (uint64_t)NEVER_CONNECTED_MS * 1000),
                            TAG, "give-up timer");
    }

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
    /* Power saving off. By default the radio sleeps between the router's
     * beacons, about ten times a second, and data in either direction can
     * wait up to 100 ms for it to wake. That saves power on a battery; this
     * device is mains-powered and streams audio, where such waits are heard
     * as gaps and can stall a send for long enough to lose the connection. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "power save");
    /* Returns at once; WIFI_EVENT_STA_START follows and starts connecting */
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");

    ESP_LOGI(TAG, "joining '%s' as %s; these settings have %s connected", ssid, hostname,
             s_proven ? "already" : "never");
    return ESP_OK;
}
