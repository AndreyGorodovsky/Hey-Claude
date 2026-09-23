# Known issues

Open risks, defects and caveats. Risks identified during design are listed
before any code exists so that they are tested for rather than discovered.

**Confirmed defects:** none. No code has been written.

## Open risks

### R1 — Wake-word accuracy is the largest unknown

A custom wake phrase trained with microWakeWord from synthetic speech will not
match the accuracy of a commercial assistant trained on real recordings.
Expect a tuning cycle trading false accepts against false rejects, and expect
the balance to shift with room acoustics and speaker distance.

Espressif's own engine cannot supply a custom phrase without a paid corpus of
more than 500 speakers, so there is no drop-in alternative.

*Mitigation:* bring the pipeline up on a stock model first so that stages 5 and
6 are not blocked on wake-word quality, then substitute the trained model.
Tuning is measured over a multi-hour false-accept run, not by informal trials.

### R2 — Brownout under amplifier load

Peak draw is roughly 1.4 A at 5 V when amplifier transients into the 4 Ω load
coincide with WiFi transmit bursts. A 500 mA USB port will reset the board
during loud playback. The failure presents as a reboot mid-sentence and is
easily misattributed to firmware.

*Mitigation:* 1 A or greater supply; 1000 µF at the amplifier supply pin;
amplifier fed from 5 V, not 3.3 V. If resets persist, the supply is the first
suspect, not the code.

### R3 — Display refresh rate ceiling

A full-screen refresh moves about 121 KB and takes roughly 12-25 ms depending
on SPI clock. Full-screen animation at high frame rates is not achievable.

*Mitigation:* animations are designed as partial-region redraws from the start.
This constrains the visual design and is not a limitation that can be optimised
away later.

### R4 — Provisional pinout is unvalidated

The pin assignment in [README.md](README.md) avoids the octal flash and PSRAM
pins, the strapping pins and the native USB pins, but has not been checked
against a physical board. Dev board silkscreens vary between vendors.

*Mitigation:* verified during stages 2 and 3, and the table updated with
measured results.

### R5 — Display module power requirements unconfirmed

Small NV3007 panels frequently require a backlight boost rail rather than
direct 3.3 V drive, and the eight-pin breakout's exact pin order is not
confirmed. The panel is write-only over SPI, so a wiring fault produces a blank
screen with no error rather than a diagnosable failure.

*Mitigation:* confirm against the supplied module before powering it.

### R6 — Three external services in the latency path

Speech-to-text, Claude and text-to-speech each contribute latency and each can
fail independently. The budget in [ARCHITECTURE.md](ARCHITECTURE.md) assumes
all three are healthy and reachable over the local network's uplink.

*Mitigation:* adapter interfaces on both speech services allow substitution
without touching the pipeline. Partial failures surface as an error state on
the device rather than a hang.

### R7 — Toolchain version drift

The original development machine carries two ESP-IDF checkouts, a
`release/v5.3` branch and a `v6.1-beta1`, and neither is the pinned v5.5.x.
Building against the wrong one produces failures that look like code defects,
particularly around the I2S and LCD drivers.

*Mitigation:* confirm `IDF_PATH` before building. Documented in
[CLAUDE.md](CLAUDE.md).

### R8 — Unencrypted transport on the LAN

Device tokens are shared secrets sent over plain WebSocket. Anything on the
local network can read conversation audio in both directions.

*Mitigation:* accepted for LAN-only development. Moving the server off the LAN
requires TLS and a stronger device credential before it is reachable publicly.

## Caveats

- **Wake word required for every request.** The device does not remain open for
  follow-up questions. This is intended behaviour, not a defect.
- **Conversation resets at 04:00 local time.** A conversation that spans the
  rollover loses its earlier context. The hour was chosen to make this rare.
- **Captured audio and transcripts are personal data.** They are sent to
  third-party services for processing and are excluded from version control.
- **Wake-word detection runs entirely on-device.** No audio is transmitted
  before the wake word fires.
