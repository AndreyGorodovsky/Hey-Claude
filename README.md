# Hey Claude

A voice assistant built on an ESP32-S3. Say "Hey Claude", ask a question,
and hear Claude answer out loud. A small panel shows what the device is
doing.

**Status:** working end to end on a breadboard. The wake word is detected
on the device, the request streams to a server on the local network, and
Claude's answer is spoken, drawing on a web search when it needs one. The
volume is set by voice, a button interrupts a reply, a switch turns on
continuous conversation, and settings can be entered from a phone. The
wake-word model is trained on one speaker's voice; its false detections
are not yet measured. More in [STATUS.md](STATUS.md).

**To build one,** follow [docs/GETTING-STARTED.md](docs/GETTING-STARTED.md).

## How it works

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

1. The device listens for the wake word on its own. No audio leaves it
   until the wake word fires.
2. It plays two short notes, then streams what is said to the server.
3. The server transcribes the audio, decides when the speaker has
   finished, and tells the device to stop listening.
4. The request joins the day's conversation and goes to Claude, through
   the Claude Code tool, which can search the web.
5. Claude's reply is turned into speech sentence by sentence and streamed
   back; the device plays it as it arrives.
6. The display shows each state with its own animation throughout.

Requests on the same day continue one conversation, so follow-ups keep
their context; it resets at 04:00 local time. The button interrupts a
reply, and with the switch closed the device listens again after each
reply without the wake word.

## Hardware

| Part | Role |
| --- | --- |
| ESP32-S3 "N16R8" dev board | 16 MB flash, 8 MB octal PSRAM, dual-core 240 MHz |
| INMP441 | I2S MEMS microphone |
| MAX98357A | I2S Class-D amplifier, 3 W |
| 4 Ω speaker | Output |
| NV3007 2.79" TFT, 142x428, SPI | Status display |
| 1000 µF / 10 V electrolytic | Amplifier supply decoupling |
| Momentary push button, three-leg slide switch | Interrupting a reply; continuous conversation |

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

Verified on the breadboard by a test of each peripheral; how to build and
check the circuit is in [docs/BRINGUP.md](docs/BRINGUP.md).

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

**Button and switch**, on a breadboard of their own whose
ground rail is joined to the main one. Each closes its pin to ground; the
chip's internal pull-up holds the pin high while open, so no resistor is
fitted.

| Part | Leg | Connection | Purpose |
| --- | --- | --- | --- |
| Momentary push button, two legs | One leg | GPIO 1 | Pressed, interrupts a turn |
| | Other leg | Ground rail | |
| Three-leg slide switch | Middle leg | Ground rail | The common |
| | One outer leg | GPIO 2 | Toward this leg, closed: continuous conversation |
| | Other outer leg | Not connected | |

GPIO 26–37 are reserved by the flash and PSRAM and must not be used. GPIO 0, 3,
45 and 46 are strapping pins; GPIO 19 and 20 carry native USB. The board's own
BOOT button, on GPIO 0, is read by the firmware once it is running, to open
setup mode; held while the board is reset or powered on, it still makes the
chip wait to be flashed.

## Repository layout

```
firmware/          ESP-IDF application for the ESP32-S3
  main/            Startup and the serial console
  components/      app_config (settings), net (WiFi), setup (setup mode),
                   audio (microphone and amplifier), audio_ring (shared
                   microphone audio), wakeword (detection and its model),
                   app_state (device state), state_machine (what the device
                   does next, and the request upload), server_link (server
                   connection), player (reply playback), protocol (the
                   protocol's numbers), display (panel and animations),
                   controls (button and switch), board (pin map)
  tools/           Hardware test programs and the wake-word training notebook
server/            Python server
  transport/       WebSocket endpoint, device authentication, connections
  pipeline/        One turn, from request to reply
  stt/, llm/, tts/ Speech-to-text, Claude, text-to-speech
  conversation/    The day's conversations, in SQLite
  tools/           Desktop client and the Claude Code bench
  tests/           Tests that need no keys or network
docs/              Guides and the protocol reference
```

## Documentation

| Document | Contents |
| --- | --- |
| [docs/GETTING-STARTED.md](docs/GETTING-STARTED.md) | From the parts to the first spoken answer |
| [docs/OPERATING.md](docs/OPERATING.md) | Flashing, settings, console commands, running the server, updating Claude Code |
| [docs/BRINGUP.md](docs/BRINGUP.md) | Building the circuit and checking it, peripheral by peripheral |
| [docs/WAKEWORD-RECORDING.md](docs/WAKEWORD-RECORDING.md) | Recording voices for wake-word training |
| [docs/WAKEWORD-TRAINING.md](docs/WAKEWORD-TRAINING.md) | Training a wake-word model, and each run's results |
| [ARCHITECTURE.md](ARCHITECTURE.md) | How the system is designed, and why |
| [docs/PROTOCOL.md](docs/PROTOCOL.md) | Every message, rule and time limit between device and server |
| [STATUS.md](STATUS.md) | Where the project stands, measured results, next steps, decisions |
| [KNOWN-ISSUES.md](KNOWN-ISSUES.md) | Open risks and caveats |
| [SECRETS.md](SECRETS.md) | What must never be committed, and the pre-commit checklist |
| [CLAUDE.md](CLAUDE.md) | Rules for AI assistants working on the repository |

## Privacy

Audio is captured only after the wake word fires, and wake-word detection runs
entirely on-device. Captured audio and its transcript are sent to third-party
speech and language services for processing. The server keeps no audio. It
keeps the day's conversation as text, and deletes it when the next day's
first exchange is stored. Recorded audio, transcripts and
conversation logs are treated as personal data and are excluded from version
control.
