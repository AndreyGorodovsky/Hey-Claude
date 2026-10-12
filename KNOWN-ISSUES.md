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
reset, on a PC USB 3.0 port. Seen in stage 6, on 2026-10-07, on the
breadboard and USB-powered: a spoken reply of 16.4 s played at the full
level while the device stayed connected, with no reset. Measured the same
day with the USB power meter, on the breadboard, on WiFi: a full-scale
tone for 10 s read about 0.582 A at 4.822 V, and a spoken reply of 66.7 s
at the full level about 0.2 A at 4.918 V, both with no reset. Speech
draws about a third of what the tone does.

From stage 7 the firmware has a guard for a supply that cannot carry the
device at all: at the third brownout in a row, each within 60 s of the
boot before it, it stops before joining WiFi, plays nothing, and rests on
`ERROR`, "Power supply too weak". After a single brownout the screen says
"Restarted: supply dipped" for the first seconds. The guard was tested
with simulated brownouts only, and it cannot help a supply too weak for
the display and microphone, which start before it is asked
([ARCHITECTURE.md](ARCHITECTURE.md), Recovering by itself).

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
the device rather than a hang. Built in stage 5: each service's failure
ends the turn with its own error code, and each wait has a limit (5 s to
connect to any of them, 24 s of silence from Claude, 10 s from
text-to-speech), listed in [docs/PROTOCOL.md](docs/PROTOCOL.md). These
paths are tested with stand-ins for the services; none has been observed
with a real outage. From stage 7 the synthesiser gets a second try at
connecting within the same 5 s, after its first connection of a session
was once seen to hang.

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

The server keeps the device tokens in plain text too, in its untracked
`.env` file, beside the API keys. Anyone who can read that file can do
more with the API keys than with the tokens.

### R9 — Playback peak known only as an average, continuity not checked

The first bring-up was carried out without a multimeter or a USB power meter.
The figures were taken afterwards with a USB power meter and are recorded in
[docs/BRINGUP.md](docs/BRINGUP.md). Two gaps remain.

- **The true playback peak described in R2 is unmeasured.** The full-scale
  tone test in stage 2 read 0.556 A at 4.831 V, but the meter shows averages
  refreshed a few times per second, so it cannot catch a transient of about
  10 ms, and it reads voltage before the cable rather than at the board. The
  board did not reset, which is the decisive result for this supply. The
  stage 6 readings of 2026-10-07, 0.582 A for the tone and about 0.2 A
  for a spoken reply, were taken with the same meter and have the same
  limits.
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

### R17 — Reply audio is not paced, and the device must hold it

The server sends reply audio as fast as it is synthesised. Measured on
2026-10-06 with the desktop client: a reply of 61 s of audio was fully
received 32 s after the request ended. The device must therefore hold many
seconds of audio, or lose it. At 24 kHz a second is 48 KB, so a minute is
close to 3 MB of the 8 MB of PSRAM, and the reply ceiling of 8000 tokens
allows replies of several minutes.

The device cannot slow the server by reading slowly: a connection that
stops reading stops answering keepalive pings and is dropped.

Pacing also cannot be added to the server by simply waiting between
chunks. Sentences are synthesised one after another, each requested only
when the one before has been sent. With sending slowed to the speed of
playback, each sentence would be requested as the previous one finishes
playing, leaving a silence between sentences as long as the synthesiser
takes to answer.

*Mitigation:* built in stage 6. The server sends at most 2 s of audio ahead
of playback, and the device has room for 4 s (384 KB of PSRAM set aside,
enough at any rate the amplifier accepts). Synthesis runs ahead of sending,
so that pacing leaves no gap between sentences. Measured on 2026-10-06 with
the desktop client and synthetic speech: a 62 s reply was delivered over
about 60 s, never more than 2.05 s ahead. The alternative, a device that
holds the whole reply, was rejected: it needs megabytes of PSRAM and a hard
cap on reply length.

What remains of the risk: the server has no report of what the device has
played. It assumes playback starts with the first chunk and never pauses.
A device that starts late or stalls falls behind that assumption by as
much, which is what the device's second 2 s is for. If that is not enough
the device closes the connection.

On the device, 2026-10-06, on the breadboard: a reply of 33 s held at
most 2.08 s of the buffer's 4 s at once and never ran dry. The device logs
these figures for every reply, which is the evidence the 2 s and 4 s rest
on. A pause inside a reply was first counted by the server as playback,
which would have let the audio after it arrive further ahead than the
buffer holds; found in review and corrected before it was seen on the
device.

A network stall longer than what the device holds is not recovered from
within a reply: the device plays silence while the server's count runs on,
and the gap stays for the rest of the reply, each further stall adding to
it against the same 2 s of margin. Beyond that margin the device abandons
the turn. A report of playback from the device would close this; it is
left for a later protocol version, if the logged figures call for it.

### R18 — A pause between sentences ends the request

The end of a request is taken from 400 ms of silence after speech. A
speaker who pauses that long between two sentences is cut off after the
first. Observed on 2026-10-06 with synthetic speech: "Thank you. What is
two plus two?" ended at the pause, before the question was spoken. A
longer silence makes every reply later by the same amount.

A related case: speech-to-text can report words and then withdraw them. The
request then ends at once with nothing heard, and the device returns to
idle without a reply. Observed once on 2026-10-06, with a synthetic voice.

*Tried in stage 7, and left at 400 ms.* A setting of 1 s was tried on the
device on 2026-10-08, by a person, over six spoken requests. One in three
ended on the service's silence signal, 1 to 1.5 s after the speaker
stopped. The others ended only on its second signal, from word timings,
2 to 4 s after: the service does not send that one until it has settled
its transcript, which it does on its own schedule. So at 1 s the silence
signal is not dependable on the microphone's audio, which is never quite
silent. With synthetic speech followed by faint noise, sent to the service
directly, the same setting ended every request 1.7 to 2.1 s after the
recording, so the effect needs the device's own audio to show.

*Mitigation:* none in place; a speaker who pauses for 400 ms in
mid-request is still cut off. The silence length is one setting,
`ENDPOINTING_MS`, but raising it far is not a remedy, as above. What would
be: the server measuring the silence itself, from the word timings the
service reports several times a second, and ending the request when they
show no new word for the wanted time. Considered on 2026-10-08 and not
built. The server can log each of the service's events, without their
words, when the `server.stt.deepgram` logger is set to DEBUG.

### R19 — The server alone is over the latency budget

Measured on 2026-10-06 with the desktop client on the server's machine,
synthetic speech and a home internet connection, across four turns: from
the end of speech, Claude's first text arrived after 1.2-1.8 s and the
first reply audio after 2.4-3.5 s. The budget in
[ARCHITECTURE.md](ARCHITECTURE.md) allows 1.0-1.8 s for the same span.
About half is Claude's time to its first text, with thinking on at low
effort; the rest is the wait for the first sentence to be complete and
then synthesised. The device's own delays come on top.

On the device, 2026-10-06, on the breadboard, five short questions in a
synthetic voice, timed by the device's own log:

| Turn | Wake word to first sound | `stop_capture` to first sound | Of which the server (its own figure) |
| --- | --- | --- | --- |
| 1 | 6.53 s | 3.09 s | 3.03 s |
| 2 | 5.94 s | 2.34 s | 2.27 s |
| 3 | 6.73 s | 2.99 s | 2.91 s |
| 4 | 6.67 s | 3.26 s | 3.17 s |
| 5 | 5.52 s | 2.24 s | 2.17 s |

The first column includes the time taken to speak the question, about
3.5 s each. The second is what the person waits through: 2.2 to 3.3 s,
2.8 s on average. The device adds about 0.07 s to the server's own figure,
most of it the 80 ms it gathers before starting to play; the rest of the
wait is the server's. Within the server, Claude's first text took 1.4 to
2.3 s and the first sentence's synthesis 0.7 to 1.3 s.

Three requests spoken by a person the same day gave 2.50, 2.91 and 2.94 s
by the server's figure, in the same range, and eight more in one run gave
2.4 to 3.2 s by the device's log, 2.7 s on average.

In that run the first request after the server started failed: the
server's first connection to the synthesiser timed out after its 5 s
limit, and the turn ended with `tts_failed`. A connection opened ahead of
need, when the server starts or a request begins, would remove that
failure and shorten the first reply's wait as well; it is one of the
levers below. A second try at connecting would also have saved the turn.

The synthetic figures flatter the system in one respect. A synthetic recording
ends in silence that is part of the recording, so `stop_capture` arrived
as the playback of the question ended. After a person stops speaking, the
server first waits out the 400 ms of silence that marks the end, which
comes on top.

*Stage 7: measured, and accepted as it is.* The wait was judged
acceptable in use on 2026-10-08, so the stage took only the inexpensive
remedies and the budget is not met. The server's log now also gives, for
every turn, when speech-to-text was reached and when the model's first
whole sentence was complete; a turn that failed logs as far as it got.
With it, on 2026-10-08, over twelve turns with the desktop client: Claude's
first text 0.9 to 4.5 s after the end of speech, its first sentence 0 to
1.1 s later, the first audio 0.3 to 1.5 s after that. On the device, four
spoken turns: 1.8, 2.8, 2.8 and 5.0 s from the end of the request to the
first sound; and one more, 12 s, in which Claude's first text took 10.7 s.
Claude's time to its first text is nearly all of the wait and all of its
spread, and nothing on the server shortens it short of the levers below.

Two of the levers were looked into and closed. Keeping a connection to
the synthesiser warm was built, measured and taken out: its first audio
came 0.58 to 0.62 s after the request on a new connection, 0.58 s on one
just used and 0.50 to 0.52 s on one opened 8 s earlier, three times each,
and the same questions were answered no sooner. Requesting the next
sentence while the current one is being sent was already built in
stage 6. The failed first request is covered instead by a second try at
connecting.

*Mitigation:* none further planned. The server logs its figures for every
turn, and the device logs each change of phase with its time, as
`phase [state]`: `chime`
is the wake word, `capturing` the start of the request, `awaiting` its
end and `playing` the reply's first sound. Levers left, if the wait
becomes a nuisance: starting Claude on an early transcript before the
silence has run out, and synthesising the first clause instead of the
first whole sentence. One more, set aside in stage 7 as unlikely to gain
anything, since request audio already waits in a queue while the
speech-to-text connection opens (0.6 to 0.9 s, measured):
the device announces a request only when its chime is over, 220 ms after
the wake word, and the server opens its speech-to-text connection on that
announcement. Announcing at the wake word and sending audio from the end
of the chime would let that connection open while the chime plays. It
would move the server's capture limits by the same 220 ms and change the
wording of `utterance_start` in [docs/PROTOCOL.md](docs/PROTOCOL.md).

### R20 — Chip temperature

The ESP32-S3 runs warm in this firmware: its processor never idles, since
wake-word detection runs all the time, and from stage 6 its radio never
sleeps either, WiFi power saving being off. The chip was noticed to be hot
to the touch on 2026-10-06, though a finger could be kept on it for ten
seconds.

The chip has a temperature sensor of its own, read with the `temp` console
command. It measures the silicon, which is hotter than the chip's surface.
The chip is rated for surrounding air up to 85 °C; the sensor is accurate
to a few degrees.

**Every test on the device records the chip's temperature**, with what the
device was doing, how long it had been on, and the room if it is unusual.
The record:

| Date | Conditions | Inside the chip |
| --- | --- | --- |
| 2026-10-06 | Breadboard, USB-powered, open air. Idle: WiFi joined with power saving off, wake-word detection running, looking for a server that was not running. Read 15, 45, 75 and 105 s after power-on, the board already warm from earlier use | 51.5, 53.5, 54.5, 54.5 °C |
| 2026-10-06 | Same bench. Connected to the server and idle, about 20 min after the row above, during the connection tests (server stopped and restarted, device replaced, token refused) | 54.5 to 56.5 °C |
| 2026-10-06 | Same bench. While playing a reply of about 55 s at the fixed 40 % level, read three times 12 s apart, and once more just after it ended | 57.5 °C each time |
| 2026-10-06 | Same bench, after reflashing. Idle and connected, about a minute after power-on; during a reply of 33 s; just after it; and a minute later, idle | 55.5, 57.5, 57.5, 56.5 °C |
| 2026-10-06 | Same bench, after reflashing again. Half a minute after power-on, looking for a server that was not running | 54.5 °C |
| 2026-10-06 | Same bench, playback level 60 %. Idle and connected, before five short turns in a row; and just after them | 56.5, 57.5 °C |
| 2026-10-06 | Same bench, playback level 100 %. Before and after three spoken turns, the longest reply 15.6 s | 57.5, 57.5 °C |
| 2026-10-06 | Same bench, playback level 100 %, late evening, after hours switched off. Just after power-on; and after nine spoken turns in four and a half minutes | 46.5, 52.5 °C |
| 2026-10-07 | Same bench, playback level 100 %, with the chime. Connected and idle, 40 s after power-on and 90 s later; after three spoken turns; and during a reply of 16.4 s, the fifth turn | 49.5, 51.5, 52.5, 53.5 °C |
| 2026-10-07 | Same bench, after reflashing. Connected and idle: half a minute and two minutes after power-on with no server running, then twelve minutes after power-on, just before one spoken turn | 52.5, 54.5, 55.5 °C |
| 2026-10-07 | Same bench, afternoon, after hours switched off. Connected, during the first reply of six spoken turns, 25 s after the log began; and nine minutes later, idle with no server, four minutes after the last reply | 46.5, 54.5 °C |
| 2026-10-07 | Same bench. Before and after a minute of 440 Hz tone at 35 % of full scale, with no server running, two minutes after a restart | 49.5, 50.5 °C |
| 2026-10-07 | Same bench, for the supply check. Ten seconds after a restart and just after 10 s of tone at 100 %; then connected and idle a minute later, before a spoken reply of 66.7 s | 46.5, 48.5, 49.5 °C |
| 2026-10-07 | Same bench. After the screens of all eight states had been shown in turn, 80 s after a restart, the board warm from the supply check, with no server running | 56.5 °C |
| 2026-10-08 | Same bench, on the stage 7 firmware with the task watchdog. 25 s after flashing, looking for a server that was not running; and 21 min after a restart, connected and idle, three minutes after two spoken turns | 43.5, 54.5 °C |
| 2026-10-08 | Same bench, same firmware, on for 26 to 50 min. Looking for a server just after three spoken turns; connected and idle half a minute later; and two minutes after three more spoken turns, the longest reply 8.8 s | 55.5, 55.5, 56.5 °C |
| 2026-10-08 | Same bench, same firmware, on for 58 to 70 min, while the server was stopped in the middle of four turns in succession. Just after a reply of 15 s; and connected and idle about a minute after each of the last three losses | 57.5, 56.5, 56.5, 56.5 °C |
| 2026-10-08 | Same bench, during the watchdog and brownout-guard tests: after seven restarts in three minutes, idle and connected; and 15 s after reflashing, idle and connected | 53.5, 51.5 °C |
| 2026-10-08 | Same bench, on the firmware as corrected after review, 15 s after flashing, idle and connected; and about 25 min later, idle, a few minutes after one spoken turn | 52.5, 56.5 °C |
| 2026-10-12 | Same bench, the stage 7 firmware, with the server reaching Claude through Claude Code (R21). 12 s after a reset, the board already warm; then once a minute through eight spoken turns in five minutes, four of them with a search | 53.5 °C; 55.5, 56.5, 55.5, 56.5, 56.5 °C |
| 2026-10-12 | Same bench, the same session. Connected and idle, once a minute for the eighteen minutes after those turns | 55.5 °C each time but one, of 54.5 °C |
| 2026-10-12 | Same bench, after a further reset. Connected and idle, just before four spoken turns; a minute later, between them; and during the last reply, of 18.7 s with a silence of 6.2 s in it | 55.5, 55.5, 57.5 °C |

The readings during replies were taken at playback levels of 40 % and
60 %, but for the last, of 2026-10-07, at the present 100 %.

Not yet measured: after hours of running; inside an enclosure, which will
be warmer than open air; and with WiFi power saving on, for comparison.

*Mitigation:* none needed at these figures. If later readings approach
70 °C, the first things to try are WiFi power saving back on, with the
send limit that stage 6 raised left as it is, and a lower processor speed
outside conversations. An enclosure is to be designed with the reading in
hand.

### R21 — The trial of Claude through Claude Code, and the open web

With the server setting `LLM_BACKEND=claude_code`, a trial built in
stage 7, the assistant is the Claude Code tool running under the server
owner's subscription login, and it can search the web and read pages
([ARCHITECTURE.md](ARCHITECTURE.md), Claude through Claude Code). The API
with no web access remains the default. The trial brings risks the API
route does not have.

**A page can mislead the assistant.** Text on a web page, or in a search
result, can be written to be taken for instructions. The assistant cannot
run a command, touch a file or reach the account's connectors, and holds
no credential, so what such text can bring about is limited to three
things: a wrong answer spoken with confidence; the day's conversation sent
out inside an address the assistant is told to open; and a page on the
local network being read, such as a router's status page. The conversation
is what was said to the device that day.

This was weighed on 2026-10-12 and accepted: reading any page is the
default. It was arrived at in steps, on measurements. Search alone
returns short extracts, and did not answer "What is the weather in London
today?" in five tries of five. Allowed to read five named sites, a
weather service, three news sites and an encyclopaedia, it answered the
weather, and failed the news in the one try, every news site refusing to
be read. Allowed any site, it answered all of weather, news, the price of
gold and a race result in one run of each.

*Mitigation:* `CLAUDE_FETCH_DOMAINS` restricts reading to named sites,
each an exact host name, or turns it off with an empty list. With named
sites the tool refuses every other, which closes the second and third of
the three; the first, a wrong answer, comes with any use of the web, and
with search alone. That a named-sites policy does refuse an unlisted site
was not seen: asked to read one, the model declined without trying. The
check made on every run covers the tools, the MCP servers and the
permission mode; the tool does not report the permission rules
themselves.

**The tool is not a fixed interface.** It updates itself. The client
relies on about fifteen of its options and on the shape of the events it
prints, written against version 2.1.296. An update can break either; every
turn then fails with `llm_failed`, and the server's log gives the tool's
own complaint. The stand-in the tests use is a copy of that shape, so the
tests cannot notice a change in the real tool.

*Mitigation:* run `python -m server.tools.claude_cli_bench` after an
update; it takes a minute and puts fixed questions through the server's
own client. The server logs the tool's version at the first turn.

**A subscription has usage limits**, shared with whatever else the
account's owner does with Claude Code. When one is reached every turn
fails until it resets. Not seen. There is no falling back to the API:
that would need both credentials on the server and was left out of the
trial.

**A searched reply that stays silent for 24 s is given up.** The limit on
silence from Claude is counted in text, as described in
ARCHITECTURE.md, and a device hears nothing while the assistant searches.
The silence itself was not logged on the bench, only the whole reply.
Before the limit was counted so, most replies that used the web were
whole in 10 to 20 s; three took 24.9, 26.5 and, on the device, 20.6 s,
with two, four and five look-ups. The limit was 20 s when it was first
counted in text, and was raised on 2026-10-12 to 24 s, the most the
device's own limit leaves room for, which leaves a second of margin
where there were five. The longest of those three replies may still end
with `llm_failed` and the device's "error" screen in place of a late
answer.
The prompt now tells the model to make at most two look-ups and why.
No reply has yet been seen to be given up for silence.

**Answers from the web are only as good as what was found.** Seen on
2026-10-12: a race result reported as unconfirmed, or given to the wrong
driver with a warning, in two of five tries with search alone; a gold
price hedged across a range of nearly a thousand dollars; and twice, on a
Monday, the remark that markets are closed at the weekend, in replies
that had the day right. The last is the model's own reasoning.

**A sentence that opens a line with "Sources:" is taken for the list.**
The client removes everything from such a line to the end of that part of
the reply. The server's log counts what it removed in each reply
(`sources_dropped`), so use shows whether the removal is needed at all:
after the prompt forbade the list, none came in the replies that were
looked at.

**What was measured, and what was not.** All on 2026-10-12, `claude-opus-5`
at low effort, Claude Code 2.1.296, a home internet connection.

- On the server's machine, with the bench script, timed from the start of
  the process. Plain questions: first text after 1.72 to 2.50 s over
  five, median 1.91 s; with ten earlier exchanges sent along, 1.67 to
  3.95 s over fifteen, median 1.95 s, and the whole reply in 3.2 to 5.5 s.
  Questions needing the web, search only, with the same history: first
  text after 1.64 to 2.28 s over twelve, the whole reply in 10.3 to
  26.5 s. Reading any site, four questions: the whole reply in 9.8 to
  19.4 s.
- On the device, on the breadboard, a person speaking, by the server's
  log from the end of the request. Before "Let me check." was spoken in
  time: four plain answers began after 3.3 to 5.4 s, four that used the
  web after 7.3 to 20.0 s. After: two plain answers after 3.2 and 3.3 s,
  two that used the web after 3.3 and 3.9 s, each with 6.2 s of silence
  before the answer.
- Not measured: a day's real conversation as history; a usage limit; the
  wait when the tool has not been run for hours; more than twelve turns
  on the device.
- The figures above are from the code before the review's corrections.
  The corrected code was then run against the tool, once, on the
  server's machine, with the limit at 20 s: eleven questions, all
  answered. First text after 1.78 to 2.50 s for five plain ones and 1.61
  to 2.27 s for six others; the whole reply in 8.1 to 18.0 s for the five
  that searched or read a page, each opening with "Let me check.",
  including one that read a page with no search before it; no list of
  sources removed, none having come. It has not been run on the device
  since the corrections, nor at all with the limit at 24 s.

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
- **Log redaction broke formatted log lines.** Found in stage 5: the
  server's redaction turned every log argument into text, so any line with
  a number format failed to print. Redaction now runs on the finished
  line, which also brings exception tracebacks under it.
- **A rejected `DEVICE_TOKENS` setting printed token values.** Found in
  review in stage 5, before any commit: the start-up error for a token
  that was too short quoted the whole setting. Configuration errors no
  longer quote any value.
- **A device that could not hear from its first second could show `IDLE`.**
  The wake-word component announced a dead microphone about a second
  after it started, a moment before the state machine began listening for
  the announcement. From stage 7 the state machine asks the wake-word
  component whether the device can hear, each time it chooses what to
  show, and keeps no copy of the answer.
- **A reply's summary line could count one pause as two.** Corrected in
  stage 7; see Caveats, "A reply can pause briefly".
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
- **Earlier days' conversations are deleted, but not at the rollover
  itself.** They go when the first exchange of a new day is stored, so a
  server that is not spoken to keeps the last day's conversation until it
  is. Deleted rows are not overwritten in the database file.
- **A request cannot be interrupted.** A wake word during a reply does
  nothing; the reply plays to its end.
- **Only completed turns are remembered.** A request whose reply failed, or
  that the model declined, is not part of the conversation, so a follow-up
  cannot refer to it.
- **The assistant does not know the date or time**, and says so when
  asked. The system prompt is kept free of anything that varies, for
  prompt caching. With the trial of R21 it knows the date, which the
  Claude Code tool tells it, and not the time of day.
- **The server has been run only with synthetic speech.** Its end-to-end
  test used a text-to-speech voice through the desktop client. No real
  voice, microphone or device has been through it yet.
- **The desktop client is not the device.** It starts sending audio at the
  request itself and takes the reply as fast as it comes; the device will
  first send a burst of audio it already holds, and will take the reply at
  the speed of playback.
- **The server's dependencies are pinned one level deep.** The packages it
  names are pinned exactly; the packages those depend on are not.
- **The server needs the `tzdata` package on Windows**, which has no
  timezone database of its own. It is in the server's requirements.
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
- **The wake word works only with a server.** From stage 6 the device
  acts on it only in `IDLE`, which it reaches only while connected. The
  wake-word counters of the `wake` command count detections in any state.
- **No state can be set from the console.** The `state` command only reads,
  since the state machine is the state's one writer. To look at a state's
  screen, `display preview <state>` draws it for a few seconds and leaves
  the state alone.
- **The console's audio tests and a reply share the amplifier.** `audio
  tone` or `audio loop` typed while a reply is playing would disturb both.
- **Speak after the chime.** The device plays two notes when it
  hears the wake word and starts listening when they end, 220 ms after
  the wake word as measured. Words spoken before that are not sent. The
  sound's level, 30 % of full scale, and its length were set by ear in one
  sitting and have no control.
- **A reply can pause briefly.** Seen on 2026-10-07: a reply of 16.4 s ran
  out of audio three times, for 0.4 s in all. Replies of 9.6 s and less
  the same day did not, nor did one of 33 s the day before: about one
  reply in twenty over the two days. Looked into the same day, on the
  server's machine with no device. The cause was not found; what was
  established:
  - *The server's supply of audio is not it.* The synthesiser's first
    audio came 0.2 to 0.3 s after each request, and it delivers speech
    in lumps of about 0.7 s of audio every 0.3 s, a little over twice as
    fast as it plays; a lump is sometimes 0.3 s late. Replayed against
    the device's 80 ms start-up buffer, that never ran out: not once in
    120 sentences, and once, by 4 ms, in 14 whole replies sent through
    the server's own pacing.
  - *Not the model's speed.* In the reply that paused, all of the text
    was written 1.2 s after its first word.
  - *The server did not see the pauses.* It finished sending that reply
    when an unbroken 2 s lead predicts, to within 0.06 s. So the audio
    was sent on time and reached the player late. What delayed it is not
    known: WiFi, or the device's own receiving. It is not the server
    holding back small packets: its connections have Nagle's algorithm
    off, as checked in the Python it runs on.
  - *Tried and taken out:* requesting each sentence from the synthesiser
    as soon as the one before it began to arrive, on the belief that the
    wait between the first two sentences left the device short. The
    waits seen at the start of a reply turned out to be the synthesiser's
    lumps, and were the same with the change as without it.
  - *Not settled:* when in the reply the pauses fell. With 2 s in hand
    in mid-reply, a late delivery can empty the buffer only in the first
    second or so, unless it is later by more than 2 s.

  The device now logs each pause as it ends, with how far into the reply
  it fell and how long it lasted. First run with it, later the same day:
  six spoken turns, replies of 3.4 to 20.9 s, and one pause, 0.84 s into
  a reply of 16 s and shorter than the 10 ms the device's clock resolves.
  That is at the start, and at the end of one of the synthesiser's
  lumps. The console's `temp` and `mem` commands, typed during another
  of those replies, caused none. One sample is not proof.

  Until 2026-10-08 the reply's summary line could count one pause as
  two: its count went up again if the buffer was still empty when the
  player next looked. Corrected in stage 7; counts in earlier logs are
  to be read with that in mind.

  Second sample, on 2026-10-08: of ten replies heard that day, one
  paused, twice, for 240 ms and 10 ms, 0.72 s and 0.76 s into a reply of
  8.8 s. Again in the first second. The double count is corrected: that
  reply's summary line said two, as its two lines did.

  When a reply is stopped part-way, as by a lost connection, its summary
  line reports "most held at once 0 ms" whatever it held. Seen on
  2026-10-08; the figure is read after the stop has already cleared it.
  Not corrected.

  A larger start-up buffer, 80 ms raised to about 300 ms, would cover a
  lump up to 0.3 s late at the start, at the cost of about 0.2 s more
  wait before every reply. Not tried, and not decided (R19).

  The same is seen after a long silence inside a reply. With the trial
  of R21 a reply says "Let me check.", is silent while the assistant
  searches, and then gives the answer. Seen on 2026-10-12, twice: after a
  silence of 6.2 s the answer ran out of audio two and four more times,
  for 40 to 80 ms each, in its first second. The player starts again
  from an empty buffer there as it does at the start of a reply, with
  the same 80 ms in hand. Found during part C of stage 7 and not
  corrected, the player being outside that part.
- **The speaker shakes the breadboard, and that can be heard.** On
  2026-10-07, in the last two of three long replies played almost back
  to back, the voice turned robotic part-way through each. The device
  and the server logged nothing wrong, and a minute of test tone at 35 %
  showed no fault in the logs either. Lifting the breadboard and holding
  it in the hands made the voice clear: the cause is mechanical, the
  speaker's vibration acting on the breadboard and what is plugged into
  it, and nothing in the firmware. To be dealt with when the circuit is
  soldered and the speaker is mounted apart from the electronics; until
  then, a distorted voice on the breadboard is to be checked by lifting
  it before anything else is suspected.
- **Playback has no volume control.** Reply audio is played at the level
  the server sends it, until stage 8. At 40 % and at 60 % of that level a
  reply was too quiet at arm's length; at 100 % it is a little quiet, as
  the next item says.
- **Replies are a little quiet.** Heard on 2026-10-06 at arm's length on
  the breadboard, with playback at its full level. The voice in use,
  `aura-2-luna-en`, was chosen by ear for its "s": the voice first used
  had a harsh one, in the synthesised audio itself, the same in a headset
  as on the device. The chosen voice peaks about 6 dB lower than that one
  (-8.8 dBFS against -1.9 dBFS on the same sentence). Ways to make it
  louder, none of them done yet:
  - *The amplifier's gain pin, in hardware.* The MAX98357A's `GAIN` pin is
    unconnected, which sets 9 dB. Tied straight to ground it sets 12 dB,
    and to ground through a 100 kΩ resistor, 15 dB: 3 or 6 dB more with no
    change to the firmware. It raises the amplifier's peak current with
    it, so `audio tone 100` is to be repeated afterwards as the supply
    check (R2).
  - *A boost in the firmware.* Multiply the reply's samples before playing
    them. This voice leaves almost 9 dB unused below full scale, so about
    6 dB can be added with little or no clipping; more than that needs a
    limiter, which turns loud peaks down instead of letting them distort.
    No hardware change; noise in the audio rises with it.
  - *Levelling on the server.* Scale each reply so that its loudest
    sample sits just below full scale, whatever the voice. It gives every
    voice the same loudness and keeps the firmware as it is, but the
    sentences of one reply are synthesised separately and would each need
    the same factor.
  - *Another voice.* `aura-2-andromeda-en` was also found clear, peaks
    about 3 dB higher than the chosen one, and speaks faster.
  Real volume control is stage 8; whichever of these is used sets the
  loudest level that control can reach.
- **English only.** Speech-to-text is set to English and the synthesiser's
  voice is English. A request in another language is not recognised: the
  device listens, the server finds no words, and the device returns to
  idle with no reply. Observed on 2026-10-06 with a request in Russian.
- **The `temp` and `mem` commands are diagnostic aids in files of their
  own**, `firmware/main/temp_cmd.c` and `mem_cmd.c`, each of which says
  what to delete to remove it.
- **A send to the server can block for several seconds on a stalled
  network.** The WebSocket client applies its send timeout more than once
  within one send. The upload then falls behind the microphone. From
  stage 7, a request that loses audio that way, at its start or further
  in, is given up with "Could not send the request" instead of being
  answered with a hole in it. The send itself still blocks: seen on
  2026-10-08, 1.5 s, in the upload's last send after the server had been
  stopped. Nothing waits on it but the upload.
- **Nothing in the firmware can be stopped once started.** A refused
  device parks its connection task until reboot, and neither the server link nor
  the player has a stop. An over-the-air update, if adopted in stage 8,
  will want the turn machinery stopped with WiFi kept.
- **The watchdog and the brownout guard were tested with a test build.**
  On 2026-10-08 a build with one extra console command hung the
  state-machine task, and counted ordinary restarts as brownouts; it was
  not kept. Not tested: the capture and player tasks hung, a real
  brownout, and, after the review's corrections moved the wording into
  the state machine, the "Restarted" line itself, which was seen only in
  its earlier form.
- **A watchdog restart loses the turn in progress**, like any restart. The
  server learns of it when the connection closes.
- **The screen is dark for about two seconds after reset.** The panel's
  start-up sequence has required pauses of about a second, and the
  backlight is lit only once the first frame is on the panel.
- **About one second passes between the boot banner and WiFi start.** Observed
  on every boot; the cause has not been investigated. It is a boot-time cost,
  not a latency on requests.
