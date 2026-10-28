/*
 * The device protocol's numbers: every value the firmware shares with the
 * server, or that must stand in a fixed relation to one of the server's.
 *
 * docs/PROTOCOL.md is the reference; this file is the firmware's copy of
 * its figures, and server/protocol.py is the server's. The two cannot share
 * a file, being in different languages, so a test on the server side
 * (server/tests/test_firmware_protocol.py) reads this header and checks it
 * against the server's values, and checks the rules noted below about
 * which limit must outlast which. For that test, each value here is a
 * plain number or string on a #define line of its own.
 *
 * Values that are the firmware's alone, such as stack sizes or how long an
 * error stays on the screen, do not belong here; they stay in the file
 * that uses them.
 *
 * A header only: this component has no code.
 */
#pragma once

/* The version this firmware speaks. The server states its own in `ready`. */
#define PROTO_VERSION                   1

/* ---- Finding and reaching the server ---- */

/* The mDNS service the server advertises, in the two parts mDNS uses */
#define PROTO_SERVICE_TYPE              "_hey-claude"
#define PROTO_SERVICE_PROTO             "_tcp"
/* Path of the WebSocket endpoint */
#define PROTO_STREAM_PATH               "/v1/stream"
/* HTTP status with which the server refuses a device's identifier and token */
#define PROTO_HTTP_REFUSED              403
/* WebSocket close code: another connection with this identifier took over */
#define PROTO_CLOSE_REPLACED            4000

/* ---- Audio ---- */

/* Request audio: samples per second, and the length of one frame */
#define PROTO_CAPTURE_RATE_HZ           16000
#define PROTO_CAPTURE_FRAME_MS          20
/* Reply audio: the rates a `reply_start` may state, and the longest chunk */
#define PROTO_REPLY_RATE_MIN_HZ         8000
#define PROTO_REPLY_RATE_MAX_HZ         48000
#define PROTO_REPLY_CHUNK_MS            40
/* The server sends at most this far ahead of playback */
#define PROTO_REPLY_LEAD_MS             2000
/* Reply audio the device has room for: at least twice the lead */
#define PROTO_REPLY_BUFFER_MS           4000

/* ---- Volume ----
 * The playback volume's scale. The device keeps its level, states it in
 * `utterance_start`, and is sent a new one in `volume`. What a level
 * sounds like is the device's own business (state_machine.c). */
#define PROTO_VOLUME_MIN                1
#define PROTO_VOLUME_MAX                10

/* ---- The device's identity ---- */

/* Lengths of the device token the server accepts, in characters */
#define PROTO_DEVICE_TOKEN_MIN          24
#define PROTO_DEVICE_TOKEN_MAX          64

/* ---- Pairing ----
 * A device without a token sends a short code, made on the server's
 * machine, once to this path in an HTTP POST, and is answered with a token
 * of its own (docs/PROTOCOL.md, Pairing). The server answers a wrong or
 * expired code with PROTO_HTTP_REFUSED. */
#define PROTO_PAIR_PATH                 "/v1/pair"
/* Digits in a pairing code */
#define PROTO_PAIR_CODE_DIGITS          6

/* ---- The device's deadlines ----
 * Each is a guard against a server that has stopped answering, and each is
 * longer than the server's own limit for the same wait, so that the
 * server's `error` message arrives first when a service fails. */

/* Longest capture. The server ends a capture after 13 s. */
#define PROTO_DEVICE_CAPTURE_LIMIT_MS   15000
/* Longest wait from `stop_capture` to the first sound of the reply. The
 * server sends audio or an error within 30 s. */
#define PROTO_DEVICE_THINKING_LIMIT_MS  40000
/* Longest wait for the turn's end, `reply_end` or `error`, after sending
 * `cancel`, and also the longest the device lets the sending itself take.
 * The server answers at once; the wait covers a stalled network, and a
 * server too old to know `cancel`, which ignores it. */
#define PROTO_DEVICE_CANCEL_LIMIT_MS    3000
/* Longest silence inside a reply. The server's services allow at most 39 s
 * between them: 24 s of silence from Claude, then 5 s to reach the
 * synthesiser and 10 s of silence from it. */
#define PROTO_DEVICE_STALL_LIMIT_MS     40000

/* ---- Keeping the connection ---- */

/* From starting to connect until `ready` must have arrived */
#define PROTO_DEVICE_READY_WAIT_MS      10000
/* The device pings this often and gives the connection up if no pong has
 * come back within the timeout. The server pings too, every 20 s. */
#define PROTO_DEVICE_PING_INTERVAL_S    10
#define PROTO_DEVICE_PONG_TIMEOUT_S     20
/* Wait before trying again: doubling from the first value to the second */
#define PROTO_DEVICE_BACKOFF_FIRST_MS   1000
#define PROTO_DEVICE_BACKOFF_MAX_MS     30000
/* Wait after being replaced. Long, because an immediate return would take
 * the connection back from the other device, which would take it back
 * again, without end. */
#define PROTO_DEVICE_BACKOFF_REPLACED_MS 60000
