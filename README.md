# Hey Claude

A self-contained voice assistant built on an ESP32-S3. Say the wake word, ask a
question, and hear Claude answer out loud. A small TFT panel shows what the
device is doing at each moment.

> **Project status: device and server built, being joined.** The
> circuit is built on a breadboard. The firmware joins WiFi, records and
> plays audio, shows its state on the display, and detects the wake word
> "Hey Claude" on the device. The wake-word model is trained on one
> speaker's recordings and detects that speaker well; its false detections
> are not yet measured on the device. The server turns a spoken request
> into a spoken reply and has been tested from a desktop with synthetic
> speech. Connecting the two is under way: the device finds the server and
> connects to it, and the first spoken conversation is still being
> debugged. See [STATUS.md](STATUS.md) for the current stage.

## How it works

```
   ┌──────────────── ESP32-S3 device ────────────────┐      ┌──── LAN server ────┐
   │                                                 │      │                    │
   │  INMP441 mic ──► wake-word model ──► capture ───┼─────►│ speech-to-text     │
   │                                                 │  WS  │        ▼           │
   │  NV3007 display ◄── state machine               │      │ Claude API         │
   │                                                 │      │        ▼           │
   │  speaker ◄── MAX98357A amp ◄── playback ◄───────┼──────┤ text-to-speech     │
   └─────────────────────────────────────────────────┘      └────────────────────┘
```

1. The device listens continuously for the wake word, entirely on-device. No
   audio leaves the device until the wake word fires.
2. After the wake word the device plays two short notes to say it is
   listening. What is said after them streams to the server as it is spoken.
3. The server transcribes the audio and decides when the speaker has finished,
   then tells the device to stop capturing.
4. The transcript is appended to the day's conversation and sent to Claude.
5. Claude's reply is streamed, converted to speech sentence by sentence, and
   streamed back to the device, which plays it as it arrives.
6. The display runs a distinct animation for each state throughout.

Each request requires the wake word, and a request runs to its end: a
second one cannot interrupt it. Requests made on the same day continue the
same conversation, so follow-up questions retain context. The conversation
resets at 04:00 local time.

## Hardware

| Part | Role |
| --- | --- |
| ESP32-S3 "N16R8" dev board | 16 MB flash, 8 MB octal PSRAM, dual-core 240 MHz |
| INMP441 | I2S MEMS microphone |
| MAX98357A | I2S Class-D amplifier, 3 W |
| 4 Ω speaker | Output |
| NV3007 2.79" TFT, 142x428, SPI | Status display |
| 1000 µF / 10 V electrolytic | Amplifier supply decoupling |

### Power requirements

Peak draw is roughly **1.4 A at 5 V** — amplifier transients into a 4 Ω load
plus WiFi transmit bursts. A 500 mA USB 2.0 port is not sufficient and will
cause brownout resets during loud playback. Use a 1 A or greater supply.

The amplifier must be fed from the board's 5 V rail, not 3.3 V, with the
1000 µF capacitor placed directly at its supply pin. The amplifier's `GAIN` pin
is configured for 6–9 dB rather than maximum; volume is controlled digitally
instead. At 4 Ω, maximum analogue gain produces distortion
rather than usable loudness.

The board sold as "N16R8" used here is not built on the ESP32-S3-WROOM
module. It carries a bare ESP32-S3R8 chip, with the 8 MB PSRAM inside the chip
package and 16 MB of flash as a separate part, plus a CH340K USB-serial bridge
on its UART port. Silkscreens and pin orders differ from vendor pinout images
of similar boards; the board's own silkscreen is authoritative.

### Pinout

Verified on the breadboard during stage 0 by a functional test of each
peripheral. Assembly procedure and results: [docs/BRINGUP.md](docs/BRINGUP.md).

One table per module, listing every pin by the name printed on the module, so
that a loose wire can be traced from the module's side. The ground and 3.3 V
rails are the breadboard rails fed from the board's ground and 3.3 V pins; all
grounds are common.

**INMP441 microphone** — I2S controller 0

| Module pin | Connects to | Notes |
| --- | --- | --- |
| `SCK` | GPIO 4 | Bit clock |
| `WS` | GPIO 5 | Word select (left or right channel) |
| `SD` | GPIO 6 | Audio data, microphone to board |
| `L/R` | Ground rail | Selects the left channel; must not float |
| `VDD` | 3.3 V rail | |
| `GND` | Ground rail | |

**MAX98357A amplifier** — I2S controller 1

| Module pin | Connects to | Notes |
| --- | --- | --- |
| `LRC` | GPIO 16 | Word select |
| `BCLK` | GPIO 15 | Bit clock |
| `DIN` | GPIO 7 | Audio data, board to amplifier |
| `GAIN` | Not connected | Unconnected sets 9 dB. Tied to ground it sets 12 dB, and to ground through 100 kΩ, 15 dB: the hardware way to make replies louder (KNOWN-ISSUES, Caveats) |
| `SD` | GPIO 17 | Enable: high = on, low = off |
| `GND` | Ground | |
| `Vin` | Board 5 V pin | **5 V, not 3.3 V.** Dedicated lead, not the shared rail |
| Speaker `+` / `−` | 4 Ω speaker | |

The 1000 µF capacitor sits directly across the amplifier's `Vin` and `GND`
pins. It is polarised: the side marked with a stripe is negative and goes to
`GND`. Reversed, it can burst.

**NV3007 display** — SPI controller 2. Pins in the order printed on the module.

| Module pin | Connects to | Notes |
| --- | --- | --- |
| `GND` | Ground rail | |
| `VDD` | 3.3 V rail | |
| `SCL` | GPIO 12 | SPI clock |
| `SDA` | GPIO 11 | SPI data, board to display (MOSI) |
| `RES` | GPIO 8 | Reset |
| `DC` | GPIO 9 | Command or pixel data |
| `CS` | GPIO 10 | Chip select; the `CS-LOW` solder jumper stays open |
| `BL` | GPIO 14 | Backlight, dimmed by PWM from the LEDC peripheral |

GPIO 26–37 are reserved by the flash and PSRAM and must not be used. GPIO 0, 3,
45 and 46 are strapping pins; GPIO 19 and 20 carry native USB.

## Software prerequisites

- **ESP-IDF v5.5.x** — see [CLAUDE.md](CLAUDE.md) for why this version is pinned.
- **Python 3.11+** for the server.
- Accounts and API keys for Anthropic and the configured speech provider. Keys
  live on the server only and are never present in firmware. See
  [SECRETS.md](SECRETS.md).

## Building and setting up the device

From an ESP-IDF v5.5 shell in `firmware/`:

```sh
idf.py -p PORT flash monitor
```

On Windows, open the ESP-IDF shell from PowerShell, not from Git Bash or
another MSYS shell. ESP-IDF refuses to activate there, including in a
PowerShell started from inside Git Bash.

The board has two USB-C ports. Use the one marked **UART**: the settings
console reads commands only there. The native USB port shows the log but does
not accept input.

Settings are entered once through the console and kept on the device across
reboots and reflashing:

```
config set wifi_ssid "Network name"
config set wifi_pass "password"
config show
reboot
```

| Key | Meaning |
| --- | --- |
| `wifi_ssid` | Network to join. 2.4 GHz only; the ESP32-S3 has no 5 GHz radio. |
| `wifi_pass` | 8-63 characters, or 64 hexadecimal digits. Unset for an open network. |
| `server_url` | Optional. Empty means the server is discovered on the local network. |
| `device_id` | Optional. Defaults to `hc-` plus the last six hex digits of the MAC address. Letters, digits and `-`. |
| `device_token` | Credential for the server, used from stage 6: 24 to 64 characters, the same value as this device's entry in the server's `DEVICE_TOKENS`. |

Values containing spaces go in double quotes. Inside a value, a backslash is
written `\\` and a double quote `\"`. `config show` never prints the password
or token, and the console clears its line history after either is entered.
Changes take effect after `reboot`.

Two console commands test the audio hardware:

```
audio loop 5        record 5 s, print the level, play it back louder
audio tone 20 3     play a 440 Hz tone at 20 % of full volume for 3 s
```

`audio loop` boosts the recording so that it can be heard, which also makes
background hiss audible; the printed level is that of the unboosted recording.
`audio tone 100` is loud and draws the most supply current the device will
ever need, which makes it the test for a weak supply or cable.

The display shows the device's state, which the firmware's state machine
alone decides. The console can read it, and check the panel:

```
state                     print the current state, with its reason if it
                          has one
display test 30           test pattern for 30 s, then frame timing
display preview error "Speech synthesis failed" 10
                          draw a state's screen for 10 s; the state itself
                          is not changed
```

The test pattern draws a 1-pixel frame in a different colour on each edge:
red top, green bottom, blue left, white right. A missing edge means the
drawing offset is wrong; stray or wrongly coloured pixels mean the SPI clock
is too fast for the wiring.

Wake-word detection runs from boot. A detection while the state is `idle`
starts a request to the server; the device is `idle` only while it is
connected to one. The `wake` command shows and tunes detection:

```
wake                      detections, rate per hour, scores, model timing
wake reset                clear the counters to start a measurement
wake log on               print the highest score once a second
wake cutoff 0.95          change the detection threshold until reboot
```

Two more commands report on the device itself:

```
link                      WiFi and server connection: up or not, where the
                          server was found, why the last connection ended
temp                      temperature inside the chip
mem                       free memory, and each task's unused stack
```

`link` tells "the device cannot find the server" from "the server does not
answer" when the screen shows `connecting` or `error`. `temp` reads the
chip's own sensor, which runs hotter than the chip's surface; about 55 °C
with the device idle is normal. Every test on the device notes it
(KNOWN-ISSUES R20).

A false-accept measurement is `wake reset`, then hours of background sound
with nobody saying the wake phrase, then `wake`: every detection counted is
a false one.

## Running the server

From the repository root, once, to set up:

```sh
python -m venv server/.venv
server/.venv/Scripts/python -m pip install -r server/requirements-dev.txt
```

On Linux and macOS the interpreter is `server/.venv/bin/python`.

Copy `server/.env.example` to `server/.env` and fill it in. Three values
have no usable default:

| Setting | Meaning |
| --- | --- |
| `ANTHROPIC_API_KEY`, `DEEPGRAM_API_KEY` | The two service keys. See [SECRETS.md](SECRETS.md) for how to create them |
| `DEVICE_TOKENS` | One JSON object giving each device's identifier and its token, for example `{"hc-a1b2c3":"<token>"}`. A token is the device's password to the server: it stops anything else on the network from spending the keys or continuing a conversation. The file shows how to generate one |
| `TIMEZONE` | An IANA name such as `Europe/Berlin`. It decides when the day's conversation ends |

Then start it:

```sh
server/.venv/Scripts/python -m server
```

The server listens on port 8765 and announces itself on the local network,
so a device finds it without being told its address.

The server's tests need no keys and no network:

```sh
server/.venv/Scripts/python -m pytest
```

To try the server without the device, play it a recorded question with the
desktop client. The recording must be a 16 kHz, mono, 16-bit WAV file; keep
it, and the replies, outside the repository.

```sh
server/.venv/Scripts/python -m server.tools.desktop_client question.wav --out-dir replies
```

The client needs a `desktop-client` entry in `DEVICE_TOKENS`. It prints how
long each stage of the turn took and saves the reply as a WAV file. Several
files make several turns of one conversation, and `--discover` finds the
server by mDNS, as a device does.

## Repository layout

Directories are created as the corresponding stage begins.

```
firmware/          ESP-IDF application for the ESP32-S3
  main/            Startup and the serial console
  components/      app_config (settings), net (WiFi), audio (microphone and
                   amplifier), audio_ring (shared microphone audio),
                   wakeword (detection and its model), app_state (device
                   state), display (panel, backlight, animations), board
                   (pin map), server_link (server connection), player
                   (reply playback), state_machine (what the device does
                   next, and the request upload), protocol (the
                   protocol's numbers)
  tools/           Standalone hardware test programs
server/            Python server
  transport/       WebSocket endpoint, device authentication, connections
  pipeline/        One turn, from request to reply
  stt/, llm/, tts/ Speech-to-text, Claude, text-to-speech
  conversation/    The day's conversations, in SQLite
  tools/           Desktop client that stands in for the device
  tests/           Tests that need no keys or network
docs/              Protocol reference and supporting material
```

## Documentation

| Document | Audience | Contents |
| --- | --- | --- |
| [ARCHITECTURE.md](ARCHITECTURE.md) | All | System design, protocol, technology decisions |
| [STATUS.md](STATUS.md) | All | Current stage, completed work, next steps |
| [KNOWN-ISSUES.md](KNOWN-ISSUES.md) | All | Open risks, defects, caveats |
| [SECRETS.md](SECRETS.md) | All | Pre-commit checklist for sensitive material |
| [docs/PROTOCOL.md](docs/PROTOCOL.md) | All | Every message, rule and time limit between device and server |
| [docs/BRINGUP.md](docs/BRINGUP.md) | All | Breadboard assembly and electrical verification |
| [CLAUDE.md](CLAUDE.md) | AI assistants | Project rules and conventions |

## Privacy

Audio is captured only after the wake word fires, and wake-word detection runs
entirely on-device. Captured audio and its transcript are sent to third-party
speech and language services for processing. The server keeps no audio. It
keeps the day's conversation as text, and deletes it when the next day's
first exchange is stored. Recorded audio, transcripts and
conversation logs are treated as personal data and are excluded from version
control.
