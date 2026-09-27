# Architecture

Design reference for the Hey Claude voice assistant. This describes the
intended system; which parts are built is tracked by stage in
[STATUS.md](STATUS.md). Decisions recorded here were
made during the feasibility review and are binding unless explicitly revisited.

## Design principles

1. **The device is stateless.** It holds no conversation history and no
   credentials beyond its own identity. Any device can be reflashed or replaced
   without data loss.
2. **The server owns all judgement.** End-of-speech detection, conversation
   grouping and error recovery are server-side. Firmware executes a state
   machine and nothing more.
3. **Stream everything.** Audio moves in both directions while it is still
   being produced. A request/response design cannot reach acceptable latency
   and cannot be retrofitted cheaply.
4. **No secrets in firmware.** API keys exist only on the server.

## Device

### Firmware structure

| Component | Responsibility |
| --- | --- |
| `main` | Startup order, boot banner, serial console and its commands |
| `app_config` | Settings stored in NVS: loading the running configuration, validating and storing edits. No user interface of its own |
| `net` | WiFi station: joining, reconnecting with backoff, readable failure reasons |
| `board` | The verified pin map, the single source of pin numbers |

Settings are loaded once at boot into a read-only structure. Edits change only
what is stored and apply after a reboot, so no running code has to cope with a
setting changing underneath it. `app_config` knows nothing about the console,
so later provisioning methods reuse the same validation.

`net` keeps no connection state for other components to query. Consumers
subscribe to the standard `IP_EVENT_STA_GOT_IP` and
`WIFI_EVENT_STA_DISCONNECTED` events on the default event loop.

Every boot logs the firmware version, flash size, free internal RAM and PSRAM,
and the reset reason. Brownout and power-glitch resets are logged as errors,
which makes a supply fault visible without instruments (R2, R9). A crash is
written to a core-dump partition and can be read after the reboot with
`idf.py coredump-info`.

### Flash layout

| Partition | Offset | Size | Purpose |
| --- | --- | --- | --- |
| `nvs` | 0x9000 | 24 KB | Settings |
| `otadata`, `phy_init` | 0xF000 | 12 KB | OTA slot selection, RF calibration |
| `ota_0`, `ota_1` | 0x20000 | 4 MB each | Two application slots |
| `coredump` | 0x820000 | 64 KB | Last crash |
| unallocated | 0x830000 | ~7.8 MB | Reserved |

The layout is OTA-ready from the start. Whether over-the-air updates are
adopted is decided at stage 8, but adopting them then needs no change to the
flash layout. Flash runs in QIO mode at 80 MHz; PSRAM is octal at 80 MHz.

### State machine

| State | Trigger to enter | Display animation |
| --- | --- | --- |
| `BOOT` | Power on | Splash |
| `CONNECTING` | Boot complete | Connecting indicator |
| `IDLE` | Server session ready | Waiting for wake word |
| `CAPTURING` | Wake word detected | Listening |
| `THINKING` | Server sends `stop_capture` | Waiting for response |
| `SPEAKING` | First reply audio chunk arrives | Playing |
| `ERROR` | Transport or server error | Error indicator |

`ERROR` returns to `CONNECTING` after a backoff. Every other transition is
driven either by local audio events or by server messages.

### Task and core allocation

| Core | Tasks | Rationale |
| --- | --- | --- |
| 0 | WiFi and lwIP, WebSocket client, display and LVGL | Network stack jitter is tolerable for rendering |
| 1 | Audio capture, wake-word inference, audio playback | Hard real-time work isolated from WiFi interrupt load |

Isolating the audio chain from the WiFi stack is deliberate: I2S DMA underruns
produce audible artefacts, and WiFi driver interrupt latency is the most likely
cause of them.

### Memory placement

| Buffer | Location | Reason |
| --- | --- | --- |
| Audio ring buffers | PSRAM | Large, latency-tolerant |
| LVGL frame buffer (~121 KB) | PSRAM | Too large for internal SRAM |
| Wake-word tensor arena | Internal SRAM | Inference runs continuously; PSRAM latency would cost frames |
| Network TX/RX queues | Internal SRAM | Touched from interrupt context |

### Audio path

- **Capture:** INMP441 on I2S0, 16 kHz, 16-bit mono. The microphone emits
  24-bit frames; the top 16 bits are taken. 16 kHz mono is what both the
  wake-word model and the speech-to-text service expect, so no resampling
  occurs anywhere in the chain.
- **Playback:** MAX98357A on I2S1, 24 kHz, 16-bit mono. Playback runs at a
  higher rate than capture because synthesised speech benefits from it and the
  amplifier sits on an independent I2S controller.
- The amplifier `SD` pin is held low except during playback, to suppress the
  switching click produced when the output stage engages. Driven high from a
  3.3 V GPIO, it selects the left I2S slot, so playback samples are written to
  the left slot, or to both.
- The microphone, with `L/R` tied to ground, transmits in the left slot only.
  The right slot reads zero.

The ESP32-S3 has two independent I2S controllers, so capture and playback never
contend.

### Display

LVGL 9 drives the NV3007 panel over SPI. A full-screen refresh at RGB565 moves
about 121 KB, roughly 12-25 ms depending on SPI clock, which is too slow for
full-screen animation at high frame rates. State animations are therefore
designed as partial-region redraws within the panel's narrow 142x428 format.
The panel is write-only over SPI with no MISO line, which LVGL accommodates.

The controller's frame memory is 168x428, wider than the 142 visible columns,
so drawing needs a column offset. The offset is determined in stage 3. The
initialisation sequence is LVGL's `lv_nv3007` driver sequence, which works on
this module as verified during stage 0.

## Protocol

A single bidirectional WebSocket per device, opened at boot and held open.
Control messages are JSON text frames; audio travels as binary frames of raw
little-endian signed 16-bit PCM.

### Connection

```
GET /v1/stream
Authorization: Bearer <device_token>
X-Device-Id: <device_id>
```

The server replies with a `ready` message once the session store is available.

### Server discovery

The device finds the server by mDNS service discovery on the local network,
so the server machine's address can change without reconfiguring the device.
The server advertises a service; the device looks it up each time it
connects. The optional `server_url` setting overrides discovery with a fixed
address or hostname. Discovery is built in stages 5 and 6.

### Message sequence

| Direction | Message | Meaning |
| --- | --- | --- |
| Device to server | `utterance_start` | Wake word fired; PCM frames follow |
| Device to server | *binary* | 20 ms frames, 16 kHz mono |
| Server to device | `stop_capture` | Speaker has finished; stop the microphone |
| Server to device | `reply_start` | Audio follows, with format and sample rate |
| Server to device | *binary* | Reply audio chunks |
| Server to device | `reply_end` | Playback complete; return to `IDLE` |
| Server to device | `error` | Abort to `ERROR`, carrying a code and message |

WebSocket ping and pong frames provide keepalive. A device-side hard cap of 15
seconds on capture guards against a server that never sends `stop_capture`.

## Server

Python 3.11+, asyncio throughout. FastAPI provides the WebSocket endpoint and
health checks.

```
server/
  transport/   WebSocket endpoint, device auth, framing
  stt/         Speech-to-text adapter interface and implementations
  llm/         Claude client, prompt construction, streaming
  tts/         Text-to-speech adapter interface and implementations
  session/     Day-scoped conversation store
  config.py    Environment-driven configuration
```

### Speech services

Deepgram is the default for both directions, behind adapter interfaces so
either side can be swapped by configuration.

Speech-to-text was chosen for its streaming utterance-end events, which supply
the end-of-speech signal directly. A separate voice-activity detector would
otherwise be required; using one service for both transcript and endpointing
keeps the two in agreement and avoids adding a third vendor to the latency
path.

Documented alternatives: ElevenLabs Flash for higher voice quality;
faster-whisper with Piper for a fully local pipeline requiring no accounts.

### Conversation model

Conversations are keyed by device identifier and chat date, and persisted in
SQLite so that a server restart does not lose context. The chat date advances
at **04:00 local time** rather than at midnight, so that a conversation held
late at night is not split partway through.

History is resent on every turn, which makes **prompt caching** the mechanism
that keeps cost and time-to-first-token flat across a day rather than growing
with each exchange. The system prompt is held stable and placed first, and the
cache breakpoint sits at the end of prior history so that only the new turn
falls outside the cached prefix.

Context length is not a constraint. A full day of spoken exchanges stays far
below the model's context window.

### Claude configuration

| Setting | Value | Reason |
| --- | --- | --- |
| Model | `claude-opus-5` | |
| Thinking | Adaptive | Default on this model; disabling it introduces documented failure modes |
| Effort | Low | Conversational replies are not reasoning-intensive, and effort is the primary latency lever |
| Streaming | Enabled | Required to begin synthesis before the reply completes |
| Fallbacks | Server-side, default routing | Handles refusal stop reasons without a maintained model list |

The system prompt constrains replies to spoken form: no markdown, no lists, no
headings, and short answers by default. Text shaped for a screen reads badly
when synthesised.

### Reply pipeline

The response stream is split into sentences as it arrives. Each completed
sentence is synthesised and forwarded immediately, so playback begins while the
reply is still being generated.

## Latency budget

Target, from wake word to first audible sound:

| Stage | Budget |
| --- | --- |
| Capture and upload, overlapped with speech | ~0 ms |
| End-of-speech detection | 300-500 ms |
| Claude time-to-first-token | 600-1200 ms |
| First sentence synthesis | 300-500 ms |
| Network and playback start | 100 ms |
| **Total** | **1.3-2.3 s** |

A non-streaming implementation of the same pipeline lands at 5-9 seconds, which
is why streaming is a structural requirement rather than an optimisation.

## Deployment

The server runs on the local network during development. Transport is plain
WebSocket over the LAN, and device tokens are shared secrets. Moving the server
off the LAN requires TLS and a stronger device credential, and is deliberately
deferred.

## Decisions and rationale

| Decision | Alternative rejected | Reason |
| --- | --- | --- |
| microWakeWord | Espressif WakeNet custom training | Custom phrases require a paid corpus of 500+ speakers |
| ESP-IDF v5.5.x | v5.3, v6.1-beta1 | v5.3 predates the NV3007 and LVGL reference work; v6.1 is a beta |
| Server-side endpointing | On-device VAD | Better models, no device CPU cost, keeps firmware simple |
| Server-side history | Device-side history | Survives reboots and WiFi loss, and enables prompt caching |
| Two I2S controllers | Shared bus | No contention, and different sample rates per direction |
| Settings entered through the serial console | Credentials compiled in, or flashed from a local file | Nothing sensitive is ever written to disk on the development machine |
| mDNS server discovery | Fixed server address | The server machine's LAN address changes; a fixed address would need re-entering each time |
| OTA-ready partition layout | Single application slot | Costs nothing in 16 MB, and keeps OTA open without a later layout change |
| Streaming transport | Request and response | Fourfold latency difference; cannot be retrofitted cheaply |
