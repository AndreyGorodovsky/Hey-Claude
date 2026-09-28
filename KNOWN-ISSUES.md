# Known issues

Open risks, defects and caveats. Risks identified during design are listed
before any code exists so that they are tested for rather than discovered.

**Confirmed defects:** none.

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
suspect, not the code. Baseline current and the peak under load are recorded
before and during stage 2, per [docs/BRINGUP.md](docs/BRINGUP.md). The
baseline is recorded: 0.10-0.14 A with the rail at 4.93 V or above, measured
on a PC USB 3.0 port.

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

A related trap: shells from the MSYS family, such as Git Bash, set `MSYSTEM`,
and ESP-IDF refuses to activate when it is present, with the error "MSys/Mingw
is not supported". This includes a PowerShell started from inside Git Bash,
which inherits the variable.

*Mitigation:* when changing ESP-IDF versions, check `IDF_PATH`,
`IDF_TOOLS_PATH` and `IDF_PYTHON_ENV_PATH` in the persisted user environment,
not only in the current shell, and run installers from a shell with no ESP-IDF
environment active. Remove orphaned toolchains with `idf_tools.py uninstall`
rather than by deleting directories, since tool versions are shared between
installations. Run ESP-IDF from a PowerShell that was not started from an
MSYS shell.

### R8 — Unencrypted transport on the LAN

Device tokens are shared secrets sent over plain WebSocket. Anything on the
local network can read conversation audio in both directions.

*Mitigation:* accepted for LAN-only development. Moving the server off the LAN
requires TLS and a stronger device credential before it is reachable publicly.

### R9 — Playback peak not measured, continuity not checked

The first bring-up was carried out without a multimeter or a USB power meter.
The baseline figures were taken afterwards with a USB power meter and are
recorded in [docs/BRINGUP.md](docs/BRINGUP.md): the rail held 4.93 V or above
at up to 0.14 A. Two gaps remain.

- **The playback peak described in R2 is unmeasured.** The meter shows
  averages refreshed a few times per second, so it cannot catch a transient of
  about 10 ms, and it reads voltage before the cable rather than at the board.
- **No continuity or short checks were made**, since they need a multimeter.
  The working peripherals show the connections are sound, but not that there
  is no marginal contact.

*Mitigation:* take the stage 2 peak measurement as planned, and treat a
reading from this meter as a lower bound on the peak. The brownout reset
reason in the boot log remains the decisive test: a reset during loud playback
is attributed to the supply first. Obtain a multimeter for the continuity
checks before the circuit is soldered.

### R10 — Device credentials stored unencrypted

The WiFi password and the device token are stored in NVS in plain text.
Anyone with physical access to the device and a USB cable can read them out
of flash.

*Mitigation:* accepted for LAN-only development, alongside R8. NVS encryption,
or flash encryption, is to be adopted together with provisioning and OTA in
stage 8.

### R11 — Backlight and amplifier enable pins left floating

Neither the stage 1 firmware nor the `mic_test` and `amp_test` programs
configure the display's backlight pin (GPIO14), and the stage 1 firmware does
not configure the amplifier's `SD` pin either. A pin no code drives is
"floating": it settles high or low unpredictably. Observed on 2026-09-28: the
backlight was off under `mic_test` and on under `amp_test` and the stage 1
firmware. A floating `SD` can likewise leave the amplifier enabled and drawing
current, or picking up noise, with no audio playing.

*Mitigation:* the firmware drives both pins to a defined state at boot:
amplifier off until audio plays, from stage 2, and backlight under explicit
control from stage 3. Fixing it earlier is outside the current stage.

## Resolved

- **R4 — Provisional pinout unvalidated.** Built on the breadboard and each
  peripheral exercised by a test program during stage 0. The pinout matched
  the plan without change. See [docs/BRINGUP.md](docs/BRINGUP.md).
- **R5 — Display power requirements unconfirmed.** The module works with
  `VDD` at 3.3 V, and its backlight is switched by an on-board transistor, so
  `BL` is driven directly from a GPIO. The 8-pin order is `GND VDD SCL SDA RES
  DC CS BL`, as printed on the module.

## Caveats

- **Wake word required for every request.** The device does not remain open for
  follow-up questions. This is intended behaviour, not a defect.
- **Conversation resets at 04:00 local time.** A conversation that spans the
  rollover loses its earlier context. The hour was chosen to make this rare.
- **Captured audio and transcripts are personal data.** They are sent to
  third-party services for processing and are excluded from version control.
- **Wake-word detection runs entirely on-device.** No audio is transmitted
  before the wake word fires.
- **The console echoes what is typed.** A password entered with `config set`
  is visible on screen as it is typed, and in any terminal scrollback or log
  capture. It is not kept in the console's own line history.
- **Settings apply after a reboot.** `config set` changes what is stored,
  not what is running.
- **The console accepts input only on the UART port.** The native USB port
  shows the log but ignores keystrokes.
- **About one second passes between the boot banner and WiFi start.** Observed
  on every boot; the cause has not been investigated. It is a boot-time cost,
  not a latency on requests.
