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
suspect, not the code. Baseline current is recorded in stage 0 and the peak
under load in stage 2, per [docs/BRINGUP.md](docs/BRINGUP.md).

A breadboard compounds this. Contact resistance and shared rails not intended
for 1 A transients can themselves cause the voltage drop, making the bench the
fault rather than the supply. Peak figures taken on a breadboard should be
re-measured once the circuit is soldered before concluding that the supply is
inadequate.

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

*Mitigation:* verified during stage 0 against the physical modules, and the
table in [README.md](README.md) updated to match what was actually built.

### R5 — Display module power requirements unconfirmed

Small NV3007 panels frequently require a backlight boost rail rather than
direct 3.3 V drive, and the eight-pin breakout's exact pin order is not
confirmed. The panel is write-only over SPI, so a wiring fault produces a blank
screen with no error rather than a diagnosable failure.

*Mitigation:* confirm against the supplied module before powering it, as the
first step of stage 0. See [docs/BRINGUP.md](docs/BRINGUP.md).

### R6 — Three external services in the latency path

Speech-to-text, Claude and text-to-speech each contribute latency and each can
fail independently. The budget in [ARCHITECTURE.md](ARCHITECTURE.md) assumes
all three are healthy and reachable over the local network's uplink.

*Mitigation:* adapter interfaces on both speech services allow substitution
without touching the pipeline. Partial failures surface as an error state on
the device rather than a hang.

### R7 — Stale ESP-IDF environment variables

Installing or removing an ESP-IDF version leaves persisted user environment
variables behind, and they survive to break the next install. Observed in
practice: a removed installation had written `IDF_PYTHON_ENV_PATH` into the
user profile, which caused a later `install.bat` for a different version to
abort with a version-mismatch error, and had added its own Python directories
to the user `PATH`, which remained after the installation was deleted.

The failures present as problems with the new version rather than as residue
from the old one.

*Mitigation:* when changing ESP-IDF versions, check `IDF_PATH`,
`IDF_TOOLS_PATH` and `IDF_PYTHON_ENV_PATH` in the persisted user environment,
not only in the current shell, and run installers from a shell with no ESP-IDF
environment active. Remove orphaned toolchains with `idf_tools.py uninstall`
rather than by deleting directories, since tool versions are shared between
installations.

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
