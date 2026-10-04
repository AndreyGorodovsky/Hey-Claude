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

*Stock model results.* Observed on 2026-10-01 with the stock "Hey Jarvis"
model at its own cutoff of 0.97, on the breadboard in a room with ordinary
background sound, one speaker. These are spot checks, not the tuning run.

- At 0.5, 1 and 2 m, nearly every try was detected, with averaged scores
  of about 0.98.
- At 3-4 m, about half were detected, and only when spoken loudly. The
  misses scored about 0.7.
- Speech with a strong Russian or Hebrew accent was often missed, while
  the same speaker with a more English accent was detected reliably.
  microWakeWord models are trained on synthetic voices; the custom model's
  training should include varied voices and accents, and possibly the
  user's own recordings, which stay out of the repository.

The targets for the custom model, agreed for stage 4: at most 0.5 false
detections per hour against continuous speech-heavy background sound (TV,
podcasts), and at least 90 % detection at 1 m in a quiet room.

*Trained "Hey Claude" models.* Not yet established. Two runs trained on
synthetic voices only fell well short on the device, and the notebook's
test proved a poor predictor of the device. Run 3, with recordings of one
real speaker, is the first to work on the device: on 2026-10-04 that
speaker was detected in nearly every try at 0.5, 1 and 2 m. Four things
remain open.

- The detection target is not established. The device test was 7 tries
  at each distance, by the speaker the model was trained on, with the
  room conditions not noted: too few to show 90 %.
- The false-detection target has not been measured on the device. Stage 4
  closed without the multi-hour false-accept run, so the cutoff is the
  training notebook's starting value, not a tuned one. The run is
  recommended before any release, and after any further training run,
  since it is made per model.
- The model is specialised to the recorded voice. It misses far more of
  the notebook's synthetic voices than run 2 did, so other speakers
  should expect more misses until they are recorded for a further run.
- "Hey cloud" can trigger, and did once in the device test. It scores
  above any usable cutoff in the real-voice test, so the remedy is more
  recordings of it in a further run, not a higher cutoff.

Each run's results are recorded in one place, the run history in
[docs/WAKEWORD-TRAINING.md](docs/WAKEWORD-TRAINING.md).

### R2 — Brownout under amplifier load

Peak draw is roughly 1.4 A at 5 V when amplifier transients into the 4 Ω load
coincide with WiFi transmit bursts. A 500 mA USB port will reset the board
during loud playback. The failure presents as a reboot mid-sentence and is
easily misattributed to firmware.

*Mitigation:* 1 A or greater supply; 1000 µF at the amplifier supply pin;
amplifier fed from 5 V, not 3.3 V. If resets persist, the supply is the first
suspect, not the code. Baseline current and the peak under load are recorded
before and during stage 2, per [docs/BRINGUP.md](docs/BRINGUP.md). The
baseline is 0.10-0.14 A with the rail at 4.93 V or above. Measured in stage 2
with a full-scale tone and WiFi connected: 0.556 A average at 4.831 V, with no
reset, on a PC USB 3.0 port.

A breadboard compounds this. Contact resistance and shared rails not intended
for 1 A transients can themselves cause the voltage drop, making the bench the
fault rather than the supply. Peak figures taken on a breadboard should be
re-measured once the circuit is soldered before concluding that the supply is
inadequate.

### R3 — Display refresh rate ceiling

A full-screen refresh moves 121,552 bytes. Measured on 2026-09-29 with
`display test`: 12.5 ms at 80 MHz, the highest SPI clock the chip supports,
so about 80 full frames per second at the very most, before any drawing
time. Full-screen animation at high frame rates is not achievable.

*Mitigation:* animations are designed as partial-region redraws from the start.
The stage 3 animations change a 142 x 142 region, about 4 ms per frame. This
constrains the visual design and is not a limitation that can be optimised
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

### R9 — Playback peak known only as an average, continuity not checked

The first bring-up was carried out without a multimeter or a USB power meter.
The figures were taken afterwards with a USB power meter and are recorded in
[docs/BRINGUP.md](docs/BRINGUP.md). Two gaps remain.

- **The true playback peak described in R2 is unmeasured.** The full-scale
  tone test in stage 2 read 0.556 A at 4.831 V, but the meter shows averages
  refreshed a few times per second, so it cannot catch a transient of about
  10 ms, and it reads voltage before the cable rather than at the board. The
  board did not reset, which is the decisive result for this supply.
- **No continuity or short checks were made**, since they need a multimeter.
  The working peripherals show the connections are sound, but not that there
  is no marginal contact.

*Mitigation:* the brownout reset reason in the boot log remains the decisive
test: a reset during loud playback is attributed to the supply first. Repeat
`audio tone 100` after soldering and after any change of supply or cable.
Obtain a multimeter for the continuity checks before the circuit is
soldered.

### R10 — Device credentials stored unencrypted

The WiFi password and the device token are stored in NVS in plain text.
Anyone with physical access to the device and a USB cable can read them out
of flash.

*Mitigation:* accepted for LAN-only development, alongside R8. NVS encryption,
or flash encryption, is to be adopted together with provisioning and OTA in
stage 8.

### R12 — Speech arrives quietly from the microphone

Speech at 0.5-1 m measured about -52 dBFS RMS on 2026-09-28, against a
quiet-room floor of about -67 dBFS: roughly 15 dB above the noise, and more
than 45 dB below full scale. The firmware keeps the top 16 of the
microphone's 24 bits, which suits loud sound and leaves ordinary speech small.
Speech-to-text services and wake-word models normalise level to some degree,
so it is not yet known whether this matters.

*Mitigation:* the shift is one named constant, `CAPTURE_SHIFT` in
`firmware/components/audio/audio.c`. Its value is decided against real
results: wake-word detection in stage 4 and transcripts in stage 6. A smaller
shift adds gain in 6 dB steps and must clip rather than wrap.

Wake-word detection does not need more gain: the stock model detected
reliably up to 2 m with the shift at 16 (2026-10-01), and its feature
frontend evens out loudness itself. The value stays at 16 until stage 6
shows whether transcripts need more.

### R13 — The display tears during fast movement

The panel has no tearing-effect (TE) output, so the firmware cannot tell
when the panel is between refreshes of its glass. When a frame arrives
while the panel is part-way through showing the previous one, the picture
briefly shows the top of one frame and the bottom of the other. Observed on
2026-09-29: the `display test` bar, which moves across the full height of
the screen, shows a step. The state animations, which are small and redraw
in about 4 ms, showed no visible tearing.

*Mitigation:* keep animations small and avoid fast horizontal movement of
tall shapes. The 80 MHz clock keeps each transfer short, which makes tearing
smaller and rarer but cannot remove it.

### R15 — Microphone overflow is not counted

The ring buffer counts audio a listener missed by falling more than 2 s
behind, and the `wake` command reports it. It cannot see a loss one step
earlier: if the capture task were ever more than 75 ms late reading the
microphone, the I2S driver would drop the oldest audio without any count.
The "no audio lost" results in this repository therefore cover the ring,
not the whole path from the microphone.

*Mitigation:* the capture task runs at the highest audio priority on core 1
and holds the ring's lock only briefly, so a 75 ms delay is not expected.
If audio skips are ever heard, the driver's receive-overflow callback
(`on_recv_q_ovf`) can count such losses.

### R16 — Limits of the training notebook

Version 1 of the training notebook was built to get one run through
Colab's free plan, and reviews after run 2 found it unsafe to rerun with
changed inputs. Version 2 (`hey_claude_v2.ipynb`) fixes that: steps are
skipped only when a marker of their settings matches, the training
settings are written by the training cell itself, a run cannot be
continued with changed settings, each run records its settings, model
shape and package versions, sections are named rather than numbered, and
the manifest is generated. These limits remain:

- Only microWakeWord, the sample generator and two audio libraries are
  pinned; the rest comes from whatever Colab provides. Each run records
  the full list, from which a later run can pin.
- After a disconnect, training resumes from the last weights but restarts
  its step count and its record of the best checkpoint. This is inside
  microWakeWord.
- Checkpoints are chosen by recall on synthetic samples, which run 2
  showed does not predict the device. The test on real voices reports on
  the chosen model but does not take part in choosing it, which would need
  a further set of recordings beyond the test set.
- The test on real voices mirrors the device's arithmetic and decision,
  but its feature front end, like training's, is microWakeWord's own
  build of the same TensorFlow Lite Micro code, not the firmware's.
  The device test remains the final word.
- Speech in a recording is found by level alone: sound at least `above_db`
  over the recording's quietest tenth, for at least 0.3 s. A recording
  with a loud background can be dropped as holding no speech, and the same
  rule sets where each kept phrase is trimmed. Measured on run 3's
  recordings (185 single-phrase files, 16 kHz mono): at the original
  12 dB, 6 of the 100 wake-phrase training recordings were dropped, 5 of
  them among the 14 made with a kitchen running, where the voice peaked
  about 13 dB over the background; at 9 dB none were, and `above_db` is
  now 9. Recordings made in louder surroundings may still be dropped. The
  value is part of the test set's identity, so runs compared on real
  voices must be scored with the same one. The Recordings section's count
  of phrases per file shows any that are lost, but as one line among
  many; a closing count of files with no speech would be harder to miss.
- The real-voice test is a loose guide to the device. For run 2 it gave
  8 of 10 at 2 m where the device gave 1 of 10; the test recordings are
  not made with the device's microphone. Recordings from the device
  itself would close the gap. The streaming upload of stage 6 could
  supply them, which would need its own decision on where such audio is
  kept.
- The real-voice test may score each step one count low. It rounds the
  model's output, as returned by microWakeWord, the way the firmware
  rounds the cutoff; if that output is the raw value divided by 256, as
  the model files suggest, a raw 213 becomes 212. Not confirmed by
  running it. The error is at most 0.4 % of a score, and no recording of
  run 2's or run 3's tests is that close to its cutoff.
- The test set's fingerprint is taken from every converted test folder on
  the session's disk. A test folder removed or renamed in Drive during a
  session still counts until the session is restarted, so the same test
  set can show two fingerprints.
- The recordings' `share` adds to the synthetic examples instead of
  replacing part of them: with a share of 0.2, wake phrases are drawn 1.25
  times as often as without recordings, and sound-alikes likewise, so a
  run with recordings differs from one without in balance as well as in
  data. Recordings are also drawn per clip from all speakers together, so
  a second speaker with fewer recordings gets a smaller part of the share.
- The manifest is written by the notebook, and its cutoff is then meant
  to be adjusted by hand after tuning. Nothing in the file says which
  value it holds, and downloading the results again replaces a tuned
  value. Likewise, Keep results records whatever Settings holds when it
  is run, not what the run was trained with.
- The code that accepts version 1's samples has done its work and can be
  removed.

*Mitigation:* the device test decides whether a run is better. The
real-voice test compares runs on the same test set, and the notebook's
synthetic test does neither. The notebook is kept as it trained run 3;
the items above that change it are for the start of a further run.

## Resolved

- **R4 — Provisional pinout unvalidated.** Built on the breadboard and each
  peripheral exercised by a test program during stage 0. The pinout matched
  the plan without change. See [docs/BRINGUP.md](docs/BRINGUP.md).
- **R5 — Display power requirements unconfirmed.** The module works with
  `VDD` at 3.3 V, and its backlight is switched by an on-board transistor, so
  `BL` is driven directly from a GPIO. The 8-pin order is `GND VDD SCL SDA RES
  DC CS BL`, as printed on the module.
- **R11 — Backlight pin left floating.** From stage 3, the firmware drives
  the backlight pin (GPIO14) low within about a second of reset, in
  `display_init()`, and lights it only after the first frame is drawn. It
  still floats between reset and that point, as the amplifier's `SD` pin
  does before `audio_init()`.
- **R14 — LVGL's version not locked.** From stage 4,
  `firmware/dependencies.lock` is committed, fixing the exact version of
  every component fetched from the registry: LVGL 9.6.0, and the wake-word
  libraries, which are also pinned exactly in their manifest because the
  tuned cutoff depends on them. Updating any of them is now a deliberate
  change (`idf.py update-dependencies`), followed by retesting.

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
- **The microphone is not usable for about 2 s after boot.** It starts in
  `audio_init()` and its output settles over that time.
- **`audio loop` playback includes audible hiss.** The test boosts quiet
  recordings by up to 36 dB so that speech can be heard, which raises the
  microphone's own noise with it. The recorded audio itself is not boosted.
- **Wake-word detections show only from `IDLE`.** Until the server
  connection exists the device does not reach `IDLE` by itself, so it is
  set with `state set idle` before testing. The stand-in that reacts to
  detections is replaced in stage 6. It reads the state and then changes
  it, so a `state set` typed at the same instant can be overwritten, and it
  changes the state from inside the event loop that announces the change.
  Both are harmless for a test aid and are not to be copied into the state
  machine.
- **The `state` console commands set the state directly.** They stand in for
  the state machine built in stage 6 and would conflict with it; stage 6
  replaces them.
- **The screen is dark for about two seconds after reset.** The panel's
  start-up sequence has required pauses of about a second, and the
  backlight is lit only once the first frame is on the panel.
- **About one second passes between the boot banner and WiFi start.** Observed
  on every boot; the cause has not been investigated. It is a boot-time cost,
  not a latency on requests.
