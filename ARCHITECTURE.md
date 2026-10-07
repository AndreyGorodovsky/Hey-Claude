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
| `main` | Startup order, boot banner, serial console and its commands: settings, audio, display and wake-word tests, and read-outs of the state, the server connection, memory and chip temperature |
| `app_config` | Settings stored in NVS: loading the running configuration, validating and storing edits. No user interface of its own |
| `net` | WiFi station: joining, reconnecting with backoff, readable failure reasons |
| `audio` | Microphone capture and amplifier playback over I2S: plain 16-bit samples in and out, no buffering or tasks of its own |
| `audio_ring` | The microphone's only reader: a capture task filling a ring buffer of the last 2 s, from which every listener reads at its own position |
| `wakeword` | On-device wake-word detection with a microWakeWord model; announces detections and decides nothing else |
| `app_state` | The device state and its detail text: stored, and announced on change. Decides no transitions |
| `state_machine` | The state machine: the one place that decides what the device does next, and the only writer of the state. Also runs the request upload |
| `server_link` | The WebSocket connection to the server: finds it by mDNS, connects, authenticates, reconnects with backoff, and turns the server's messages into reports |
| `player` | Reply playback: a buffer for 4 s of reply audio and the task that plays it |
| `protocol` | The protocol's numbers as the firmware uses them, in one header. A test on the server side holds it to the server's own |
| `display` | The NV3007 panel, its backlight, and what is drawn for each state, from a single task that is the only user of LVGL |
| `board` | The verified pin map and panel geometry, the single source of both |

Settings are loaded once at boot into a read-only structure. Edits change only
what is stored and apply after a reboot, so no running code has to cope with a
setting changing underneath it. `app_config` knows nothing about the console,
so later provisioning methods reuse the same validation.

`net` keeps no connection state for other components to query. Consumers
subscribe to the standard `IP_EVENT_STA_GOT_IP` and
`WIFI_EVENT_STA_DISCONNECTED` events on the default event loop.

`app_state` announces changes the same way, as `APP_STATE_CHANGED` on the
default event loop, and the display follows them without being called.
`wakeword` announces each detection as `WAKEWORD_DETECTED`, carrying the
position in the audio stream where the phrase ended.
`app_state` has exactly one writer: the state machine, `state_machine`. The boot
sequence sets the first states and then hands over. Components that detect
something (the wake word, a server message, a lost connection) report it
and never set states themselves; otherwise two tasks setting states at once
would leave the screen on whichever came last. The console can read the
state but not set it. To look at a state's screen without bringing the
state about, the console's `display preview` draws any state for a few
seconds; it changes what is drawn and leaves the state alone.

`server_link` and `player` report to the state machine by calling a function
given to them at start, and `server_link` hands reply audio on the same way. They
do not use the event loop for this: each report has one receiver, and a
call cannot be lost on the way or arrive out of order. Neither component
knows of `state_machine`; the boot sequence joins the three. Wake-word detection
keeps its events, which the `wake` console command also relies on.

Reply audio passes through `state_machine` on its way to `player`, and is let
through only while a turn is under way. Audio arriving at any other time,
such as the tail of a turn the device has already given up, never reaches
the speaker.

`state_machine` handles everything in one task, from one queue, one report at a
time, so no two decisions overlap and its variables need no lock. It never
waits on the network: everything the device sends during a turn is sent by
the upload task. Time limits use the same queue: the task waits for the
next report only until the current deadline, and a wait that runs out is
the deadline passing.

The eight device states are chosen for what a person needs to see. The
state machine needs finer steps, so it keeps its own, the phase, and shows
each phase as one of the states: waiting for a reply and holding its first
audio both show `THINKING`; playing and playing out both show `SPEAKING`;
a failure shown for a few seconds and a fault that stays both show
`ERROR`. The wording for every reason is in one table in `state_machine`, sized to
the panel's detail line.

The state machine also learns when the device can no longer hear.
`wakeword` announces `WAKEWORD_LISTENING` when the microphone's audio stops
or resumes, or the model fails, and the boot sequence reports whether
listening and playback started at all. A device that cannot hear or speak
shows `ERROR` with the reason and stays there, so the screen never shows
`IDLE` on a device that cannot hear.

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
| `SETUP` | WiFi settings missing or rejected at boot | Setup needed, with the reason |
| `CONNECTING` | Boot complete | Connecting indicator |
| `IDLE` | Server session ready | Waiting for wake word |
| `CAPTURING` | Wake word detected while `IDLE` | Listening |
| `THINKING` | Server sends `stop_capture` | Waiting for response |
| `SPEAKING` | The first sound of the reply reaches the speaker | Playing |
| `ERROR` | A failed turn, a lost connection, or a device that cannot hear or speak | Error indicator |

What follows `ERROR` depends on its cause. After an `error` message from
the server, which ends one turn and leaves the connection open, the device
shows `ERROR` briefly and returns to `IDLE`. After a lost connection it
returns to `CONNECTING` after a backoff. `SETUP` does not retry:
retrying cannot fix missing or wrong settings, so it stays until settings
are entered and the device is rebooted. Every other transition is driven
either by local audio events or by server messages. `SETUP` and `ERROR`
carry a short detail text naming the cause, and `CONNECTING` carries one
saying why the last attempt failed; no other state has one. The
wording is the device's own, chosen by cause and sized for the panel; the
sentence in the server's `error` message goes to the log.

`SETUP` is also where a device goes whose identifier and token the server
refuses: retrying cannot fix that either.

The device gives up on a turn by itself in four cases: 15 s of capture
with no `stop_capture`, 40 s of `THINKING` with no reply, 40 s of a reply
under way with no audio arriving, and a message that does not fit the
state it is in. In each it closes the connection, which is how the server
learns of it (docs/PROTOCOL.md, rule 6), shows `ERROR` for 4 s, and
reconnects. Each of the three times is longer than the server's own limit
for the same wait, so that the server's `error` arrives first; the figures
are in [docs/PROTOCOL.md](docs/PROTOCOL.md). After a reply, wake-word
detections are ignored for 0.7 s, so that the reply's last echoes cannot
start a request.

`CAPTURING` opens with a chime: two short rising notes that say
the wake word was heard. The screen changes with the sound, and the
request starts when the sound is over, so the device is not yet listening
while it plays. That order is deliberate. The person has a signal to wait
for before speaking, and the microphone's copy of the sound never reaches
speech recognition. The cost is that words spoken during the sound are
not sent. Measured on the breadboard on 2026-10-07, over five spoken
turns: the request began 220 ms after the end of the wake word each time.
If the sound cannot be played, the request starts all the same.

Losing a working connection is shown as `ERROR` with the reason for a few
seconds. While there is no server, the screen rests on `CONNECTING`, with
the reason the last attempt failed under it; a failed attempt changes that
line and nothing else.

### Task and core allocation

| Core | Tasks | Rationale |
| --- | --- | --- |
| 0 | WiFi and lwIP, WebSocket client, connection upkeep, state machine, request upload, display and LVGL | Network stack jitter is tolerable for rendering |
| 1 | Audio capture, wake-word inference, reply playback | Hard real-time work isolated from WiFi interrupt load |

Isolating the audio chain from the WiFi stack is deliberate: I2S DMA underruns
produce audible artefacts, and WiFi driver interrupt latency is the most likely
cause of them.

Task priorities decide which ready task a core runs first; higher runs first.
This table is the single place that records them, so that code comments
refer here instead of quoting each other's numbers.

| Task | Priority | Core | Notes |
| --- | --- | --- | --- |
| WiFi driver | 23 | 0 | ESP-IDF's; must keep up with the radio |
| `esp_timer` | 22 | 0 | ESP-IDF's; runs software timer callbacks, which must be short |
| Default event loop | 20 | 0 | ESP-IDF's; delivers WiFi, state and wake-word events |
| Capture | 12 | 1 | Reads the microphone into the ring; must never fall 75 ms behind |
| Audio test tasks | 10 | 1 | Console `audio` commands, short-lived |
| Player | 10 | 1 | Writes reply audio to the amplifier, which paces it; above the wake word so that a model run cannot delay the speaker |
| Wake word | 8 | 1 | Features and model every 30 ms; sleeps between |
| WebSocket client | 7 | 0 | The component's own task, one per connection; receives reply audio and hands it to the player |
| Upload | 6 | 0 | Reads the ring and sends request frames while capturing; sleeps otherwise |
| State machine | 5 | 0 | Acts on one event at a time, each in well under a millisecond |
| Link | 5 | 0 | Looks the server up, connects, and waits for the connection to end |
| Display | 4 | 0 | LVGL drawing and animation; sleeps between frames |
| Console | 2 | either | ESP-IDF's REPL; typing must not stall animation or audio |
| `main` | 1 | 0 | Runs the start-up sequence, then ends |

The audio set-up in `audio_init()` runs briefly on core 1 at the caller's
priority, so that the I2S interrupts are placed on core 1.

### Memory placement

| Buffer | Location | Reason |
| --- | --- | --- |
| Audio ring buffer (64 KB, 2 s) | PSRAM | Large, latency-tolerant |
| LVGL frame buffer (~121 KB) | PSRAM | Too large for internal SRAM. DMA sends it to the panel directly from PSRAM, at no measurable cost (see Display) |
| Wake-word tensor arena (~28 KB allocated, 25.5 KB used) | Internal SRAM | Inference runs continuously; PSRAM latency would cost frames |
| Wake-word model copy (~60 KB) | PSRAM | Read-only weights, read through the cache; copied from flash at boot for alignment |
| Reply buffer (384 KB) | PSRAM | 4 s of reply audio at the highest rate the amplifier accepts; a reply at 24 kHz uses half |
| Chime samples (6.7 KB) | PSRAM | Computed once at boot and copied into the reply buffer when played; never handed to DMA |
| WebSocket client buffers (2 x 4 KB) | Internal SRAM | Allocated by the component for each connection |
| Network TX/RX queues | Internal SRAM | Touched from interrupt context |

Measured on 2026-10-06 with the `mem` console command, on the breadboard,
connected to the server, after two replies and a lost and restored
connection: 100 KB of internal RAM free, 56 KB at the lowest since boot,
and 7.7 MB of PSRAM free. Every task had at least 1.5 KB of its stack
never used; the tightest were the link task (1580 bytes left of 4096), the
state machine (1924 of 4096) and the player (1968 of 4096). Not measured:
the upload task at the moment a send fails, when the WebSocket client's
error handling runs on its stack, which is why that stack is larger.

### Audio path

- **Capture:** INMP441 on I2S0, 16 kHz, 16-bit mono. The microphone emits
  24-bit frames; the top 16 bits are taken. 16 kHz mono is what both the
  wake-word model and the speech-to-text service expect, so no resampling
  occurs anywhere in the chain.
- The microphone runs continuously from boot. It powers down whenever its
  clock stops, and its output then takes about 2 s to settle after the clock
  restarts, so stopping it between recordings would spoil the start of each
  one. Callers pull samples with a blocking read; audio not read within about
  75 ms is dropped by the driver.
- The controller reads both I2S slots and keeps the left one. Its "mono" mode,
  documented as reading one slot, delivered both slots at 32 bits on this
  chip, interleaving every sample with a zero.
- **Playback:** MAX98357A on I2S1, 16-bit mono, at the rate given when each
  playback starts: 24 kHz for replies, which is announced by the server in
  `reply_start`. Playback runs at a higher rate than capture because
  synthesised speech benefits from it and the amplifier sits on an
  independent I2S controller.
- The amplifier `SD` pin is held low except during playback, to suppress the
  switching click produced when the output stage engages. Driven high from a
  3.3 V GPIO, it selects the left I2S slot, so playback samples are written to
  the left slot, or to both.
- The microphone, with `L/R` tied to ground, transmits in the left slot only.
  The right slot reads zero.

The ESP32-S3 has two independent I2S controllers, so capture and playback never
contend. The I2S interrupt handlers run on core 1 with the rest of the audio
chain: ESP-IDF places an interrupt on the core that sets it up, so the audio
set-up runs in a short-lived task there.

The `audio` component allows one reader of the microphone at a time. That
reader is the capture task in `audio_ring`, on core 1, which copies every
15 ms chunk into a 2 s ring buffer in PSRAM. Every listener reads from the
ring at its own position: wake-word detection, the `audio loop` test, and
from stage 6 the upload. A listener more than 2 s behind skips forward and
is told how many samples it missed. A listener can also start at a
position noted earlier, which is how the upload begins exactly where the
chime ended. A mutex guards the ring; copies are short, so the capture task waits
about a millisecond at most, against the 75 ms the microphone driver
allows.

**Request upload.** When a request starts, the upload task begins reading
the ring at the position where the chime ended, which is
slightly in the past by the time the task has woken. Those first frames go
out at once; after that it sends one 20 ms frame as each is captured,
until the server's `stop_capture`.

**Chime.** Two notes of 70 ms, 660 Hz and then 880 Hz, each
faded in and out over 8 ms so that it does not click, at 30 % of full
scale. `player` computes them at start, so the repository holds no audio
file, and plays them through the same path as a reply: the amplifier has
one user at a time, and that user is the player. What the chime sounds
like is in one file of its own, `chime.c`. A reply that begins meanwhile
replaces the sound. The player reports when the sound has left the
speaker, and the state machine starts the request on that report, or
after 1 s if none comes.

The 220 ms between the wake word and the request is 140 ms of notes and
about 80 ms around them. Of the 80 ms, about 20 ms is the amplifier
waking before the first note, and most of the rest is the silence written
after the last one, to push it out of the driver's buffers before the
amplifier is switched off; these two parts are from the code's own
figures, not measured apart. The silence after the last note is also what
keeps the note's tail out of the request, so shortening it to save time
changes what speech recognition hears.

**Reply playback.** `player` holds reply audio between the network, which
delivers it in bursts, and the amplifier, which takes it at a fixed speed.
Its buffer has room for 4 s. The server sends at most 2 s ahead of
playback (see Reply pipeline); the rest is margin for a stall on the
network. A chunk that does not fit ends the reply and the connection: the
device does not play a reply with a hole in it. Playback starts once
80 ms is waiting, so that the first chunks arriving unevenly cause no gap
in the first word, and can be stopped within about 100 ms for error
paths. Reply audio is played at the level the server sends it, with no
reduction: 40 % and then 60 % were tried and found too quiet. Volume
control arrives in stage 8.

**WiFi power saving is off.** By default the radio sleeps between the
router's beacons and can hold data back for up to 100 ms each way, which
is heard as gaps and can stall a send long enough to lose the connection.
The device is mains-powered, so the saving is not needed.

### Wake word

Detection runs entirely on the device, on core 1, using a microWakeWord
model with TensorFlow Lite for Microcontrollers and ESPHome's port of its
audio feature frontend. Every 10 ms the frontend turns the latest 30 ms of
audio into 40 frequency-band values, evening out steady noise and loudness
on the way; every third slice the model runs on the new slices and gives
the probability that the phrase has just ended. The last five
probabilities are averaged, and a detection fires when the average exceeds
the cutoff. After a detection, and at start-up, the detector waits a second
before it can fire again.

The model is two files in `firmware/components/wakeword/models/`: the
network and a manifest of the settings it was trained with. The phrase is
"Hey Claude", trained for this project in Google Colab
([docs/WAKEWORD-TRAINING.md](docs/WAKEWORD-TRAINING.md)); each training
run's model keeps its run in its name, which the boot log and `wake`
command print. Only the built model is kept there; the stock "Hey Jarvis"
model used to bring the pipeline up, and earlier training runs, remain in
git history. The build reads
the cutoff, averaging window, step and memory size from the manifest, so it
is their only source. The component versions are pinned exactly, because
the scores, and so the cutoff, depend on their arithmetic.

Measured on 2026-10-01 with the stock `hey_jarvis` model, on the
breadboard, USB-powered, with WiFi connected: one run of the model takes
2.2 ms on average and 5.7 ms at most, every 30 ms, about 8 % of core 1; the
model uses 22,492 bytes of working memory. In a 10-minute run with the
display cycling through every state, no audio was lost. The second "Hey
Claude" model, `hey_claude_run2`, takes 2.1 ms on average and 4.9 ms at
most, and uses 25,548 bytes of working memory and a 60,896-byte model copy
(measured 2026-10-02 on the breadboard, USB-powered, over a one-minute idle
run). The built model, `hey_claude_run3`, has the same shape, and its boot
log shows the same working memory and model copy (2026-10-04); its timing
was not measured again.

### Display

LVGL 9 (9.6.0 when stage 3 was built, fetched from the ESP Component
Registry) drives the NV3007 panel over SPI2 at 80 MHz, the highest clock SPI2
supports; GPIO 10-12 are its dedicated pins, which that speed requires. The
panel is write-only, with no MISO line, and has no tearing-effect (TE)
output, so drawing cannot be synchronised with the panel's own refresh
(KNOWN-ISSUES R13). The initialisation sequence is LVGL's `lv_nv3007`
driver sequence, which works on this module as verified during stage 0.

The panel is used in landscape, 428 x 142, by setting the controller's
address mode so that it swaps rows and columns itself; rotation costs no
processor time. The controller's memory is 168 columns wide for the 142
visible, with 12 unused on one side and 14 on the other. In the orientation
used (LVGL's `ROTATION_270`), drawing is offset by 14 rows, measured with a
test pattern.

LVGL draws into one full-screen buffer (121,552 bytes) in PSRAM, in the
panel's byte order (`RGB565_SWAPPED`), so no conversion pass precedes each
transfer. It redraws only the rectangles that changed, and DMA sends each one
straight from PSRAM. A full frame takes 12.5 ms at 80 MHz, against a
theoretical 12.2 ms, and 98.7 ms at 10 MHz (measured 2026-09-29 with
`display test`, on the breadboard). The state animations change only a
142 x 142 region at the left, which takes about 4 ms to send.

The backlight is dimmed by PWM from the LEDC peripheral at 25 kHz, above
hearing. It stays off until the first frame has been drawn, so the random
contents of the panel at power-on are never visible, then fades in. It dims
to 25 % in `IDLE`. No noise was heard from the audio path at 25 % duty
(2026-09-29).

The state animations are placeholders, refined in stage 8. Each state's
title, default detail text, animation and brightness are kept together in
one table in `display_ui.c`.

## Protocol

This section is an overview. The reference, with every rule, limit and
close code, is [docs/PROTOCOL.md](docs/PROTOCOL.md).

A single bidirectional WebSocket per device, opened at boot and held open.
Control messages are JSON text frames; audio travels as binary frames of raw
little-endian signed 16-bit PCM.

### Connection

```
GET /v1/stream
Authorization: Bearer <device_token>
X-Device-Id: <device_id>
```

The token is a shared secret that proves the device is the one its
identifier names. Without it, anything on the network could spend the
server's API credit and continue a device's conversation. The server checks
it before accepting the connection, then sends `ready`. A device has at
most one connection; a newer one replaces an older one.

### Server discovery

The device finds the server by mDNS service discovery on the local network,
so the server machine's address can change without reconfiguring the device.
The server advertises a service; the device looks it up each time it
connects. The optional `server_url` setting overrides discovery with a fixed
address or hostname. The server's side was built in stage 5; the device's
lookup is built in stage 6. The server advertises one address, the one on
the machine's default route, since a machine often has others (virtual
adapters, VPNs) that a device cannot reach.

### Message sequence

| Direction | Message | Meaning |
| --- | --- | --- |
| Device to server | `utterance_start` | Wake word fired; PCM frames follow |
| Device to server | *binary* | 20 ms frames, 16 kHz mono |
| Server to device | `stop_capture` | Speaker has finished; stop sending audio |
| Server to device | `reply_start` | Audio follows, with format and sample rate |
| Server to device | *binary* | Reply audio, in chunks of at most 40 ms |
| Server to device | `reply_end` | All reply audio has been sent; return to `IDLE` once it has played |
| Server to device | `error` | The turn was abandoned, with a code and message. The connection stays open |

A turn, once started, runs to its end. The server accepts `utterance_start`
only when no turn is running and ignores one that arrives during a turn;
the device acts on the wake word only in `IDLE`. A request cannot be
interrupted by a second one. When nothing is said after the wake word, the
server sends `stop_capture` and then `reply_end`, with no reply.

The server pings every 20 s for keepalive. It ends a capture after 13 s. A
device-side hard cap of 15 s guards against a server that never sends
`stop_capture`; when it is reached, or whenever the device is in doubt, the
device closes the connection, which cancels the turn on the server.

## Server

Python 3.11+, asyncio throughout. FastAPI provides the WebSocket endpoint and
health checks.

```
server/
  protocol.py     Message shapes and constants of the device protocol
  transport/      WebSocket endpoint, device auth, one connection per device,
                  framing of reply audio
  pipeline/       One turn from request to reply; sentence splitting
  stt/            Speech-to-text adapter interface and implementations
  llm/            Claude client, system prompt, streaming
  tts/            Text-to-speech adapter interface and implementations
  conversation/   Day-scoped conversation store
  discovery.py    mDNS advertisement
  config.py       Environment-driven configuration
  tools/          Desktop client that stands in for the device
  tests/          Tests, run with stand-ins for the three services
```

A turn is one task in `pipeline/`. It decides what happens and in what
order, and reports each step (capture done, reply audio, reply done,
failed) to the connection, which alone knows how a step is put on the wire.
A turn is added to the conversation only if it completed, so the stored
history always alternates between the person and the assistant. A turn
whose device disconnects is cancelled.

Device tokens are configured on the server as one setting, `DEVICE_TOKENS`,
mapping each device identifier to its token, and compared in constant time.

### Speech services

Deepgram is the default for both directions, behind adapter interfaces, so
that either side can be replaced by writing one class. There is no setting
that selects a provider yet, since there is only one. Deepgram is reached
directly, over its WebSocket and HTTP interfaces, without its SDK: the
server uses three event types and one request, and the SDK's interface has
changed across major versions.

The end of a request is decided from Deepgram's events: silence of a set
length after speech (400 ms by default), or its second signal based on word
timings, which still works over background noise. Silence before any speech
does not end a request; if no word is recognised within 5 s, the turn ends
with no reply.

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
late at night is not split partway through. Only the current day is kept:
the first exchange stored on a new chat date deletes every earlier day's
conversations.

History is resent on every turn, which makes **prompt caching** the mechanism
that keeps cost and time-to-first-token flat across a day rather than growing
with each exchange. The system prompt is held stable and placed first, with
nothing that varies in it. Cache breakpoints sit at the end of the system
prompt and at the end of the new request, so each turn reads everything
before it from the cache and writes only what the last turn added. Caching
starts once the prompt reaches the model's minimum cacheable length, which a
day's conversation passes after a few exchanges.

Measured on 2026-10-06 with `claude-opus-5`, in a five-turn conversation of
synthetic test requests: the first two requests, of 468 and 486 input
tokens, were not cached; the third wrote 538 tokens to the cache, and the
next reply read those 538 from it.

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
| Output ceiling | 8000 tokens | Stops a runaway reply from being synthesised for minutes |
| Timeout | 20 s of silence on the stream | The SDK's default tolerates ten minutes, which would hold a device in `THINKING` as long |

The system prompt constrains replies to spoken form: no markdown, no lists, no
headings, and short answers by default. Text shaped for a screen reads badly
when synthesised. It also tells the model that its input is a transcript
that may contain misheard words, and that it has no clock and no access to
the internet.

If the model declines a request and says nothing, the server speaks a fixed
sentence in its place. A declined request is not added to the conversation.

### Reply pipeline

The response stream is split into sentences as it arrives. Each completed
sentence is synthesised and forwarded immediately, so playback begins while the
reply is still being generated.

Three things run at once for a reply, each feeding the next: the model
writes sentences, the synthesiser turns them into audio, and the
connection sends the audio in chunks of 40 ms. Sending is paced: the
server never sends more than 2 s ahead of where playback has reached, so
that the device needs only a small fixed buffer however long the reply.
The server has no report of playback. It takes playback to begin with the
first chunk and to continue for as long as the device has audio; when the
reply itself pauses and the device runs dry, the count starts again from
the next chunk. Because sending is slower than synthesis, synthesis runs
ahead: the next sentence is requested while the current one is being sent,
unless 5 s of audio is already waiting.

Measured on 2026-10-06: with the desktop client and synthetic speech, a
62 s reply was delivered over about 60 s and never ran more than 2.05 s
ahead of playback. On the device, which logs these figures for every
reply, a reply of 33 s held at most 2.08 s of its 4 s at once and never
ran dry (KNOWN-ISSUES R17).

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

Measured on 2026-10-06, server only, with the desktop client on the same
machine sending synthetic speech over a home internet connection, across
four turns: the end of speech was detected within 0.1 s of the recording's
end, Claude's first text came 1.2-1.8 s after that, and the first reply
audio 2.4-3.5 s after it. The budget's figures from end-of-speech detection
onward add up to 1.0-1.8 s, so the server is over budget before the device
is involved. The recordings end in silence, which hides the wait for
end-of-speech detection. Bringing this down is stage 7 (KNOWN-ISSUES R19).

Measured the same day on the device, on the breadboard, over five short
questions in a synthetic voice: 2.2 to 3.3 s from the server's
`stop_capture` to the first sound from the speaker, 2.8 s on average. The
device's own share of that is about 0.07 s, so the budget's 100 ms for
network and playback start holds, and the whole overrun is the server's.

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
| Server paces the reply, device holds 4 s | Device holds the whole reply | A minute of audio is 3 MB; a fixed small buffer works for a reply of any length |
| Reconnecting done by `server_link`, not by the WebSocket client | The client's own automatic reconnect | The server may have moved, so every attempt starts again from the lookup; and the rest of the firmware must hear of every loss |
| The device closes the connection to abandon a turn | A "cancel" message | One mechanism covers every case of doubt, and the server already cancels a turn whose connection closes |
| WiFi power saving off | The default, modem sleep | Mains-powered; the radio's sleep delays streamed audio in both directions |
| OTA-ready partition layout | Single application slot | Costs nothing in 16 MB, and keeps OTA open without a later layout change |
| Streaming transport | Request and response | Fourfold latency difference; cannot be retrofitted cheaply |
| Ring buffer shared by position | One queue per listener | Listeners never take audio from each other, and a past position (the wake word's end) can be replayed |
| Component versions locked (`dependencies.lock` committed, wake-word libraries pinned exactly) | Version ranges | A newer library can break the build or shift the model's scores without any change in the repository |
| A turn cannot be interrupted | Barge-in | With no echo cancellation the device would interrupt itself, and a reply cut short leaves the history unsure of what was heard |
| `error` ends a turn and keeps the connection | Reconnecting after every error | A failed service does not make the connection unhealthy; reconnecting would only add delay |
| Newer connection replaces the older | Refusing a second connection | A device back from a power cut must not be locked out by its own dead connection |
| Device tokens in the server's environment | A table in the database | Keeps credentials apart from the transcripts |
| Deepgram without its SDK | The Deepgram SDK | The interface used is small and stable; the SDK's has changed across major versions |
| Only the current day's conversation kept | Keeping every day | Transcripts are personal data and earlier days are never read again |
