# Status

**Current stage:** Pre-stage 1 — documentation prepared, implementation not
started.

**Last updated:** 2026-09-23

## Where the project stands

Feasibility has been reviewed and the approach confirmed. All major technology
decisions are made and recorded in [ARCHITECTURE.md](ARCHITECTURE.md). No
firmware or server code exists yet. The repository currently contains
documentation only.

## Stages

| # | Stage | State | Exit criteria |
| --- | --- | --- | --- |
| 1 | Foundations | Not started | ESP-IDF project skeleton, NVS-backed configuration for WiFi, server URL and device identity, logging. Device boots, joins WiFi and logs. |
| 2 | Audio I/O bring-up | Not started | INMP441 capture on I2S0 and MAX98357A playback on I2S1. A three-second record-then-play loopback runs cleanly, validating wiring, supply rail and gain staging under real load. |
| 3 | Display and state machine | Not started | LVGL 9 driving the NV3007 panel, backlight under control, and the state animations driven by a mock state machine that cycles on a timer. No network. |
| 4 | Wake word | Not started | microWakeWord integrated with a continuous ring buffer; detection drives the state transition. Custom phrase trained and thresholds tuned against a multi-hour false-accept run. |
| 5 | Server v1 | Not started | WebSocket server, device authentication, day-scoped conversation store, and the speech-to-text, Claude and text-to-speech chain. Validated end to end by a desktop client script with no device involved. |
| 6 | Integration | Not started | Device WebSocket client, streaming upload during capture, server-driven endpointing, streaming playback, real state machine, and error, timeout and reconnect paths. First end-to-end conversation. |
| 7 | Latency and robustness | Not started | Measured end-to-end latency against the budget, per-sentence synthesis chunking, WiFi and server-drop recovery, watchdogs, brownout guard. |
| 8 | Polish | Not started | Animation refinement, volume control, provisioning experience, and over-the-air update if adopted. |

Stage 5 does not depend on stages 1 through 4 and can be built in parallel.
Keeping it independently testable means a server defect cannot be mistaken for
a firmware defect during stage 6.

## Environment

ESP-IDF v5.5.5 is installed and verified by a successful `hello_world` build
targeting `esp32s3`. No other ESP-IDF version remains on the development
machine.

## Next steps

1. Obtain API keys for Anthropic and the speech provider, and place them in the
   server environment. See [SECRETS.md](SECRETS.md).
2. Confirm the power supply meets the 1 A minimum described in
   [README.md](README.md).
3. Plan stage 1 into steps and submit them for approval.

## Open decisions

| Decision | Status |
| --- | --- |
| Speech provider | Deepgram selected as the default for both directions, behind swappable adapters |
| Day rollover | 04:00 local time, settled |
| Endpointing | Server-side, settled |
| Deployment | LAN only for now; off-LAN deployment deferred and would require TLS |
| Over-the-air updates | Undecided, revisited at stage 8 |
