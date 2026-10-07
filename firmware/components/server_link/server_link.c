/*
 * The connection to the server. See server_link.h for what it offers; this file is
 * how it works.
 *
 * Two tasks are involved.
 *
 * The link task, created here, owns the connection's life. It runs a simple
 * loop for as long as the device is on: wait for WiFi, find the server,
 * connect, wait until that connection ends, tidy up, wait a while, repeat.
 *
 * The WebSocket client's task belongs to ESP-IDF's esp_websocket_client
 * component, which does the networking. It is created for each connection.
 * The client calls on_ws_event() below for everything that happens on the
 * connection. That function translates: server messages become reports to
 * whoever started the link, audio goes to the sink, and anything that ends
 * the connection is passed to the link task through a queue (s_inbox),
 * because the client must not be shut down from inside its own task.
 *
 * on_ws_event() runs in whichever task the client was in when the thing
 * happened. For everything received, that is the client's own task. But
 * when a send fails, the client reports the failure from inside the send,
 * so on_ws_event() then runs in the task that was sending: the upload task.
 * It therefore only does things that are safe from any task.
 *
 * The client can reconnect by itself, but that is switched off: the server
 * may have moved, so every attempt must start again from the lookup, and
 * the rest of the firmware must hear about every loss.
 *
 * Both tasks run on core 0, with the WiFi driver and the network stack
 * (ARCHITECTURE.md, task and core allocation).
 */
#include "server_link.h"

#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "protocol.h"

static const char *TAG = "server_link";

/* ---- Timing, in milliseconds. The protocol's own figures are in
 * protocol.h; these are this file's alone. ---- */

/* How long one mDNS lookup listens for an answer */
#define LOOKUP_MS           3000
/* The wait the client is given for the network to take one outgoing frame.
 * Generous on purpose: when it runs out, the client gives up the whole
 * connection, and a WiFi hiccup of a few hundred milliseconds is ordinary.
 * Audio waiting to be sent is safe meanwhile, in the microphone's ring.
 * It is not a bound on how long a send can take: the client applies it
 * more than once inside one send, and waits for its own lock besides, so a
 * send on a stalled network can take several times this. */
#define SEND_MS             1500
/* Limit on the client's own network operations, such as connecting. Also
 * bounds how long the client's task can stay stuck in a read after its
 * connection has been torn down under it. */
#define NETWORK_MS          5000
/* How long a lookup of the server's address by its host name may take */
#define ADDRESS_LOOKUP_MS   2000
/* TCP keepalive, in seconds: after this long with nothing received, the
 * network stack itself asks the other end whether it is still there, this
 * often, and ends the connection after this many unanswered tries. It
 * catches a server machine that has frozen or lost power in the middle of
 * a message, which the client's own ping does not while it is reading. */
#define KEEPALIVE_IDLE_S    5
#define KEEPALIVE_EVERY_S   5
#define KEEPALIVE_TRIES     3

/* ---- Tasks ---- */

/* Stack sizes in bytes. The client's is above its default because its
 * event handler parses JSON and copies audio into the player. */
#define LINK_TASK_STACK     4096
#define WS_TASK_STACK       6144
/* Priorities are recorded in ARCHITECTURE.md's task table */
#define LINK_TASK_PRIO      5
#define WS_TASK_PRIO        7
#define NETWORK_CORE        0
/* The client's receive buffer, in bytes. A WebSocket message larger than
 * this is delivered in pieces; reply audio chunks are at most 1920 bytes at
 * 24 kHz, so each arrives whole. */
#define WS_BUFFER_BYTES     4096

/* Longest text message from the server that is kept; they are all short */
#define TEXT_MAX            256

/* WebSocket frame types ("opcodes"), from the WebSocket standard, RFC 6455 */
#define OP_CONTINUATION     0x0
#define OP_TEXT             0x1
#define OP_BINARY           0x2

/* What the client's task and other tasks tell the link task */
typedef enum {
    MSG_READY,      /* the server sent `ready` */
    MSG_GONE,       /* the connection ended or failed; `reason` says how */
    MSG_DROP,       /* server_link_drop() was called */
} msg_kind_t;

typedef struct {
    msg_kind_t kind;
    server_link_down_t reason;
} msg_t;

/* Set once at start and read-only afterwards */
static char s_server_url[128];
static char s_headers[192];
static server_link_sink_t s_sink;
static server_link_report_fn s_report;

static QueueHandle_t s_inbox;
/* An event group is a set of flags tasks can wait on. One flag here: set
 * while WiFi has an address. */
static EventGroupHandle_t s_flags;
#define FLAG_WIFI           BIT0

/* The live client, or NULL. The mutex is held by anyone using it: a sender
 * while it sends, the link task while it shuts the client down. That keeps
 * a send from using a client that is being destroyed. */
static SemaphoreHandle_t s_client_lock;
static esp_websocket_client_handle_t s_client;
/* True between `ready` and the end of the connection. Written by the link
 * task only; a plain bool is safe to read from other tasks on this chip. */
static volatile bool s_up;

/* A text message being put together from pieces, and the type of the
 * frame under way. Used only for received data, so only in the client's
 * own task. */
static char s_text[TEXT_MAX];
static size_t s_text_len;
static uint8_t s_frame_op;
/* The reason for the end of the connection, noted when it is learnt and
 * reported when it ends. Written from the client's task, and from a
 * sending task when a send fails; a single word, so safe either way. */
static volatile server_link_down_t s_end_reason;

/* For server_link_get_status(). The spinlock (a lock for very short sections)
 * keeps a reader from seeing half an update. */
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static server_link_status_t s_status;

/* Posts to the link task. Never blocks: called from the client's task and
 * from event handlers, which must not wait. The queue has room for every
 * message one connection can produce. */
static void tell(msg_kind_t kind, server_link_down_t reason)
{
    msg_t m = { .kind = kind, .reason = reason };
    if (xQueueSend(s_inbox, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "inbox full; a %d message was lost", (int)kind);
    }
}

/* Tells whoever started the link what happened */
static void report(server_link_report_t what, uint32_t value)
{
    s_report(what, value);
}

/* Whether a `reply_start` describes audio this firmware can play. The
 * fields other than the rate have one value in this protocol version; a
 * message that leaves them out is taken to mean that value. */
static bool reply_format_ok(const cJSON *root, uint32_t *hz)
{
    const cJSON *rate = cJSON_GetObjectItemCaseSensitive(root, "sample_rate");
    const cJSON *format = cJSON_GetObjectItemCaseSensitive(root, "format");
    const cJSON *channels = cJSON_GetObjectItemCaseSensitive(root, "channels");
    /* Compared as a double first: converting a negative or huge number to
     * an unsigned integer has no defined result in C */
    if (!cJSON_IsNumber(rate) || rate->valuedouble < PROTO_REPLY_RATE_MIN_HZ
        || rate->valuedouble > PROTO_REPLY_RATE_MAX_HZ) {
        return false;
    }
    if (cJSON_IsString(format) && strcmp(format->valuestring, "pcm_s16le") != 0) {
        return false;
    }
    if (cJSON_IsNumber(channels) && channels->valuedouble != 1) {
        return false;
    }
    *hz = (uint32_t)rate->valuedouble;
    return true;
}

/* ---- Messages from the server ---- */

static server_link_error_t error_from_code(const char *code)
{
    if (code == NULL)                       return SERVER_LINK_ERROR_UNKNOWN;
    if (strcmp(code, "stt_failed") == 0)    return SERVER_LINK_ERROR_STT;
    if (strcmp(code, "llm_failed") == 0)    return SERVER_LINK_ERROR_LLM;
    if (strcmp(code, "tts_failed") == 0)    return SERVER_LINK_ERROR_TTS;
    if (strcmp(code, "internal") == 0)      return SERVER_LINK_ERROR_INTERNAL;
    return SERVER_LINK_ERROR_UNKNOWN;
}

/* Handles one complete text message: a JSON object with a "type". Unknown
 * types and unknown fields are ignored, as the protocol requires, so that a
 * newer server can add them. */
static void on_text(const char *text, size_t len)
{
    /* cJSON builds a tree of the message on the heap; cJSON_Delete frees it */
    cJSON *root = cJSON_ParseWithLength(text, len);
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type)) {
        ESP_LOGW(TAG, "server sent a message with no type");
        cJSON_Delete(root);
        return;
    }
    const char *t = type->valuestring;
    if (strcmp(t, "ready") == 0) {
        /* A server speaking another version of the protocol cannot be
         * talked to safely: the same messages may mean something else */
        const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "protocol");
        if (cJSON_IsNumber(version) && version->valuedouble == PROTO_VERSION) {
            tell(MSG_READY, 0);
        } else {
            ESP_LOGE(TAG, "the server speaks protocol version %d; this firmware speaks %d",
                     cJSON_IsNumber(version) ? (int)version->valuedouble : 0, PROTO_VERSION);
            tell(MSG_GONE, SERVER_LINK_DOWN_INCOMPATIBLE);
        }
    } else if (strcmp(t, "stop_capture") == 0) {
        report(SERVER_LINK_STOP_CAPTURE, 0);
    } else if (strcmp(t, "reply_start") == 0) {
        uint32_t hz = 0;
        if (reply_format_ok(root, &hz)) {
            /* The sink first, here in the client's task, so that it is
             * ready before the audio that follows this message */
            s_sink.start(hz);
            report(SERVER_LINK_REPLY_START, hz);
        } else {
            /* Audio in a form that cannot be played. The protocol's answer
             * to anything the device cannot make sense of: start afresh. */
            ESP_LOGE(TAG, "the server announced reply audio this device cannot play");
            tell(MSG_GONE, SERVER_LINK_DOWN_CONFUSED);
        }
    } else if (strcmp(t, "reply_end") == 0) {
        report(SERVER_LINK_REPLY_END, 0);
    } else if (strcmp(t, "error") == 0) {
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
        server_link_error_t e = error_from_code(cJSON_IsString(code) ? code->valuestring : NULL);
        /* The server's sentence is for this log; the screen shows the
         * device's own wording for the code */
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");
        ESP_LOGW(TAG, "server error %s: %s",
                 cJSON_IsString(code) ? code->valuestring : "?",
                 cJSON_IsString(message) ? message->valuestring : "");
        report(SERVER_LINK_SERVER_ERROR, e);
    } else {
        ESP_LOGW(TAG, "server sent an unknown message type");
    }
    cJSON_Delete(root);
}

/* Handles one piece of incoming data. A WebSocket message can arrive split
 * two ways: as several frames (the first carries the type, the rest are
 * "continuation" frames), and a frame too large for the receive buffer
 * comes as several events. Audio is passed on piece by piece; text is put
 * together first. */
static void on_data(const esp_websocket_event_data_t *d)
{
    if (d->op_code == OP_TEXT || d->op_code == OP_BINARY) {
        s_frame_op = d->op_code;
        if (d->payload_offset == 0) {
            s_text_len = 0;
        }
    } else if (d->op_code != OP_CONTINUATION) {
        return;     /* ping, pong, close: the client deals with those */
    }

    if (s_frame_op == OP_BINARY) {
        if (d->data_len > 0) {
            s_sink.data((const uint8_t *)d->data_ptr, (size_t)d->data_len);
        }
        return;
    }
    if (s_frame_op != OP_TEXT) {
        return;
    }
    if (s_text_len + (size_t)d->data_len > sizeof(s_text)) {
        ESP_LOGW(TAG, "server sent an oversized text message; ignored");
        s_text_len = sizeof(s_text) + 1;    /* poisons the rest of it */
        return;
    }
    memcpy(s_text + s_text_len, d->data_ptr, (size_t)d->data_len);
    s_text_len += (size_t)d->data_len;
    /* Complete when this piece reaches the end of its frame, and the frame
     * is the last of its message */
    bool frame_done = d->payload_offset + d->data_len >= d->payload_len;
    if (frame_done && d->fin) {
        on_text(s_text, s_text_len);
        s_text_len = 0;
    }
}

/* Called by the client for everything that happens on the connection:
 * in its own task for what is received, and in the sending task when a
 * send fails (see the top of this file) */
static void on_ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const esp_websocket_event_data_t *d = data;
    switch (id) {
    case WEBSOCKET_EVENT_BEFORE_CONNECT:
        s_end_reason = SERVER_LINK_DOWN_LOST;
        s_text_len = 0;
        s_frame_op = 0;
        break;
    case WEBSOCKET_EVENT_DATA:
        on_data(d);
        break;
    case WEBSOCKET_EVENT_ERROR:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_FINISH:
        /* The server answers the opening request with an HTTP status, and
         * 403 is its refusal of the device's identifier and token. Only
         * the status is looked at: the client files a refused request
         * under a general connection failure, not under a kind of its own. */
        if (d != NULL && d->error_handle.esp_ws_handshake_status_code == PROTO_HTTP_REFUSED) {
            s_end_reason = SERVER_LINK_DOWN_REJECTED;
        }
        if (d != NULL && d->close_status_code == PROTO_CLOSE_REPLACED) {
            s_end_reason = SERVER_LINK_DOWN_REPLACED;
        }
        if (id == WEBSOCKET_EVENT_ERROR) {
            break;      /* the event that ends the connection follows */
        }
        /* More than one of these can follow a single ending; the link task
         * acts on the first and discards the rest */
        tell(MSG_GONE, s_end_reason);
        break;
    default:
        break;
    }
}

/* ---- WiFi ---- */

/* Runs in the default event loop's task */
static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_flags, FLAG_WIFI);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_flags, FLAG_WIFI);
        /* A connection over a network that has gone would only be noticed
         * at the next ping timeout; this ends it now. Only while there is
         * a client: WiFi repeats this event for every failed attempt to
         * rejoin, and nobody would be reading them. */
        if (s_client != NULL) {
            tell(MSG_GONE, SERVER_LINK_DOWN_LOST);
        }
    }
}

/* ---- Finding the server ---- */

/* Writes the WebSocket address to try into `uri`, and "host:port" into
 * `shown` for the status. Returns false if no server was found. */
static bool resolve(char *uri, size_t uri_size, char *shown, size_t shown_size)
{
    if (s_server_url[0] != '\0') {
        /* A configured address. "host:port" is accepted as well as a full
         * "ws://host:port/path": what is missing is filled in. */
        const char *rest = strstr(s_server_url, "://");
        const char *authority = rest ? rest + 3 : s_server_url;
        bool has_path = strchr(authority, '/') != NULL;
        snprintf(uri, uri_size, "%s%s%s", rest ? "" : "ws://", s_server_url,
                 has_path ? "" : PROTO_STREAM_PATH);
        snprintf(shown, shown_size, "%.*s", (int)strcspn(authority, "/"), authority);
        return true;
    }

    /* Ask the network for the service. The answer is a list this function
     * must free; one entry is enough. */
    mdns_result_t *found = NULL;
    esp_err_t err = mdns_query_ptr(PROTO_SERVICE_TYPE, PROTO_SERVICE_PROTO, LOOKUP_MS, 1, &found);
    if (err != ESP_OK || found == NULL) {
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "mDNS lookup failed: %s", esp_err_to_name(err));
        }
        return false;
    }
    /* The answer names the service, its host and its port, and usually the
     * host's address as well. Usually, not always: a responder may leave
     * the address out of an answer it has given recently, expecting the
     * asker to remember it. Then the address is asked for by host name. */
    esp_ip4_addr_t address = { 0 };
    bool ok = false;
    for (const mdns_ip_addr_t *a = found->addr; a != NULL; a = a->next) {
        if (a->addr.type == ESP_IPADDR_TYPE_V4) {
            address = a->addr.u_addr.ip4;
            ok = true;
            break;
        }
    }
    if (!ok && found->hostname != NULL) {
        err = mdns_query_a(found->hostname, ADDRESS_LOOKUP_MS, &address);
        ok = err == ESP_OK;
        if (!ok) {
            ESP_LOGW(TAG, "server found, but not its address: %s", esp_err_to_name(err));
        }
    }
    if (ok) {
        const char *path = PROTO_STREAM_PATH;
        for (size_t i = 0; i < found->txt_count; i++) {
            if (strcmp(found->txt[i].key, "path") == 0 && found->txt[i].value != NULL) {
                path = found->txt[i].value;
            }
        }
        /* IPSTR and IP2STR are ESP-IDF's helpers for printing an address */
        snprintf(shown, shown_size, IPSTR ":%u", IP2STR(&address), found->port);
        snprintf(uri, uri_size, "ws://%s%s", shown, path);
    }
    mdns_query_results_free(found);
    return ok;
}

/* ---- One connection, from attempt to end ---- */

static void set_status_server(const char *shown)
{
    taskENTER_CRITICAL(&s_status_lock);
    strlcpy(s_status.server, shown, sizeof(s_status.server));
    s_status.attempts++;
    taskEXIT_CRITICAL(&s_status_lock);
}

/* No `default`: the compiler then warns when a reason is added to the
 * enum and not here */
const char *server_link_down_name(server_link_down_t reason)
{
    switch (reason) {
    case SERVER_LINK_DOWN_NOT_FOUND:       return "no server found on the network";
    case SERVER_LINK_DOWN_UNREACHABLE:     return "the server did not answer";
    case SERVER_LINK_DOWN_REJECTED:        return "the server refused this device's identifier and token";
    case SERVER_LINK_DOWN_INCOMPATIBLE:    return "the server speaks another protocol version";
    case SERVER_LINK_DOWN_REPLACED:        return "another connection used this device's identifier";
    case SERVER_LINK_DOWN_LOST:            return "the connection was lost";
    case SERVER_LINK_DOWN_CONFUSED:        return "the server sent something this device cannot use";
    case SERVER_LINK_DOWN_DROPPED:         return "the device closed it to abandon a turn";
    }
    return "?";
}

/* Tells the rest of the firmware that there is no connection, and why */
static server_link_down_t announce_down(server_link_down_t reason)
{
    s_up = false;
    taskENTER_CRITICAL(&s_status_lock);
    s_status.up = false;
    s_status.last_down = reason;
    s_status.had_down = true;
    taskEXIT_CRITICAL(&s_status_lock);
    ESP_LOGW(TAG, "down: %s", server_link_down_name(reason));
    report(SERVER_LINK_DOWN, reason);
    return reason;
}

/* Tries to connect, and if that works, stays until the connection ends.
 * Returns why there is no connection any more, having announced it. */
static server_link_down_t run_connection(void)
{
    char uri[192];
    char shown[sizeof(s_status.server)] = "";
    if (!resolve(uri, sizeof(uri), shown, sizeof(shown))) {
        set_status_server("");
        return announce_down(SERVER_LINK_DOWN_NOT_FOUND);
    }
    set_status_server(shown);
    ESP_LOGI(TAG, "connecting to %s", shown);

    const esp_websocket_client_config_t config = {
        .uri = uri,
        /* Sent with the opening request: who the device is, and its proof */
        .headers = s_headers,
        .disable_auto_reconnect = true,
        .network_timeout_ms = NETWORK_MS,
        .ping_interval_sec = PROTO_DEVICE_PING_INTERVAL_S,
        .pingpong_timeout_sec = PROTO_DEVICE_PONG_TIMEOUT_S,
        .keep_alive_enable = true,
        .keep_alive_idle = KEEPALIVE_IDLE_S,
        .keep_alive_interval = KEEPALIVE_EVERY_S,
        .keep_alive_count = KEEPALIVE_TRIES,
        .buffer_size = WS_BUFFER_BYTES,
        .task_name = "server_link_ws",
        .task_stack = WS_TASK_STACK,
        .task_prio = WS_TASK_PRIO,
        .task_core_id_set = true,
        .task_core_id = NETWORK_CORE,
    };
    esp_websocket_client_handle_t client = esp_websocket_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "could not create the WebSocket client");
        return announce_down(SERVER_LINK_DOWN_UNREACHABLE);
    }
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, on_ws_event, NULL);

    /* Anything still queued belongs to an earlier connection */
    xQueueReset(s_inbox);
    xSemaphoreTake(s_client_lock, portMAX_DELAY);
    s_client = client;
    xSemaphoreGive(s_client_lock);

    server_link_down_t reason = SERVER_LINK_DOWN_UNREACHABLE;
    if (esp_websocket_client_start(client) == ESP_OK) {
        bool up = false;
        for (;;) {
            msg_t m;
            /* Until `ready`, the wait is limited; after it, the connection
             * lasts until something ends it */
            TickType_t wait = up ? portMAX_DELAY : pdMS_TO_TICKS(PROTO_DEVICE_READY_WAIT_MS);
            if (xQueueReceive(s_inbox, &m, wait) != pdTRUE) {
                ESP_LOGW(TAG, "no `ready` from the server in time");
                break;
            }
            if (m.kind == MSG_READY && !up) {
                up = true;
                s_up = true;
                taskENTER_CRITICAL(&s_status_lock);
                s_status.up = true;
                s_status.connections++;
                taskEXIT_CRITICAL(&s_status_lock);
                ESP_LOGI(TAG, "connected");
                report(SERVER_LINK_UP, 0);
            } else if (m.kind == MSG_GONE) {
                /* A connection that never became ready was not "lost" */
                reason = (!up && m.reason == SERVER_LINK_DOWN_LOST) ? SERVER_LINK_DOWN_UNREACHABLE
                                                             : m.reason;
                break;
            } else if (m.kind == MSG_DROP && up) {
                reason = SERVER_LINK_DOWN_DROPPED;
                break;
            }
        }
    }

    /* Announced before the tidying up below, which can take seconds: the
     * state machine must not go on believing in a connection that is gone. */
    announce_down(reason);
    /* Taking the lock waits for any send under way to return. Stopping
     * waits for the client's task to end, which is why it is done here and
     * not in that task; that task may first have to give up on a read.
     * Stop fails harmlessly if the task has ended itself. */
    xSemaphoreTake(s_client_lock, portMAX_DELAY);
    s_client = NULL;
    xSemaphoreGive(s_client_lock);
    esp_websocket_client_stop(client);
    esp_websocket_client_destroy(client);
    return reason;
}

static void link_task(void *arg)
{
    uint32_t backoff_ms = PROTO_DEVICE_BACKOFF_FIRST_MS;
    for (;;) {
        xEventGroupWaitBits(s_flags, FLAG_WIFI, pdFALSE, pdTRUE, portMAX_DELAY);

        uint32_t before = s_status.connections;
        server_link_down_t reason = run_connection();
        bool worked = s_status.connections != before;


        if (reason == SERVER_LINK_DOWN_REJECTED) {
            /* Trying again with the same identifier and token cannot
             * succeed. The settings are changed from the console, and take
             * effect at the reboot that follows. */
            ESP_LOGE(TAG, "the server refused this device. Check device_token, and "
                          "that the server lists this device, then reboot");
            vTaskSuspend(NULL);
        }
        uint32_t wait_ms;
        if (reason == SERVER_LINK_DOWN_REPLACED) {
            wait_ms = PROTO_DEVICE_BACKOFF_REPLACED_MS;
        } else if (reason == SERVER_LINK_DOWN_INCOMPATIBLE) {
            /* Only an update of the server or of this firmware changes
             * that; looked at again now and then, in case the server's did */
            wait_ms = PROTO_DEVICE_BACKOFF_MAX_MS;
        } else if (worked) {
            /* It worked for a while: start again from the short wait */
            wait_ms = backoff_ms = PROTO_DEVICE_BACKOFF_FIRST_MS;
        } else {
            wait_ms = backoff_ms;
            backoff_ms = backoff_ms * 2 > PROTO_DEVICE_BACKOFF_MAX_MS
                             ? PROTO_DEVICE_BACKOFF_MAX_MS : backoff_ms * 2;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

/* ---- Public functions ---- */

esp_err_t server_link_start(const server_link_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(s_inbox == NULL, ESP_ERR_INVALID_STATE, TAG, "already started");
    ESP_RETURN_ON_FALSE(cfg && cfg->device_id && cfg->device_token
                        && cfg->sink.start && cfg->sink.data && cfg->on_report,
                        ESP_ERR_INVALID_ARG, TAG, "incomplete configuration");

    strlcpy(s_server_url, cfg->server_url ? cfg->server_url : "", sizeof(s_server_url));
    /* Each header line ends with a carriage return and a line feed, as HTTP
     * requires */
    int n = snprintf(s_headers, sizeof(s_headers),
                     "Authorization: Bearer %s\r\nX-Device-Id: %s\r\n",
                     cfg->device_token, cfg->device_id);
    ESP_RETURN_ON_FALSE(n > 0 && (size_t)n < sizeof(s_headers), ESP_ERR_INVALID_SIZE,
                        TAG, "identifier and token too long");
    s_sink = cfg->sink;
    s_report = cfg->on_report;

    s_inbox = xQueueCreate(8, sizeof(msg_t));
    s_flags = xEventGroupCreate();
    s_client_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_inbox && s_flags && s_client_lock, ESP_ERR_NO_MEM, TAG, "no memory");

    /* Starts the mDNS service, which is needed even just to ask */
    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mdns");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   on_wifi, NULL), TAG, "ip handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                   on_wifi, NULL), TAG, "wifi handler");

    BaseType_t made = xTaskCreatePinnedToCore(link_task, "server_link", LINK_TASK_STACK, NULL,
                                              LINK_TASK_PRIO, NULL, NETWORK_CORE);
    return made == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

/* Sends one frame, text or binary, if there is a connection */
static esp_err_t send_frame(bool text, const char *data, int len)
{
    if (!s_up) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_client_lock, pdMS_TO_TICKS(SEND_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    int sent = -1;
    /* Checked again under the lock: the connection may have just ended */
    if (s_client != NULL && s_up) {
        sent = text ? esp_websocket_client_send_text(s_client, data, len,
                                                     pdMS_TO_TICKS(SEND_MS))
                    : esp_websocket_client_send_bin(s_client, data, len,
                                                    pdMS_TO_TICKS(SEND_MS));
    }
    xSemaphoreGive(s_client_lock);
    return sent == len ? ESP_OK : ESP_FAIL;
}

esp_err_t server_link_send_utterance_start(void)
{
    static const char message[] = "{\"type\":\"utterance_start\"}";
    return send_frame(true, message, (int)strlen(message));
}

esp_err_t server_link_send_audio(const int16_t *samples, size_t count)
{
    /* The chip stores numbers least significant byte first, which is the
     * byte order the protocol asks for, so the samples go as they are */
    return send_frame(false, (const char *)samples, (int)(count * sizeof(int16_t)));
}

void server_link_drop(void)
{
    if (s_up) {
        tell(MSG_DROP, SERVER_LINK_DOWN_DROPPED);
    }
}

void server_link_get_status(server_link_status_t *out)
{
    taskENTER_CRITICAL(&s_status_lock);
    *out = s_status;
    taskEXIT_CRITICAL(&s_status_lock);
    /* s_flags does not exist until server_link_start() */
    out->wifi = s_flags != NULL && (xEventGroupGetBits(s_flags) & FLAG_WIFI) != 0;
}
