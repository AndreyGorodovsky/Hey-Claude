# Status

**Current stage:** Stage 4, wake word. Part A (the detection pipeline, on a
stock model) is complete. Part B (training and tuning the project's own
phrase, "Hey Claude") is in progress: the second training run is done and
its model is in the firmware but falls short on the device; recordings
of real voices are next.

**Last updated:** 2026-10-02

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
port.

The display runs in landscape and shows the device state with a placeholder
animation for each of the eight states, including a `SETUP` state for
missing WiFi settings, added in stage 3. The backlight stays dark until the
first frame is drawn, then fades in, and dims while idle. States are set
from the console until the real state machine exists. Measured on the
breadboard: the panel runs cleanly at 80 MHz and a full frame takes 12.5 ms.
A ten-minute run through every state showed no faults, and audio loopback
and tone playback sounded as in stage 2 while the display animated, at full
and at dimmed brightness.

Wake-word detection runs on the device from boot, added in stage 4. A
capture task is the microphone's only reader and keeps the last 2 s of
audio in a ring buffer, from which every listener reads at its own
position. A microWakeWord model runs on that audio every 30 ms and
announces each detection; until the stage 6
state machine exists, a detection in `IDLE` shows `CAPTURING` for 3 s. The
`wake` console command shows detections, scores and timing, and changes
the cutoff for tuning. Measured on the breadboard: a run of the model takes
2.2 ms on average, about 8 % of core 1; detection was reliable up to 2 m
and partial at 3-4 m; playback and the loopback test were unaffected. The
review agents defined in `.claude/agents/` reviewed part A before it was
committed. Those figures are for the stock "Hey Jarvis" model.

Part B so far. "Hey Claude" models are trained in Google Colab with a
notebook in `firmware/tools/wakeword_training/`, described in
[docs/WAKEWORD-TRAINING.md](docs/WAKEWORD-TRAINING.md), whose run history
is the one record of each run's results. Two runs on synthetic voices only
fall well short of the targets on the device. The firmware builds the
second, `hey_claude_run2`, and the boot log and `wake` command name the
model, so each device test can be tied to its run. Version 2 of the
notebook, for run 3, adds recordings of real voices, made following
[docs/WAKEWORD-RECORDING.md](docs/WAKEWORD-RECORDING.md), and a test on a
separate set of them that is never trained on. It is also safe to rerun
with changed inputs, and generates the firmware manifest; its remaining
limits are in KNOWN-ISSUES R16. Its helpers, the recordings step, the
feature building, the training settings and refusals, the sample
migration from version 1 and the real-voice test were run locally against
stand-ins for Colab and Google Drive, with the run 2 model and Windows
text-to-speech clips; the sample generation, downloads and training run
only in Colab. In that local test the run 2 model scored the
text-to-speech "hey cloud" and "okay Claude" above 0.95, which agrees with
the device test.

The server exists only as a configuration skeleton.

## Stages

| # | Stage | State | Exit criteria |
| --- | --- | --- | --- |
| 0 | Hardware bring-up | Done, with a deviation (see below) | Circuit assembled on breadboard, checked for shorts, and powered peripheral by peripheral. Supply and cable confirmed adequate, baseline current recorded, pinout corrected to match what was built. Procedure in [docs/BRINGUP.md](docs/BRINGUP.md). |
| 1 | Foundations | Done | ESP-IDF project skeleton, NVS-backed configuration for WiFi, server URL and device identity, logging. Device boots, joins WiFi and logs. |
| 2 | Audio I/O bring-up | Done | INMP441 capture on I2S0 and MAX98357A playback on I2S1. A three-second record-then-play loopback runs cleanly. Also the first point at which peak current can be measured under amplifier load, completing the record started in stage 0. |
| 3 | Display and state machine | Done | LVGL 9 driving the NV3007 panel, backlight under control, and the state animations driven by a mock state machine that cycles on a timer. No network. |
| 4 | Wake word | Part A done; part B in progress (run 3, with real recordings, next) | microWakeWord integrated with a continuous ring buffer; detection drives the state transition. Custom phrase trained and thresholds tuned against a multi-hour false-accept run. |
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

Stage 4 is split in two, as KNOWN-ISSUES R1 advises. Part A built the
pipeline on a stock model, so that later stages are not blocked on
wake-word quality. Part B trains a model for the project's own two-syllable
phrase in Google Colab, then tunes its cutoff against the targets in R1: at
most 0.5 false detections per hour and at least 90 % detection at 1 m. If a
two-syllable phrase cannot meet them, a longer phrase is tried. Each part is
reviewed and committed separately.

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

1. Record real voices following
   [docs/WAKEWORD-RECORDING.md](docs/WAKEWORD-RECORDING.md), including
   sound-alikes such as "hey cloud" and a separate test session. Score
   run 2 on the test set as the baseline, then train run 3 with
   `hey_claude_v2.ipynb`, both as the training document describes. Then
   repeat the device test:
   10 tries each at 0.5, 1 and 2 m, plus "Claude" alone and sound-alikes,
   which must not trigger. If a two-syllable phrase still falls short
   after that, consider a longer phrase.
2. Once detection meets the target, tune the cutoff with the multi-hour
   false-accept run (step 9 of the stage 4 plan), then review, update the
   documents and commit part B (step 10).
3. Obtain API keys for Anthropic and Deepgram, copy `server/.env.example` to
   `server/.env`, and fill them in. See [SECRETS.md](SECRETS.md).
4. Obtain a multimeter for the continuity checks before the circuit is
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
| Locking dependency versions | Settled: `firmware/dependencies.lock` is committed, and the wake-word libraries are pinned exactly (R14) |
| Wake phrase | A two-syllable phrase first; a longer one if it cannot meet the R1 targets |
| `SETUP` and `ERROR` detail text | Callers pass display wording for now; before stage 6 adds server errors, decide whether to pass a reason code that the display turns into words |
