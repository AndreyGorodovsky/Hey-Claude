/*
 * The connection to the server: one WebSocket, opened once WiFi is up and
 * kept open, as docs/PROTOCOL.md describes.
 *
 * A WebSocket is a long-lived two-way connection carried over TCP. Each side
 * sends "frames": text frames, which here hold small JSON control messages,
 * and binary frames, which hold audio.
 *
 * This component finds the server, connects, proves who the device is, and
 * reconnects whenever the connection is lost. It translates the server's
 * messages into reports, and offers functions to send the device's. It
 * decides nothing about what the device does next: that belongs to the
 * state machine (the state_machine component), which receives the reports.
 *
 * Finding the server. With no `server_url` setting, the server is looked up
 * by mDNS (multicast DNS): the device asks the local network "who offers
 * the _hey-claude service?" and the server's machine answers with its
 * address and port. The lookup is repeated for every connection attempt, so
 * the server's address may change freely.
 *
 * Two things come out of the link, each through a function given at start.
 * Reports say what happened: connected, lost, a message from the server.
 * Reply audio goes to the sink. They are functions and not events on the
 * default event loop because each has exactly one receiver, and a function
 * call cannot be lost on the way or arrive out of order.
 *
 * Threading: server_link_start() is called once at boot. The send functions and
 * server_link_drop() may be called from any task. The report and sink functions
 * are called from the link's own two tasks; they must return quickly and
 * must not call back into the link.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* What the link reports, with the meaning of the value that comes with it */
typedef enum {
    SERVER_LINK_UP,            /* connected, and the server has sent `ready` */
    SERVER_LINK_DOWN,          /* not connected; value: a server_link_down_t */
    SERVER_LINK_STOP_CAPTURE,  /* server: the request is over, stop sending audio */
    SERVER_LINK_REPLY_START,   /* server: reply audio follows; value: its sample rate.
                           The sink's start() has already been called. */
    SERVER_LINK_REPLY_END,     /* server: all reply audio has been sent */
    SERVER_LINK_SERVER_ERROR,  /* server: the turn was abandoned; value: a server_link_error_t */
} server_link_report_t;

/* Why there is no connection. Reported once for every failed attempt and
 * every lost connection. */
typedef enum {
    SERVER_LINK_DOWN_NOT_FOUND,    /* no server answered the mDNS lookup */
    SERVER_LINK_DOWN_UNREACHABLE,  /* found or configured, but the connection failed */
    SERVER_LINK_DOWN_REJECTED,     /* the server refused the identifier and token
                               (HTTP 403). Not retried: it cannot succeed
                               until the settings change. */
    SERVER_LINK_DOWN_INCOMPATIBLE, /* the server speaks another protocol version */
    SERVER_LINK_DOWN_REPLACED,     /* the server closed this connection because
                               another one arrived with the same identifier */
    SERVER_LINK_DOWN_LOST,         /* a working connection ended */
    SERVER_LINK_DOWN_CONFUSED,     /* the link closed it: the server sent something
                               that cannot be used, such as reply audio in a
                               form this device cannot play */
    SERVER_LINK_DOWN_DROPPED,      /* closed on request, with server_link_drop() */
} server_link_down_t;

/* The `code` of the server's `error` message */
typedef enum {
    SERVER_LINK_ERROR_STT,         /* speech-to-text failed */
    SERVER_LINK_ERROR_LLM,         /* Claude failed */
    SERVER_LINK_ERROR_TTS,         /* text-to-speech failed */
    SERVER_LINK_ERROR_INTERNAL,    /* a fault in the server */
    SERVER_LINK_ERROR_UNKNOWN,     /* a code this firmware does not know */
} server_link_error_t;

typedef void (*server_link_report_fn)(server_link_report_t what, uint32_t value);

/* Where reply audio goes. Both functions run in the WebSocket client's
 * task, in the order the server sent things. */
typedef struct {
    /* A reply begins: audio at `sample_rate` samples per second follows.
     * The rate has been checked to be one the protocol allows. */
    void (*start)(uint32_t sample_rate);
    /* `len` bytes of reply audio: 16-bit signed little-endian mono samples.
     * A chunk may end in the middle of a sample; the next one completes it. */
    void (*data)(const uint8_t *pcm, size_t len);
} server_link_sink_t;

typedef struct {
    const char *server_url;     /* "ws://host:port", or empty to use mDNS */
    const char *device_id;
    const char *device_token;
    server_link_report_fn on_report;
    server_link_sink_t sink;
} server_link_config_t;

/* For the `link` console command */
typedef struct {
    bool wifi;                  /* the network is up */
    bool up;                    /* connected and ready */
    uint32_t attempts;          /* connection attempts since boot */
    uint32_t connections;       /* of those, how many reached `ready` */
    server_link_down_t last_down;      /* reason for the last SERVER_LINK_DOWN */
    bool had_down;              /* false until the first SERVER_LINK_DOWN */
    char server[64];            /* "host:port" of the last attempt, or empty */
} server_link_status_t;

/* Starts the task that keeps the connection. Call once, after the default
 * event loop and the network stack exist, and before WiFi is started: this
 * learns that the network is up from the event WiFi posts when it gets an
 * address, and would miss one posted earlier. The strings in `cfg` are
 * copied. Nothing is attempted until WiFi has an address. */
esp_err_t server_link_start(const server_link_config_t *cfg);

/* Tells the server a request begins: the wake word fired. Audio sent with
 * server_link_send_audio() from now on belongs to it. Blocks like server_link_send_audio(). */
esp_err_t server_link_send_utterance_start(void);

/* Sends `count` samples of request audio as one binary frame. Blocks while
 * the network takes them: normally not at all, but for several seconds when
 * the network stalls (server_link.c, SEND_MS). Returns ESP_ERR_INVALID_STATE when
 * there is no connection. A send that fails ends the connection. */
esp_err_t server_link_send_audio(const int16_t *samples, size_t count);

/* Closes the connection on purpose; it is then opened again like any lost
 * one. This is how the device abandons a turn: the protocol has no message
 * for it, and the server cancels the turn when the connection closes.
 * Followed by SERVER_LINK_DOWN with SERVER_LINK_DOWN_DROPPED. Does nothing when there is
 * no connection. */
void server_link_drop(void);

void server_link_get_status(server_link_status_t *out);

/* A short description of a reason, for logs and the console */
const char *server_link_down_name(server_link_down_t reason);
