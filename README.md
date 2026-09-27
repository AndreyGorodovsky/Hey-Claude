# Hey Claude

A self-contained voice assistant built on an ESP32-S3. Say the wake word, ask a
question, and hear Claude answer out loud. A small TFT panel shows what the
device is doing at each moment.

> **Project status: hardware assembled.** The circuit is built and verified on a
> breadboard. No project firmware or server code has been written yet. The
> documentation in this repository describes the intended design and the staged
> plan for building it. See [STATUS.md](STATUS.md) for the current stage.

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
2. After the wake word, captured audio streams to the server as it is spoken.
3. The server transcribes the audio and decides when the speaker has finished,
   then tells the device to stop capturing.
4. The transcript is appended to the day's conversation and sent to Claude.
5. Claude's reply is streamed, converted to speech sentence by sentence, and
   streamed back to the device, which plays it as it arrives.
6. The display runs a distinct animation for each state throughout.

Each request requires the wake word. Requests made on the same day continue the
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

| Signal | GPIO | Peripheral |
| --- | --- | --- |
| Mic SCK | 4 | I2S0 |
| Mic WS | 5 | I2S0 |
| Mic SD | 6 | I2S0 |
| Amp BCLK | 15 | I2S1 |
| Amp LRC | 16 | I2S1 |
| Amp DIN | 7 | I2S1 |
| Amp SD (enable) | 17 | GPIO |
| Display SCLK | 12 | SPI2 |
| Display MOSI | 11 | SPI2 |
| Display CS | 10 | SPI2 |
| Display DC | 9 | GPIO |
| Display RST | 8 | GPIO |
| Display backlight | 14 | LEDC PWM |

| Supply | Connection |
| --- | --- |
| INMP441 `VDD` | 3.3 V rail |
| NV3007 `VDD` | 3.3 V rail |
| MAX98357A `Vin` | Board 5 V pin, on a dedicated lead rather than a shared rail |
| 1000 µF capacitor | Across the amplifier's `Vin` and `GND` pins |
| All grounds | Common |

GPIO 26–37 are reserved by the flash and PSRAM and must not be used. GPIO 0, 3,
45 and 46 are strapping pins; GPIO 19 and 20 carry native USB. The INMP441
`L/R` pin is tied to ground to select the left channel. The MAX98357A `GAIN`
pin is left unconnected, which sets 9 dB.

## Software prerequisites

- **ESP-IDF v5.5.x** — see [CLAUDE.md](CLAUDE.md) for why this version is pinned.
- **Python 3.11+** for the server.
- Accounts and API keys for Anthropic and the configured speech provider. Keys
  live on the server only and are never present in firmware. See
  [SECRETS.md](SECRETS.md).

## Repository layout

Directories are created as the corresponding stage begins.

```
firmware/          ESP-IDF application for the ESP32-S3
firmware/tools/    Standalone hardware test programs
server/            Python server: transport, speech, Claude, session store
docs/              Diagrams and supporting material
```

## Documentation

| Document | Audience | Contents |
| --- | --- | --- |
| [ARCHITECTURE.md](ARCHITECTURE.md) | All | System design, protocol, technology decisions |
| [STATUS.md](STATUS.md) | All | Current stage, completed work, next steps |
| [KNOWN-ISSUES.md](KNOWN-ISSUES.md) | All | Open risks, defects, caveats |
| [SECRETS.md](SECRETS.md) | All | Pre-commit checklist for sensitive material |
| [docs/BRINGUP.md](docs/BRINGUP.md) | All | Breadboard assembly and electrical verification |
| [CLAUDE.md](CLAUDE.md) | AI assistants | Project rules and conventions |

## Privacy

Audio is captured only after the wake word fires, and wake-word detection runs
entirely on-device. Captured audio and its transcript are sent to third-party
speech and language services for processing. Conversation history is retained
on the server for the duration of the day. Recorded audio, transcripts and
conversation logs are treated as personal data and are excluded from version
control.
