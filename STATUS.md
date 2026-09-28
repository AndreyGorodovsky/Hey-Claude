# Status

**Current stage:** Stage 2 complete; stage 3 not yet planned.

**Last updated:** 2026-09-28

## Where the project stands

Feasibility has been reviewed and the approach confirmed. All major technology
decisions are made and recorded in [ARCHITECTURE.md](ARCHITECTURE.md).

The hardware is assembled on a breadboard and every peripheral has been
exercised by a test program, kept in `firmware/tools/bringup/`: the microphone captures, the display
initialises and fills, and the amplifier plays a tone. The pinout matched the
plan without change.

The firmware boots with all 8 MB of PSRAM, logs a boot banner including the
reset reason, stores its settings in NVS, and joins WiFi with automatic
reconnection. Settings are entered through a serial console. The microphone
records and the amplifier plays: a record-then-play loopback runs cleanly
from the console, and a full-volume tone ran without a brownout on a PC USB
port. The server exists only as a configuration skeleton.

## Stages

| # | Stage | State | Exit criteria |
| --- | --- | --- | --- |
| 0 | Hardware bring-up | Done, with a deviation (see below) | Circuit assembled on breadboard, checked for shorts, and powered peripheral by peripheral. Supply and cable confirmed adequate, baseline current recorded, pinout corrected to match what was built. Procedure in [docs/BRINGUP.md](docs/BRINGUP.md). |
| 1 | Foundations | Done | ESP-IDF project skeleton, NVS-backed configuration for WiFi, server URL and device identity, logging. Device boots, joins WiFi and logs. |
| 2 | Audio I/O bring-up | Done | INMP441 capture on I2S0 and MAX98357A playback on I2S1. A three-second record-then-play loopback runs cleanly. Also the first point at which peak current can be measured under amplifier load, completing the record started in stage 0. |
| 3 | Display and state machine | Not started | LVGL 9 driving the NV3007 panel, backlight under control, and the state animations driven by a mock state machine that cycles on a timer. No network. |
| 4 | Wake word | Not started | microWakeWord integrated with a continuous ring buffer; detection drives the state transition. Custom phrase trained and thresholds tuned against a multi-hour false-accept run. |
| 5 | Server v1 | Not started | WebSocket server, device authentication, day-scoped conversation store, and the speech-to-text, Claude and text-to-speech chain. Validated end to end by a desktop client script with no device involved. |
| 6 | Integration | Not started | Device WebSocket client, streaming upload during capture, server-driven endpointing, streaming playback, real state machine, and error, timeout and reconnect paths. First end-to-end conversation. |
| 7 | Latency and robustness | Not started | Measured end-to-end latency against the budget, per-sentence synthesis chunking, WiFi and server-drop recovery, watchdogs, brownout guard. |
| 8 | Polish | Not started | Animation refinement, volume control, provisioning experience, and over-the-air update if adopted. |

Stage 0 is hardware only and produces no code. Peripherals cannot be
functionally verified there, since that requires firmware; it establishes that
the wiring is correct and the supply is sound, so that later faults can be
attributed to software rather than to the bench.

Stage 0 closed without the baseline current and rail voltage figures, because
no meter was available. They were measured on 2026-09-28 with a USB power
meter: 0.10-0.14 A across every peripheral and WiFi, with the rail at 4.93 V
or above. Continuity checks remain undone for lack of a multimeter, and the
playback peak is still to be measured in stage 2 (R9 in
[KNOWN-ISSUES.md](KNOWN-ISSUES.md)). Results are in
[docs/BRINGUP.md](docs/BRINGUP.md).

Stage 5 does not depend on stages 0 through 4 and can be built in parallel.
Keeping it independently testable means a server defect cannot be mistaken for
a firmware defect during stage 6.

## Environment

ESP-IDF v5.5.5 is installed and verified by a successful `hello_world` build
targeting `esp32s3`. No other ESP-IDF version remains on the development
machine.

The server configuration skeleton exists ahead of stage 5, so that credentials
can be put in place as soon as they are obtained: `server/config.py` and
`server/.env.example`. Nothing else in the server is built.

## Next steps

1. Plan stage 3 into steps and submit them for approval.
2. Obtain API keys for Anthropic and Deepgram, copy `server/.env.example` to
   `server/.env`, and fill them in. See [SECRETS.md](SECRETS.md).
3. Obtain a multimeter for the continuity checks before the circuit is
   soldered (R9).

## Open decisions

| Decision | Status |
| --- | --- |
| Speech provider | Deepgram selected as the default for both directions, behind swappable adapters |
| Day rollover | 04:00 local time, settled |
| Endpointing | Server-side, settled |
| Deployment | LAN only for now; off-LAN deployment deferred and would require TLS |
| Over-the-air updates | Undecided, revisited at stage 8; the flash layout already allows it |
| Server discovery | mDNS on the LAN, with `server_url` as an override; built in stages 5 and 6 |
| Device settings entry | Serial console for now; stage 8 provisioning reuses the same validation |
