# Device protocol

The reference for everything that passes between a device and the server.
The server's side is built and tested (stage 5); the device's side is built
in stage 6, against this document. Message shapes and constants are defined
in code in `server/protocol.py`, which follows this document.

Protocol version: **1**.

## Connection

One WebSocket per device, opened at boot and held open.

```
GET /v1/stream
Authorization: Bearer <device_token>
X-Device-Id: <device_id>
```

| Limit | Value |
| --- | --- |
| Device identifier | 1 to 32 characters |
| Device token | 24 to 64 characters |

The server checks the token before accepting the connection.

| Outcome | Meaning | What the device does |
| --- | --- | --- |
| Upgrade succeeds, then `ready` arrives | Session open | Enter `IDLE` |
| HTTP 403 | Unknown device or wrong token | Do not retry with the same settings; they cannot succeed. Show the reason |
| Connection refused or timed out | Server not running or not reachable | Retry with backoff |

A device has at most one connection. If a device connects while the server
still holds an earlier connection for the same identifier, the newer one
wins and the older is closed with code `4000`. This is the normal case
after a power cut or a WiFi drop: the device is back before the server has
noticed the old connection is dead.

| Close code | Meaning | What the device does |
| --- | --- | --- |
| `4000` | Another connection with this identifier took over | Reconnect only after a long backoff. An immediate reconnect means two devices share an identifier, and each would evict the other without end |
| Any other | Server stopped or connection lost | Reconnect with backoff |

## Discovery

The server advertises itself by mDNS, and the device looks it up each time
it connects.

| Field | Value |
| --- | --- |
| Service type | `_hey-claude._tcp.local.` |
| Port | The server's port |
| TXT `path` | `/v1/stream` |
| TXT `protocol` | `1` |

The device's optional `server_url` setting overrides discovery.

## Frames

- **Text frames** are control messages: one JSON object each, with a `type`
  field. A receiver ignores fields it does not know, so that either side can
  add one later.
- **Binary frames** are audio: raw little-endian signed 16-bit mono PCM, with
  no header.

| Direction | Rate | Frame size |
| --- | --- | --- |
| Device to server | 16 kHz, fixed | 20 ms, 640 bytes. Frames over 1280 bytes, or with an odd number of bytes, are dropped |
| Server to device | Stated in `reply_start`; 24 kHz by default | At most 40 ms at the stated rate: 1920 bytes at 24 kHz. Only the last chunk of a reply may be shorter. Always a whole number of samples |

The capture rate is not announced anywhere. It is fixed by the microphone
and the wake-word model.

## Messages

| Message | Sender | Sent when | What the receiver does |
| --- | --- | --- | --- |
| `ready` | Server | Once, after the connection is accepted | Device enters `IDLE` |
| `utterance_start` | Device | The wake word fired while the device was in `IDLE` | Server starts a turn and takes the audio that follows |
| *binary* | Device | From `utterance_start` until `stop_capture` | Server transcribes it |
| `stop_capture` | Server | The request is over | Device stops sending audio and enters `THINKING` |
| `reply_start` | Server | Before the first reply audio | Device prepares playback at the stated rate |
| *binary* | Server | After `reply_start` | Device plays it and enters `SPEAKING` at the first chunk |
| `reply_end` | Server | The turn is over | Device finishes playing what it holds, then enters `IDLE` |
| `error` | Server | The turn was abandoned | Device stops playback, discards unplayed audio, shows `ERROR` briefly, then enters `IDLE` |

### Fields

```json
{"type": "ready", "protocol": 1}
{"type": "utterance_start"}
{"type": "stop_capture"}
{"type": "reply_start", "format": "pcm_s16le", "sample_rate": 24000, "channels": 1}
{"type": "reply_end"}
{"type": "error", "code": "llm_failed", "message": "The assistant failed to answer."}
```

### Error codes

| Code | Cause |
| --- | --- |
| `stt_failed` | Speech-to-text failed or could not be reached |
| `llm_failed` | Claude failed, could not be reached, or returned nothing |
| `tts_failed` | Text-to-speech failed or could not be reached |
| `internal` | A fault in the server |

`message` is a short English sentence. Whether the device shows it, or its
own wording chosen by `code`, is decided in stage 6.

## A turn

A turn is one request and its reply. The usual sequence:

```
device                              server
  │ utterance_start                   │
  │ ────────────────────────────────► │
  │ audio, 20 ms frames               │
  │ ────────────────────────────────► │   transcribing
  │                      stop_capture │
  │ ◄──────────────────────────────── │   speaker finished
  │                       reply_start │
  │ ◄──────────────────────────────── │
  │                 audio, 40 ms chunks│
  │ ◄──────────────────────────────── │   sentence by sentence
  │                         reply_end │
  │ ◄──────────────────────────────── │
```

Variations:

| Case | Sequence |
| --- | --- |
| Nothing was said | `stop_capture`, then `reply_end`. No `reply_start`, no audio |
| A service failed before any reply audio | `error`, with or without a `stop_capture` before it |
| A service failed part-way through the reply | `stop_capture`, `reply_start`, some audio, then `error`. No `reply_end` |

Every turn ends with exactly one of `reply_end` and `error`.

## Rules

1. **One turn at a time, and a turn cannot be interrupted.** The server
   accepts `utterance_start` only when no turn is running. One that arrives
   during a turn is ignored: it gets no answer and the running turn carries
   on. The device, for its part, acts on the wake word only in `IDLE`.
2. **`reply_end` means all audio has been sent, not that it has been
   played.** The server sends reply audio as fast as it is synthesised, so
   the device still holds audio when `reply_end` arrives. The server is
   ready for a new turn at once; the device is not in `IDLE`, and so sends
   no `utterance_start`, until playback is finished.
3. **Audio is taken only during capture.** Frames sent before
   `utterance_start` or after `stop_capture` are dropped without comment.
   Frames already on their way when `stop_capture` is sent are expected.
4. **`error` ends the turn, not the connection.** After `error` the
   connection is open and the server is ready for a new turn. The device
   returns to `IDLE` without reconnecting.
5. **A message the server cannot understand is ignored.** It is logged on
   the server and not answered.
6. **When the device is in doubt, it closes the connection.** The device has
   no message for "I gave up". On its own timeout, a playback fault, or a
   message it did not expect in its state, it closes the WebSocket and
   connects again. The server cancels the running turn when the connection
   closes, and a cancelled turn is not added to the conversation. This is
   the only way a turn ends early.

## Time limits

| Limit | Value | Held by |
| --- | --- | --- |
| Wait for the first recognised word | 5 s (server setting) | Server: ends the turn with `stop_capture`, `reply_end` |
| Longest capture | 13 s from `utterance_start` (server setting) | Server: sends `stop_capture` and answers what was heard |
| Longest capture, as a guard | 15 s from the wake word | Device: closes the connection (rule 6) |
| Silence from Claude | 20 s | Server: `error` with `llm_failed` |
| Silence from text-to-speech | 10 s | Server: `error` with `tts_failed` |
| Keepalive | Ping every 20 s; 20 s for the pong | Server: drops a connection that does not answer |

The server's capture limit is deliberately below the device's, so that a
request that never seems to end is answered instead of abandoned.

The device's own deadlines, for the wait between `stop_capture` and the
first reply audio and for a gap between chunks, are set in stage 6. They
must be longer than the server's limits above, so that the server's `error`
arrives first.

A device that stops reading from the connection also stops answering pings
and is dropped. Not reading is therefore not a way to slow the server down.

## Not in version 1

- **Pacing of reply audio.** The server does not limit how far ahead of
  playback it sends. See R17 in [KNOWN-ISSUES.md](../KNOWN-ISSUES.md).
- **The device's protocol version.** The server states its version in
  `ready` and in the mDNS record; the device does not state its own.
- **A turn identifier.** Messages do not say which turn they belong to.
  Rule 6 is what keeps a device from mistaking one turn's reply for
  another's.
