# Architecture

How the Hey Claude voice assistant is designed, and why. Its parts are all
built; what has been measured is in [STATUS.md](STATUS.md), and the open
risks in [KNOWN-ISSUES.md](KNOWN-ISSUES.md). The decisions recorded here
are binding unless explicitly revisited.

## Overview

```
   ┌──────────────── ESP32-S3 device ────────────────┐      ┌──── LAN server ────┐
   │                                                 │      │                    │
   │  INMP441 mic ──► wake-word model ──► capture ───┼─────►│ speech-to-text     │
   │                                                 │  WS  │        ▼           │
   │  NV3007 display ◄── state machine               │      │ Claude             │
   │                                                 │      │        ▼           │
   │  speaker ◄── MAX98357A amp ◄── playback ◄───────┼──────┤ text-to-speech     │
   └─────────────────────────────────────────────────┘      └────────────────────┘
```

The device listens for the wake word, streams the request, and plays the
reply. The server decides everything else: when the request has ended,
what the conversation holds, and what Claude says.

**One turn, start to finish:**

1. The wake word fires on the device. It plays a chime, then sends
   `utterance_start` and streams 20 ms audio frames.
2. The server transcribes as audio arrives (Deepgram), decides that the
   speaker has finished, and sends `stop_capture`.
3. The request joins the day's conversation and goes to Claude, by
   default through the Claude Code tool, which can search the web.
4. Claude's reply is cut into sentences as it streams; each is
   synthesised (Deepgram) and sent as audio at most 2 s ahead of playback.
5. The device plays the audio as it arrives, and `reply_end` closes the
   turn. With the switch closed, it then listens again without the wake
   word.

**Design principles**

1. **The device is stateless.** It holds no conversation history and no
   credentials beyond its own identity, so it can be reflashed or replaced
   without loss.
2. **The server owns all judgement.** End-of-speech detection, the
   conversation and error recovery are on the server; the firmware runs a
   state machine.
3. **Stream everything.** Audio moves in both directions while it is still
   being produced. A request-and-response design cannot reach acceptable
   latency and cannot be retrofitted cheaply.
4. **No secrets in firmware.** API keys exist only on the server.

## Device

### Components

| Component | Responsibility |
| --- | --- |
| `main` | Start-up order, boot banner, the brownout guard, and the serial console with its commands |
| `app_config` | Settings in NVS: loads the running configuration, checks and stores edits, all of a set or none |
| `net` | WiFi: joining, reconnecting with backoff, readable failure reasons, giving up on settings that have never worked |
| `setup` | Setup mode: a hotspot with a settings page and captive-portal DNS; the BOOT button that opens it |
| `audio` | Microphone capture and amplifier playback over I2S: plain 16-bit samples, no buffering or tasks of its own |
| `audio_ring` | The microphone's only reader: a capture task filling a 2 s ring buffer, read by every listener at its own position |
| `wakeword` | Wake-word detection; announces detections and decides nothing |
| `app_state` | The device state, its detail text and the continuous-conversation mark: stored, and announced on change |
| `state_machine` | Decides what the device does next and is the only writer of the state; also runs the request upload and keeps the volume (`volume.c`) |
| `server_link` | The WebSocket connection: finds the server by mDNS, pairs if given a code, connects, authenticates, reconnects, and turns messages into reports |
| `player` | Reply playback: a 4 s buffer and the task that plays it, at the playback level, through a limiter |
| `protocol` | The protocol's numbers, in one header, held to the server's by a test |
| `display` | The panel, its backlight and each state's screen, from the one task that uses LVGL |
| `controls` | The button and the switch: reads both, settles contact bounce, announces presses and moves |
| `board` | The verified pin map and panel geometry |

**How they talk.**

- **Events on the default event loop**, for news with several listeners:
  WiFi up and down (`IP_EVENT_STA_GOT_IP`, `WIFI_EVENT_STA_DISCONNECTED`),
  `APP_STATE_CHANGED` (the display follows it), `WAKEWORD_DETECTED` with
  the position where the phrase ended, `WAKEWORD_LISTENING` when the
  microphone or model stops or resumes, `CONTROLS_BUTTON_PRESSED` the
  moment the button goes down, and `CONTROLS_SWITCH_CHANGED`. A switch
  move is a level, so one the loop has no room for is announced again
  every 10 ms until it goes.
- **Direct calls** for reports with one receiver: `server_link` and
  `player` call a function given to them at start, so a report cannot be
  lost or reordered. Neither knows `state_machine`; the boot sequence
  joins them. Reply audio passes through `state_machine` on its way to
  `player`, and only while a turn is under way, so the tail of an
  abandoned turn never reaches the speaker.
- **One writer of the state.** Components that detect something report
  it and never set the state. The state machine writes it in normal
  operation, setup mode's loop on a setup boot; the console can only
  read it (`display preview` draws a state's screen without setting it).
- **Settings** are loaded once at boot into a read-only structure; edits
  apply after a reboot, so no running code sees a setting change under
  it. The console and the setup page both store through
  `app_config_store()`, which checks every value and writes all or none.
- **Whether the device can hear** has one owner, `wakeword`, which
  answers `wakeword_listening()`. The state machine asks it each time it
  chooses what to show and keeps no copy, so `IDLE` is never shown on a
  device that cannot hear.

### State machine

The eight states are what a person needs to see:

| State | Entered when | Screen |
| --- | --- | --- |
| `BOOT` | Power on | Splash |
| `SETUP` | A setup-mode boot (see Setup mode) | The reason, and how to reach the settings page |
| `CONNECTING` | Boot complete, or a lost connection after its error | Why the last attempt failed |
| `IDLE` | Server session ready | Waiting for the wake word |
| `CAPTURING` | Wake word in `IDLE`, or a reply played in full in continuous conversation | Listening |
| `THINKING` | `stop_capture` | Waiting for the reply |
| `SPEAKING` | The reply's first sound reaches the speaker | Playing |
| `ERROR` | A failed turn, a lost connection, a device that cannot hear or speak, or the third brownout in a row | The reason |

The state machine works in finer steps, its phases, each shown as one of
these: waiting for a reply and holding its first audio both show
`THINKING`; playing and playing out show `SPEAKING`; a failure shown
briefly and a fault that stays both show `ERROR`. Every reason's wording
is in one table in `state_machine`, sized to the panel's detail line; the
sentence in a server `error` goes to the log.

It runs in one task, from one queue, one report at a time, so no two
decisions overlap and nothing needs a lock. It never waits on the network:
everything sent during a turn is sent by the upload task. A deadline is
the queue's wait running out.

**After `ERROR`.** A server `error` ends one turn and keeps the
connection: `ERROR` for 4 s, then `IDLE`. A lost connection: `ERROR`, then
`CONNECTING` with backoff. A device that cannot hear or speak stays on
`ERROR`. A device the server refuses restarts into setup mode, since
retrying cannot fix it.

**The device gives up on a turn by itself** after 15 s of capture with no
`stop_capture`, 40 s of `THINKING` with no reply, 40 s of a reply with no
audio arriving, a message that does not fit its phase, or a request that
lost audio on the way out (the network stalled longer than the ring
holds). It closes the connection, which is how the server learns of it,
shows `ERROR` for 4 s, and reconnects. Each limit outlasts the server's own
for the same wait, so the server's `error` arrives first
([docs/PROTOCOL.md](docs/PROTOCOL.md), Time limits). After a reply, wake
words are ignored for 0.7 s, so the reply's echo cannot start a request.

**Interrupting with the button.** A press while listening, thinking or
speaking stops capture and playback at once, shows `IDLE` and sends
`cancel`. Until the server answers with the turn's end, normally within a
quarter of a second, a phase that looks like `IDLE` does not take the wake
word. Nothing about an interrupted turn is shown as a failure: if the
server does not answer, the connection is closed quietly. A press during
the chime only stops it; one after `reply_end` only stops the playback;
one at any other time does nothing.

**Continuous conversation.** With the switch closed, a reply played to
its end is followed by the chime and a new request. The switch is the
setting, read when a reply ends; nothing is stored. The device returns to
`IDLE` when nothing is said within the server's 5 s wait, on any failure,
on the button, with the switch open, on a lost connection, and after ten
follow-ups in a row. The screen marks the mode on `IDLE` and `CAPTURING`.
To the server a follow-up is an ordinary request.

### Setup mode

Settings can be entered from a phone. Setup mode is a way of starting up:
whatever wants it notes a reason in RTC memory, which survives a restart,
and restarts the chip; the boot sequence then starts setup mode instead of
the state machine, the server link and WiFi, and leaves it by restarting
again. Nothing running ever has to be stopped.

| Way in | Shown as | Closes by itself |
| --- | --- | --- |
| No WiFi network, or neither token nor pairing code, stored | "WiFi not set up", "Not paired with the server" | No |
| `setup` on the console | "Asked for at the console" | After 5 min |
| BOOT held 3 s | "BOOT button held" | After 5 min |
| Never-connected WiFi settings refused 5 times in a row | "WiFi password refused" | No |
| The same, network not found in 10 min | "WiFi network not found" | No |
| The server refuses the device | "Server refused this device" | No |
| The server refuses the pairing code | "Pairing code refused" | No |

Setup for missing or failed settings has no deadline, since going back to
them would only fail again. Holding BOOT leaves setup mode at any time.

1. The device scans for networks in range, so the page can offer them;
   the scan comes first because an access point cannot change channel
   under a joined phone.
2. It becomes an access point, "Hey-Claude-" and four characters of its
   MAC, with WPA2 and a random 8-character password made for each entry
   and never stored. The name, password and page address are on screen.
3. A captive-portal DNS answers every name with the device's address, so
   a phone checking for the internet is sent to the page.
4. The page asks for the WiFi network and password, a pairing code or a
   token and, optionally, the server address, and shows the device
   identifier. The stored password and token are never sent back: empty
   fields keep them.
5. A save is checked by the console's rules, stored all or none, and the
   device restarts. The WiFi password is not tried first, since that would
   move the access point to the network's channel, away from the phone.

`net` keeps a checksum of the WiFi settings that last connected, in an NVS
namespace of its own. Settings that have connected are never given up:
their network is only away. The hotspot and DNS were ported from the
author's earlier project.

### Recovering by itself

Three things restart the chip unasked: a crash, the task watchdog and the
brownout detector. Afterwards the line under `CONNECTING` names the cause,
"Restarted: crash", "watchdog" or "supply dipped", until the first attempt
to reach the server has ended. Every boot logs the firmware version, flash
size, free memory and reset reason; brownouts are logged as errors, so a
supply fault shows without instruments. A crash leaves a core dump, read
with `idf.py coredump-info`.

- **Task watchdog:** 5 s, treated as a crash. It watches each core's idle
  task, which catches a task hogging a core, and the three tasks without
  which the device cannot hear, speak or decide: capture, the player and
  the state machine. Each asserts at build time that its longest wait is
  well inside the limit. Tasks that wait on the network or run library
  code are not watched; the state machine's deadlines cover them.
- **Brownout guard:** at the third brownout in a row, each within 60 s of
  the boot before it, the boot stops before WiFi and the state machine and
  rests on `ERROR`, "Power supply too weak", with the console working,
  until power is cycled or the console restarts it. The count is in RTC
  memory, which survives a restart but not a power cut. The guard is asked
  after the display, microphone and wake word have started, so it covers
  a supply that fails when the radio or amplifier comes on.

Both were tested with test builds that hung a task or counted ordinary
restarts as brownouts; a real brownout has not been seen (R2).

### Task and core allocation

| Core | Tasks | Why |
| --- | --- | --- |
| 0 | WiFi and lwIP, WebSocket client, link, state machine, upload, display | Network jitter is tolerable for rendering |
| 1 | Capture, wake word, player | Audio isolated from WiFi interrupt load, the likeliest cause of I2S underruns |

This table is the one place task priorities are recorded; code refers
here. Higher runs first.

| Task | Priority | Core | Notes | Watched |
| --- | --- | --- | --- | --- |
| WiFi driver | 23 | 0 | ESP-IDF's | No |
| `esp_timer` | 22 | 0 | ESP-IDF's; short callbacks: WiFi retry and give-up, the BOOT button's watch, the button and switch every 10 ms | No |
| Default event loop | 20 | 0 | ESP-IDF's; also runs `net`'s handlers, which may write NVS or restart. Stack 3584 bytes, raised from 2304 (measured: 308 never used) | No |
| Capture | 12 | 1 | Reads the microphone into the ring; must never fall 75 ms behind | Yes |
| Player | 10 | 1 | Feeds the amplifier; above the wake word so a model run cannot delay the speaker | Yes |
| Audio test tasks | 10 | 1 | Console `audio` commands, short-lived | No |
| Wake word | 8 | 1 | Every 30 ms; reports its own failure | No |
| WebSocket client | 7 | 0 | The component's task; receives reply audio | No |
| Upload | 6 | 0 | Sends request frames while capturing; a send can block for seconds | No |
| State machine | 5 | 0 | Each event well under a millisecond, except storing a changed volume, done only in `IDLE` | Yes |
| Link | 5 | 0 | Looks up, connects, waits for the connection to end. With a pairing code, makes the HTTP request instead and hands the token to main.c, which stores it and restarts; 6 KB of stack for the HTTP client, of which about 3.3 KB was left unused by a pairing | No |
| Display | 4 | 0 | LVGL drawing; sleeps between frames | No |
| Web server | 5 | either | Setup mode only | No |
| Captive DNS | 4 | either | Setup mode only | No |
| Console | 2 | either | ESP-IDF's REPL | No |
| `main` | 1 | 0 | Start-up, then ends; on a setup boot runs setup mode's loop | No |
| Idle, one per core | 0 | each | FreeRTOS's | Yes |

The audio set-up runs briefly on core 1, so that the I2S interrupts are
placed there.

### Memory

| Buffer | Where | Why |
| --- | --- | --- |
| Audio ring (64 KB, 2 s) | PSRAM | Large, latency-tolerant |
| LVGL frame buffer (~121 KB) | PSRAM | Too large for internal SRAM; DMA sends it from PSRAM at no measurable cost |
| Wake-word tensor arena (~28 KB, 25.5 KB used) | Internal SRAM | Runs continuously; PSRAM latency would cost frames |
| Wake-word model copy (~60 KB) | PSRAM | Read-only weights, copied from flash at boot for alignment |
| Reply buffer (384 KB) | PSRAM | 4 s at the highest rate the amplifier takes; a 24 kHz reply uses half |
| Chime samples (6.7 KB) | PSRAM | Computed at boot, copied into the reply buffer when played |
| WebSocket client buffers (2 x 4 KB) | Internal SRAM | The component's own |
| Network queues | Internal SRAM | Touched from interrupt context |

Measured with `mem`, connected and after several turns: about 100 KB of
internal RAM free, about 56 KB at its lowest, 7.7 MB of PSRAM free, and at
least 1.3 KB of every task's stack never used.

### Audio

**Capture.** The INMP441 on I2S0 at 16 kHz, 16-bit mono, the rate both
the wake-word model and speech-to-text expect, so nothing is resampled.

- The microphone sends 24-bit samples; the top 16 bits are kept.
- It runs from boot and never stops: it powers down when its clock stops
  and takes about 2 s to settle after.
- With `L/R` grounded it uses the left slot. The controller reads both
  slots and keeps the left: its "mono" mode delivered both, interleaved
  with zeros, on this chip.
- Audio not read within about 75 ms is dropped by the driver.

**The ring.** The capture task in `audio_ring`, the microphone's only
reader, copies each 15 ms chunk into a 2 s ring in PSRAM. Every listener
(wake word, upload, `audio loop`) reads at its own position; one more than
2 s behind skips forward and is told how much it missed. A listener can
start at a position noted earlier, which is how the upload begins exactly
where the chime ended. A mutex guards the ring; capture waits about a
millisecond at most.

**Upload.** When a request starts, the upload task reads from where the
chime ended, sends the frames already waiting at once, then one 20 ms
frame as each is captured, until `stop_capture`. If any of the request's
audio is no longer in the ring when it comes to read it, the request is
given up rather than sent with a hole in it.

**Chime.** Two 70 ms notes, 660 then 880 Hz, faded over 8 ms, at 30 % of
full scale, computed at start by `player` (`chime.c`) and played through
the reply path. The screen changes with the sound, and the request starts
when it is over: the person has a signal to wait for, and the chime never
reaches speech recognition; words spoken during it are lost. The request
begins 220 ms after the wake word: 140 ms of notes, about 20 ms for the
amplifier to wake, and silence written after the last note to push it out
of the driver, which also keeps its tail out of the request. If the chime
cannot play, or no report comes within 1 s, the request starts anyway.

**Playback.** The MAX98357A on I2S1, 16-bit mono, at the rate in
`reply_start`, 24 kHz for replies. `player` holds up to 4 s between the
network, which delivers in bursts, and the amplifier, which takes audio at
a fixed speed; the server sends at most 2 s ahead (Reply pipeline). A
chunk that does not fit ends the reply and the connection. Playback starts
once 80 ms is waiting, and stops within about 100 ms. The amplifier's `SD`
pin is held low except during playback, which avoids its switch-on click;
high, it selects the left slot.

**Volume.** Levels 1 to 10, 3 dB apart, set by voice (Volume requests,
under Server) or the console's `volume`. Level 8, the default, plays the
audio as sent; level 10 is +6 dB, which the voice has room for (it peaks
about 9 dB under full scale); level 1 is -21 dB. A limiter (`gain.c`)
turns the level down the moment a sample would pass 1 dB under full scale,
then back up over about 100 ms. The level is read at the start of each
reply or chime, and the chime follows it down to a floor of -9 dB.

The level is kept in NVS in its own namespace, `volume`, apart from the
settings: it is the one value that changes while the device runs. A level
from the server reaches the player as it arrives, before the confirmation
that follows it. Writing flash pauses the audio tasks for milliseconds, so
a changed level is stored only in `IDLE`.

**WiFi power saving is off.** The radio's sleep holds data back for up to
100 ms each way, heard as gaps; the device is mains-powered.

### Wake word

Detection runs on core 1 with a microWakeWord model, TensorFlow Lite for
Microcontrollers and ESPHome's port of its feature front end. Every 10 ms
the front end turns the latest 30 ms of audio into 40 frequency-band
values, evening out steady noise and loudness; every third slice the model
gives the probability that the phrase has just ended. The last five are
averaged, and a detection fires when the average passes the cutoff; then,
and at start-up, the detector waits a second.

The model is two files in `firmware/components/wakeword/models/`: the
network and a manifest of its training settings. The build takes the
cutoff, averaging window, step and memory size from the manifest, their
only source. The model in use is `hey_claude_run3`, trained for this
project ([docs/WAKEWORD-TRAINING.md](docs/WAKEWORD-TRAINING.md)); its
name, printed at boot and by `wake`, ties each test to its run. The
component versions are pinned exactly, because the scores depend on their
arithmetic.

Measured on this model's shape: about 2.1 ms a run on average, 4.9 ms at
most, every 30 ms (about 8 % of core 1), with 25.5 KB of working memory and
a 61 KB model copy.

### Display

LVGL 9 drives the NV3007 over SPI2 at 80 MHz, the highest SPI2 supports,
on its dedicated pins GPIO 10-12. The panel is write-only and has no
tearing-effect output, so drawing cannot follow its refresh (R13). The
initialisation is LVGL's `lv_nv3007` sequence.

- **Landscape, 428 x 142,** by the controller's own address mode, so
  rotation costs nothing. Its memory is 168 columns wide for the 142
  visible; in LVGL's `ROTATION_270`, drawing is offset by 14 rows.
- **One full-screen buffer** (121,552 bytes) in PSRAM, in the panel's byte
  order (`RGB565_SWAPPED`). Only changed rectangles are redrawn, and DMA
  sends each from PSRAM. A full frame takes 12.5 ms (measured); the state
  animations change a 142 x 142 region, about 4 ms.
- **The backlight** is PWM at 25 kHz, above hearing. It stays off until
  the first frame is drawn, then fades in, and dims to 25 % in `IDLE`.
- **The screens** are placeholders, kept as they are. Each state's title,
  detail text, animation, brightness and continuous-conversation mark are
  in one table in `display_ui.c`. Changing the mode alone shows or hides
  the mark without restarting the animation.

### Flash layout

| Partition | Offset | Size | Purpose |
| --- | --- | --- | --- |
| `nvs` | 0x9000 | 24 KB | Settings (`config`), the volume (`volume`), WiFi settings that have worked (`net`) |
| `otadata`, `phy_init` | 0xF000 | 12 KB | OTA slot selection, RF calibration |
| `ota_0`, `ota_1` | 0x20000 | 4 MB each | Two application slots |
| `coredump` | 0x820000 | 64 KB | Last crash |
| unallocated | 0x830000 | ~7.8 MB | Reserved |

Over-the-air updates are not adopted; the device is flashed over USB, and
the two slots keep OTA open without a layout change. Flash runs in QIO
mode at 80 MHz; PSRAM is octal at 80 MHz.

## Protocol

An overview; the reference, with every rule, limit and close code, is
[docs/PROTOCOL.md](docs/PROTOCOL.md).

One WebSocket per device, opened at boot and held open. Control messages
are JSON text frames; audio is binary frames of little-endian signed
16-bit PCM.

```
GET /v1/stream
Authorization: Bearer <device_token>
X-Device-Id: <device_id>
```

The token proves the device is the one its identifier names, so nothing
else on the network can spend the server's credit or continue a device's
conversation. The server checks it, then sends `ready`. A newer connection
for an identifier replaces the older one.

**Pairing.** A device with a pairing code instead of a token trades it
first, in one HTTP request to the server (docs/PROTOCOL.md, Pairing), and
restarts with the token it is given. A separate request keeps the
WebSocket's one rule, a token or a refusal, unchanged, and a code entered
on a phone is 6 digits rather than 43 characters.

**Discovery.** The server advertises itself by mDNS, with the one address
on its machine's default route; the device looks it up at every connect,
so the server's address can change. The `server_url` setting overrides
the lookup.

| Direction | Message | Meaning |
| --- | --- | --- |
| Device → server | `utterance_start` | Wake word fired; 20 ms frames of 16 kHz mono follow |
| Server → device | `stop_capture` | The speaker has finished |
| Server → device | `reply_start` | Audio follows, with its format and rate |
| Server → device | *binary* | Reply audio, chunks of at most 40 ms |
| Server → device | `reply_end` | The turn is over once the audio has played |
| Server → device | `error` | The turn was abandoned; the connection stays open |
| Server → device | `volume` | Play at this level from now on |
| Device → server | `cancel` | The button was pressed; answered with `reply_end` |

A request is taken only when no turn is running, and every turn ends with
exactly one `reply_end` or `error`. When nothing is said, the server sends
`stop_capture` and then `reply_end` with no reply. The server pings every
20 s and ends a capture after 13 s.

## Server

Python 3.11+, asyncio throughout, FastAPI for the WebSocket endpoint.

```
server/
  protocol.py     Message shapes and constants of the device protocol
  transport/      WebSocket endpoint, device auth, one connection per device,
                  framing and pacing of reply audio
  pipeline/       One turn from request to reply; sentences; volume requests
  stt/            Speech-to-text interface and Deepgram implementation
  llm/            Two Claude clients behind one interface: Claude Code
                  (the default) and the API
  tts/            Text-to-speech interface and Deepgram implementation
  conversation/   The day's conversations, in SQLite
  discovery.py    mDNS advertisement
  spoken.py       Numbers as spoken, for the volume and the prompt
  config.py       Configuration from the environment
  tools/          Desktop client, Claude Code bench
  tests/          Tests with stand-ins for the three services
```

**A turn** is one task in `pipeline/`. It decides what happens and reports
each step (capture done, reply audio, reply done, failed, interrupted) to
the connection, which alone knows how a step goes on the wire. Everything
of one turn on the connection's side is one object, so the next starts
clean. A completed turn is added to the conversation whole. An interrupted
one tells the device at once, before its work has finished stopping, and
is added with its reply as far as its audio had begun to be sent, then
`[interrupted]`, which the system prompt explains. A turn whose device
disconnects is cancelled and not added. The stored history always
alternates between the person and the assistant.

**Device tokens** come from pairing or from `DEVICE_TOKENS`, which maps
each identifier to its token. Paired devices are kept as SHA-256 hashes
of their tokens in `server/data/devices.sqlite3`, a file apart from the
conversations so that credentials and transcripts are never together.
The pending code is kept there the same way, which is how
`python -m server pair`, a separate process, hands it to the server. A
paired token takes the place of the device's `DEVICE_TOKENS` entry, and
the server says so at start; `python -m server unpair <id>` withdraws it.
Tokens are compared in constant time.

**Volume requests.** A request only about the volume ("louder", "too
loud", "volume five", "maximum volume") is recognised from its transcript
by `pipeline/volume.py` and answered without the model: the device is sent
its new level and the synthesiser says, for example, "Volume six." The
device states its level with each request, so the server keeps none. Such
turns are not added to the conversation.

- The phrases are built from one table per direction, so whatever turns
  the volume up can be said, mirrored, to turn it down.
- A request may hold several commands ("volume three, volume three",
  "louder, volume nine"): it is cut into clauses at the transcript's
  punctuation and the commands are applied in order. The reply says what
  happened: the final level, a step that met the end of the scale, a
  level off the scale that was ignored.
- A phrasing the table misses goes to the model, whose prompt says how
  the volume is changed and forbids it to claim it changed it.
- The log gives the commands as understood ("+1 =9"), and the words only
  when transcripts are logged.

### Speech services

Deepgram for both directions, reached directly over its WebSocket and
HTTP interfaces, without its SDK, whose interface has changed across major
versions; each side sits behind an interface, so a provider is replaced by
writing one class. Alternatives on record: ElevenLabs Flash for voice
quality; faster-whisper with Piper for a fully local pipeline.

- **End of a request:** 400 ms of silence after speech, or Deepgram's
  second signal from word timings, which works over background noise. One
  service for both transcript and endpointing keeps them in agreement and
  needs no separate voice-activity detector. With no word within 5 s, the
  turn ends with no reply. Longer silences are not dependable on the
  microphone's audio (R18).
- **Transcript:** punctuation and capitals, so Claude reads sentences;
  numbers stay as words. Deepgram's fuller formatting held the end of
  speech back after a number, so it is not used.
- **Synthesis:** voice `aura-2-luna-en`. A failure to connect gets a
  second try, each cut at half the 5 s allowed, one limit over both; a
  sentence whose request fails before any audio arrives is tried once more.

### Conversation and caching

Conversations are keyed by device and chat date and kept in SQLite, so a
server restart loses nothing. The date advances at **04:00 local time**,
so a late conversation is not split. Only the current day is kept: the
first exchange of a new day deletes the earlier ones.

The whole day's history is sent every turn, so **prompt caching** keeps
cost and time to first text flat through the day. The system prompt holds
nothing that varies and comes first; cache breakpoints sit at its end and
at the end of the new request, so each turn reads everything before it
from the cache. Caching starts once the prompt passes the model's minimum,
after a few exchanges (measured: from the third). A day of speech stays
far below the context window.

### Claude

| Setting | Value | Why |
| --- | --- | --- |
| Model | `claude-opus-5` | |
| Thinking | Adaptive | This model's default; disabling it has documented failure modes |
| Effort | Low | Spoken replies need little reasoning, and effort is the main latency lever |
| Streaming | On | Synthesis starts before the reply is complete |
| Fallbacks | Server-side, default routing | Handles refusals without a maintained model list |
| Output ceiling | 8000 tokens | Stops a runaway reply being spoken for minutes |
| Silence limit | 24 s with no text | The SDK's default of ten minutes would hold a device in `THINKING` as long |

The system prompt keeps replies speakable: no markdown, lists or
headings, short by default. It says the input is a transcript that may
hold misheard words. Through the API it adds that the assistant has no
clock and no internet; through Claude Code that part differs. A request
the model declines in silence is answered with a fixed sentence and not
added to the conversation.

### Claude through Claude Code

The default (`LLM_BACKEND=claude_code`). The server runs the Claude Code
tool, `claude -p`, under the login of whoever is signed in on its machine;
the device has one user, that account's owner. The assistant can search
the web and read pages. `LLM_BACKEND=api` uses the API instead, with no
web access. Everything else is the same: interface, model, effort,
sentences spoken as they come.

One process runs per turn. It gets the system prompt on its command line
and, on standard input, the day's exchanges and the new request, so
transcripts are not in the process list. It keeps no session: the
conversation stays in the server's store. The tool caches the prompt
itself.

The tool is a general agent used as a voice, so it is locked down:

- **Tools:** web search and page reading only, nothing that runs a command
  or touches a file. Which pages may be read is `CLAUDE_FETCH_DOMAINS`:
  any site (the default), named hosts only, or none.
- **None of the signed-in person's set-up:** no settings files, hooks,
  plugins, skills, instruction files or MCP servers, which would include
  the account's connectors.
- **Nowhere to look:** it runs in an empty temporary directory.
- **No credentials:** its environment is built from a short list, without
  the server's keys and tokens.
- **Checked every run:** the tools, MCP servers and permission mode it
  reports at start are checked, and a run with anything more is stopped
  before it answers.

Because the reply is spoken:

- The prompt asks for "Let me check." before a search, and the client
  ends that sentence where the tool reports the end of a text block, so it
  is spoken before the search rather than with the answer.
- A closing list of sources, which the search tool asks for, is forbidden
  by the prompt and removed by the client if it comes, so no web address
  is read aloud or stored.
- The 24 s silence limit counts text that can be spoken, not lines the
  process prints during a search.

The tool updates itself, and its options and output are not a published
interface; after an update, `server/tools/claude_cli_bench.py` shows
whether they still hold ([docs/OPERATING.md](docs/OPERATING.md), Updating
Claude Code). What the open web costs in safety is R21.

### Reply pipeline

Three things run at once, each feeding the next: the model writes
sentences, the synthesiser turns them into audio, and the connection sends
it in 40 ms chunks.

- **Pacing:** the server never sends more than 2 s ahead of playback, so
  the device needs only a small fixed buffer, whatever the reply's length.
  It has no report of playback: it assumes playback starts with the first
  chunk and runs while the device has audio, and restarts its count when
  the reply itself pauses and the device runs dry (R17).
- **Synthesis runs ahead** of sending: the next sentence is requested
  while the current one is sent, unless 5 s of audio is already waiting.

## Latency budget

From the end of speech to the first sound:

| Step | Budget |
| --- | --- |
| Capture and upload, overlapped with speech | ~0 ms |
| End-of-speech detection | 300-500 ms |
| Claude's first text | 600-1200 ms |
| First sentence synthesised | 300-500 ms |
| Network and playback start | 100 ms |
| **Total** | **1.3-2.3 s** |

A non-streaming pipeline lands at 5-9 s, which is why streaming is a
structural requirement.

**The budget is not met, and that is accepted.** On the device a plain
answer comes after about 2 to 5 s. The device's own share is about 0.07 s;
nearly all of the wait, and all of its spread, is Claude's time to its
first text. The inexpensive remedies were taken, and the rest are listed
in R19. Opening service connections ahead of need was measured and gained
nothing, so connections are opened when needed.

## Deployment

The server runs on the local network. Transport is plain WebSocket and
device tokens are shared secrets; moving off the LAN requires TLS and a
stronger device credential (R8).

## Decisions and rationale

| Decision | Alternative rejected | Reason |
| --- | --- | --- |
| microWakeWord | Espressif WakeNet custom training | Custom phrases need a paid corpus of 500+ speakers |
| ESP-IDF v5.5.x | v5.3, v6.x | v5.3 predates the NV3007 and LVGL work; v6 was a beta |
| Server-side endpointing | On-device voice-activity detection | Better models, no device CPU, simpler firmware |
| Server-side history | Device-side history | Survives reboots and WiFi loss, and enables prompt caching |
| Two I2S controllers | A shared bus | No contention, and a different rate each way |
| Ring buffer shared by position | One queue per listener | Listeners never take audio from each other, and a past position can be replayed |
| Streaming transport | Request and response | Fourfold latency difference; cannot be retrofitted cheaply |
| Server paces the reply, device holds 4 s | Device holds the whole reply | A minute of audio is 3 MB; a small fixed buffer works for any length |
| Settings entered through the console | Compiled in, or flashed from a file | Nothing sensitive is written to disk on the development machine |
| Settings also entered from a phone, on the device's own hotspot | Espressif's Bluetooth provisioning and app | No app to install, no Bluetooth stack in RAM |
| Setup mode entered by restarting into it | Switching the running device to a hotspot | Nothing running has to be stopped; no half-switched radio |
| WiFi password saved without being tried | Joining before saving | Trying would move the hotspot's channel away from the phone; failing settings bring setup back |
| mDNS discovery | A fixed server address | The server's LAN address changes |
| Reconnecting done by `server_link` | The WebSocket client's own reconnect | Each attempt starts from the lookup, and the firmware hears of every loss |
| The device closes the connection to abandon a turn | A message for "I gave up" | One mechanism for every doubt; the server already cancels a turn whose connection closes |
| `cancel` answered with an ordinary `reply_end` | A "cancelled" message | Every turn still ends with exactly one `reply_end` or `error` |
| An interrupted turn kept as far as it was sent | Forgetting it | A person often interrupts to change direction, and Claude then knows what it said |
| Interrupting only with the button | Interrupting by speaking | With no echo cancellation the device would interrupt itself |
| Continuous conversation by a switch, read when a reply ends | A stored setting, or turned on by voice | Visible, changed by hand, nothing stored |
| At most ten follow-ups in a row | Ending only on silence | Bounds what a television or other voices can keep going, every capture of which goes to the server |
| `error` ends a turn and keeps the connection | Reconnecting after every error | A failed service does not make the connection unhealthy |
| A newer connection replaces the older | Refusing a second one | A device back from a power cut must not be locked out by its own dead connection |
| Pairing with a short code | Only a token typed in | 6 digits typed on a phone instead of 43 characters, and no editing of `.env` |
| Paired tokens kept as hashes, in a file of their own | Plain text in `.env`, or a table in the conversation database | The file lets no one connect as a device, and credentials stay apart from transcripts |
| Deepgram without its SDK | The SDK | The interface used is small and stable; the SDK's has changed across versions |
| Only the current day kept | Every day kept | Transcripts are personal data and earlier days are never read |
| WiFi power saving off | Modem sleep, the default | Mains-powered; the radio's sleep delays streamed audio |
| A watchdog timeout restarts the chip | A warning in the log | A device that has stopped deciding cannot say so; a restart recovers it, and the core dump keeps the evidence |
| Brownout count in RTC memory | A count in NVS | It need only outlive a restart; nothing written to flash |
| OTA-ready partition layout | One application slot | Costs nothing in 16 MB, and keeps OTA open |
| Component versions locked | Version ranges | A newer library can break the build or shift the model's scores with no change here |
| End-of-speech silence of 400 ms | 1 s, to allow a pause | At 1 s the service's silence signal was not dependable on the microphone's audio |
| Service connections opened when needed | Opened ahead | Measured: no gain |
| Claude through Claude Code by default | The API by default | It answers questions about the present, and a plain answer's wait is the same; the API stays one setting away |
| Through Claude Code, the conversation sent as text each turn | A session kept by the tool | One copy of the history, deleted with the day |
| Through Claude Code, one process per turn | A long-running process | A living process gathers a conversation of its own; starting one costs nothing measurable |
| Through Claude Code, any site readable by default | Search only, or a list of sites | Search alone missed the weather; a list missed the news. The assistant holds one day and cannot act, so the risk was accepted (R21) |
| Silence from Claude counted in spoken text, up to 24 s | Lines printed; 20 s | A search prints lines throughout, and can stay silent past 20 s; 24 s still ends a second before a device gives up |
