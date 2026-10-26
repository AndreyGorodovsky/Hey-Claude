# Device protocol

The reference for everything that passes between a device and the server.
Both sides are built: the server's in stage 5, the device's in stage 6.
Each keeps the protocol's numbers in one file, `server/protocol.py` and
`firmware/components/protocol/include/protocol.h`, and a test on the
server side reads both and fails if they disagree or if a limit that must
outlast another does not.

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
| Upgrade succeeds, then `ready` arrives | Session open | Enter `IDLE`, if `protocol` in `ready` is the device's own version. Otherwise close, and try again much later: one side needs updating |
| Upgrade succeeds, no `ready` within 10 s | Server not behaving as one | Close and retry with backoff |
| HTTP 403 | Unknown device or wrong token | Do not retry with the same settings; they cannot succeed. Show the reason |
| Connection refused or timed out | Server not running or not reachable | Retry with backoff |

A device has at most one connection. If a device connects while the server
still holds an earlier connection for the same identifier, the newer one
wins and the older is closed with code `4000`. This is the normal case
after a power cut or a WiFi drop: the device is back before the server has
noticed the old connection is dead.

| Close code | Meaning | What the device does |
| --- | --- | --- |
| `4000` | Another connection with this identifier took over | Reconnect only after 60 s. An immediate reconnect means two devices share an identifier, and each would evict the other without end |
| Any other | Server stopped or connection lost | Reconnect with backoff |

Backoff is the wait before the next attempt: 1 s after a connection that
had worked, then doubling with each failure up to 30 s. Every attempt
starts with a new lookup of the server.

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
| `utterance_start` | Device | The wake word fired while the device was in `IDLE`, and the device's chime has played | Server starts a turn and takes the audio that follows |
| *binary* | Device | From `utterance_start` until `stop_capture` | Server transcribes it |
| `stop_capture` | Server | The request is over | Device stops sending audio and enters `THINKING` |
| `reply_start` | Server | Before the first reply audio | Device prepares playback at the stated rate |
| *binary* | Server | After `reply_start` | Device plays it, and enters `SPEAKING` when the first sound comes out |
| `reply_end` | Server | The turn is over | Device finishes playing what it holds, then enters `IDLE` |
| `error` | Server | The turn was abandoned | Device stops playback, discards unplayed audio, shows `ERROR` briefly, then enters `IDLE` |
| `volume` | Server | The request asked for a change of volume; after `stop_capture` and before the `reply_start` of the reply that confirms it | Device plays at the new level from that reply on, and keeps it |
| `cancel` | Device | The person pressed the device's button during a turn: while capturing, thinking or speaking | Server ends the turn and sends `reply_end`, if the turn has not already ended (see Interrupting a turn) |

### Fields

```json
{"type": "ready", "protocol": 1}
{"type": "utterance_start", "volume": 8}
{"type": "stop_capture"}
{"type": "reply_start", "format": "pcm_s16le", "sample_rate": 24000, "channels": 1}
{"type": "reply_end"}
{"type": "error", "code": "llm_failed", "message": "The assistant failed to answer."}
{"type": "volume", "level": 9}
{"type": "cancel"}
```

### Volume

The device's playback volume is a level from **1** to **10**. The device
keeps it, through restarts, and states it as `volume` in every
`utterance_start`. What a level sounds like is the device's own business.

The server answers a request that is only about the volume ("louder",
"turn it down", "volume five") itself, without the model: it sends
`volume` with the new level, then a short spoken confirmation as an
ordinary reply. Nothing of such a turn is added to the conversation. At
the end of the scale, or for a level off it, nothing is sent and the
spoken reply says so.

`volume` is an addition within version 1, which the rule on unknown fields
and types allows: a device that states no `volume`, or one that is not a
whole number, is answered by the model whatever it says, because it could
not act on the answer. A device ignores a `volume` whose level is off the
scale.

### Interrupting a turn

The person can end a turn early by pressing the device's button; speech
does not interrupt. The device stops capturing and playing at once,
discards the audio it holds, shows `IDLE`, and sends `cancel`. It then
waits for the turn's end, `reply_end` or `error`, whichever comes, and
drops any audio that arrives meanwhile. Only once that end has arrived
does it act on the wake word again, since the server accepts a new
request only when no turn is running (rule 1).

The server answers `cancel` with `reply_end` when a turn is running and
its end has not yet been sent: it stops speech-to-text, Claude and the
synthesiser, and sends no more audio. Audio sent before the cancel was
read is ahead of `reply_end` on the connection, so everything before
`reply_end` belongs to the interrupted turn. When the turn has already
ended, or none is running, `cancel` is ignored: the end it waits for is
already on its way, or has arrived. A turn therefore always ends exactly
once, whether or not it was interrupted.

An interrupted turn stays in the conversation if it had reached Claude:
the request, and the reply as far as its audio had begun to be sent,
followed by `[interrupted]`. The server cannot know how much of that the
person heard, since the device holds up to two seconds of it. A turn
interrupted while capturing, or a volume request, adds nothing.

A device that has not received the turn's end within 3 s of sending
`cancel` closes the connection (rule 6), and so does one that could not
send `cancel` within as long again, as when the network stalls. Either way
it shows no failure: the person has already dismissed the turn.

`cancel` is an addition within version 1, which the rule on unknown types
allows. A server older than it ignores the message and the turn runs on;
the device, waiting 3 s for an end that comes later or not at all, closes
the connection, which cancels the turn.

### Error codes

| Code | Cause |
| --- | --- |
| `stt_failed` | Speech-to-text failed or could not be reached |
| `llm_failed` | Claude failed, could not be reached, or returned nothing |
| `tts_failed` | Text-to-speech failed or could not be reached |
| `internal` | A fault in the server |

`message` is a short English sentence, for logs. The device does not show
it: it shows its own wording, chosen by `code` and sized for its display.

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
| A change of volume | `stop_capture`, `volume`, then the confirmation as a reply: `reply_start`, audio, `reply_end`. Without `volume` when the level does not change |
| A service failed before any reply audio | `error`, with or without a `stop_capture` before it |
| A service failed part-way through the reply | `stop_capture`, `reply_start`, some audio, then `error`. No `reply_end` |
| Interrupted by the person | The turn's messages as far as it got, then from the device `cancel`, and from the server `reply_end`. Without `stop_capture` if the turn was still capturing |

Every turn ends with exactly one of `reply_end` and `error`.

## Rules

1. **One turn at a time.** The server accepts `utterance_start` only when
   no turn is running. One that arrives during a turn is ignored: it gets
   no answer and the running turn carries on. The device, for its part,
   acts on the wake word only in `IDLE`, and after a `cancel` only once the
   turn's end has arrived. The one way to end a turn early without closing
   the connection is `cancel` (Interrupting a turn).
2. **`reply_end` means all audio has been sent, not that it has been
   played.** The device still holds up to two seconds of audio when
   `reply_end` arrives. The server is ready for a new turn at once; the
   device is not in `IDLE`, and so sends no `utterance_start`, until
   playback is finished.
3. **Audio is taken only during capture.** Frames sent before
   `utterance_start` or after `stop_capture` are dropped without comment.
   Frames already on their way when `stop_capture` is sent are expected.
4. **`error` ends the turn, not the connection.** After `error` the
   connection is open and the server is ready for a new turn. The device
   returns to `IDLE` without reconnecting.
5. **A message the server cannot understand is ignored.** It is logged on
   the server and not answered.
6. **When the device is in doubt, it closes the connection.** The device has
   no message for "I gave up": `cancel` is the person's choice, not the
   device's. On its own timeout, a playback fault, or a message it did not
   expect in its state, it closes the WebSocket and connects again. The
   server cancels the running turn when the connection closes, and a turn
   cancelled that way is not added to the conversation.

## Pacing of reply audio

The server never sends more than **2 s** of reply audio ahead of playback,
however long the reply. It has no report of what the device has played, so
it works this out for itself: playback is taken to begin when the first
chunk is sent and to run without pause at the stated rate. Up to the first
two seconds of a reply therefore arrive as fast as they are made, and the
rest at the speed of playback.

The device, for its part:

- starts playing as soon as the first chunk arrives, or very shortly
  after (the firmware waits until 80 ms is in hand);
- has room for **4 s** of reply audio. The second two seconds are the
  margin for what the server cannot see: a stall on the network, after
  which the delayed chunks arrive together, or playback that started late
  or paused;
- closes the connection if a chunk arrives that it has no room for
  (rule 6). It does not drop audio and carry on.

## Time limits

| Limit | Value | Held by |
| --- | --- | --- |
| Wait for the first recognised word | 5 s (server setting) | Server: ends the turn with `stop_capture`, `reply_end` |
| Longest capture | 13 s from `utterance_start` (server setting) | Server: sends `stop_capture` and answers what was heard |
| Longest capture, as a guard | 15 s from `utterance_start` | Device: closes the connection (rule 6) |
| Reaching a service | 5 s | Server: `error` for that service |
| Silence from Claude | 24 s | Server: `error` with `llm_failed` |
| Silence from text-to-speech | 10 s | Server: `error` with `tts_failed` |
| First reply audio, from `stop_capture` | 30 s | Server: `error` with `llm_failed` or `tts_failed`, whichever it was waiting on |
| Wait in `THINKING` | 40 s from `stop_capture` | Device: closes the connection (rule 6) |
| Silence inside a reply | 40 s with nothing to play | Device: closes the connection (rule 6) |
| The turn's end after `cancel` | 3 s from sending `cancel` | Device: closes the connection (rule 6) |
| Keepalive, server | Ping every 20 s; 20 s for the pong | Server: drops a connection that does not answer |
| Keepalive, device | Ping every 10 s; 20 s for the pong | Device: treats the connection as lost |

Each of the device's limits is longer than the server's for the same wait,
so that the server's `error` arrives first when a service fails:

- the server's capture limit, 13 s, is below the device's 15 s, so that a
  request that never seems to end is answered instead of abandoned;
- the server sends audio or an `error` within 30 s of `stop_capture`,
  whatever its services' own limits and retries add up to, and the device
  waits 40 s;
- inside a reply, the longest silence the server's services allow is 39 s
  (24 s from Claude, then 5 s to reach the synthesiser and 10 s of silence
  from it), and the device allows 40 s. The first of these was 20 s until
  stage 7 and cannot be raised further.

"Silence from Claude" is silence in the text of the reply, whatever else
the server may be receiving meanwhile. With Claude reached through
Claude Code, which gives the assistant access to the web ([ARCHITECTURE.md](../ARCHITECTURE.md), Claude
through Claude Code), a reply can pause for a search after its first
words; a pause of 24 s ends the turn with `llm_failed` like any other.

A device that stops reading from the connection also stops answering pings
and is dropped. Not reading is therefore not a way to slow the server down.

## Not in version 1

- **A report of playback from the device.** Pacing rests on the server's
  assumption about when playback began, not on anything the device says.
- **The device's protocol version.** The server states its version in
  `ready` and in the mDNS record; the device does not state its own.
- **A turn identifier.** Messages do not say which turn they belong to.
  Rule 6 is what keeps a device from mistaking one turn's reply for
  another's.
