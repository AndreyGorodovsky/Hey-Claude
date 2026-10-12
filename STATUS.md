# Status

**Current stage:** Stage 7, latency and robustness. Its parts A, latency,
and B, robustness, are built, reviewed and corrected, with two tests left
for later: WiFi lost and restored, and a run of several hours. Part C, a
trial of reaching Claude through a Claude Code subscription, with access
to the web, is built on a branch of its own, measured on the device,
reviewed and corrected. The steps are under Stage 7 plan below.

**Last updated:** 2026-10-12

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

Part B. "Hey Claude" models are trained in Google Colab with a
notebook in `firmware/tools/wakeword_training/`, described in
[docs/WAKEWORD-TRAINING.md](docs/WAKEWORD-TRAINING.md), whose run history
is the one record of each run's results. Two runs on synthetic voices only
fell well short of the targets on the device. The firmware builds the
third, `hey_claude_run3`, and the boot log and `wake` command name the
model, so each device test can be tied to its run. Version 2 of the
notebook, which trained run 3, adds recordings of real voices, made following
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

Run 3, trained on 2026-10-04 with 150 recordings of one speaker, is the
first to work on the device: in the device test that speaker was
detected in nearly every try at 0.5, 1 and 2 m. It is specialised to the
recorded voice. The notebook's test on synthetic voices suggests other
speakers will be missed far more often until they are recorded too, and
"hey cloud" can still trigger. The figures and their limits are in the
run history; what remains open is in KNOWN-ISSUES R1 and R16. The models
of earlier runs and the stock model were removed from the firmware.

The server, built in stage 5, runs with `python -m server`. A device
connects over a WebSocket with its identifier and token, and each request
goes through Deepgram speech-to-text, Claude and Deepgram text-to-speech,
with the reply sent back sentence by sentence as it is generated. The
server decides when the speaker has finished, keeps each device's
conversation for the current day in SQLite, deletes earlier days, and
advertises itself on the network by mDNS. A request cannot be interrupted
by another. The protocol is written down in
[docs/PROTOCOL.md](docs/PROTOCOL.md), which is what the stage 6 firmware
is built against.

The server is tested at two levels. Its own tests, 100 of them as of
stage 7 and 161 with part C's, run the
whole path with stand-ins for the three services and need no keys or
network. A desktop client, `server/tools/desktop_client.py`, plays recorded
requests to a running server as the device will. Measured with it on
2026-10-06, with synthetic speech, on the server's machine: four of five
requests were answered aloud and the fifth, whose words speech-to-text
withdrew, ended without a reply as designed; the server was found by mDNS, a later request was answered from
the conversation's history, prompt caching took effect from the third
request, and the first reply audio came 2.4-3.5 s after the end of speech,
above the budget (KNOWN-ISSUES R19). The review agents reviewed the stage
before it was committed; what they found and what was deferred is in
[KNOWN-ISSUES.md](KNOWN-ISSUES.md) (R17 to R19, and the caveats).

## Stage 7 plan

Planned and approved on 2026-10-08. The wait for a reply, about 3 s from
the end of speech against a budget of 1.3 to 2.3 s, was judged acceptable
in use, so the stage takes only the inexpensive ways of shortening it and
does not have to reach the budget. Parts A and B were reviewed together
by both review agents on 2026-10-08 and corrected; part C is reviewed and
committed by itself.

**What the review changed.** Neither agent found a critical or major
fault. Corrected from the first: a request whose opening audio was lost
before sending was not caught by the new check; the synthesiser's two
tries could take 10 s where 5 s was claimed, which is now one limit over
both; a watched task that had failed to register with the watchdog would
have logged an error on every pass. From the second: the "Restarted"
wording moved from the boot sequence into the state machine's one table,
so that the state has one writer again; whether the device can hear is
now asked of the wake-word component and no longer kept in three places;
the brownout guard moved into a file of its own; each watched task
asserts at build time that its waits fit the watchdog's limit; the state
machine tells a deadline from a watchdog wake-up by the clock, not by a
flag; a turn that fails logs its timings as far as it got. Left as it is:
the player still times a pause in two ways, which is untidy and exact
enough. The corrected firmware was built, flashed and seen to reach
`IDLE`; the corrected server answered two requests from the desktop
client. One request spoken by a person to the corrected firmware was
answered as before; the wait for its reply was 6.9 s, of which Claude's
first text took 5.3 s.

**Part A, latency, on the server.**

| Step | State |
| --- | --- |
| A1. Finer timing log: when speech-to-text was reached, and the model's first whole sentence, beside the first text and first audio already logged | Done. Baseline measured on 2026-10-08 with the desktop client on the server's machine, five short questions in a synthetic voice, a home internet connection: speech-to-text was reached 0.84 to 0.88 s after each request began; from the end of speech, Claude's first text came after 0.9 to 3.5 s, its first whole sentence 0 to 0.9 s after that, and the first audio 0.3 to 0.75 s after the sentence, 1.2 to 5.1 s in all. In the first turn the end of speech was detected 5 s late, once |
| A2. Connections to the synthesiser and to Claude opened ahead of need, at server start and when a request begins; a second try at reaching the synthesiser (R19) | Done in part, by design. The second try is built, by the HTTP library: each try is cut short at half the 5 s allowed for reaching a service, and one limit of 5 s runs over both, so no limit in the protocol moves. A connection that takes more than 2.5 s in one of its steps now fails where it used to succeed. Opening connections ahead of need was built, measured and taken out again. Measured on 2026-10-08 on the server's machine, three times each: the synthesiser's first audio came 0.58 to 0.62 s after the request on a new connection, 0.58 s on one just used, and 0.50 to 0.52 s on one opened 8 s earlier; and the same five questions as the baseline were answered no sooner with it. The wait is the synthesiser's own |
| A3. The end-of-speech silence tuned against requests spoken by a person, with and without a pause in mid-sentence (R18) | Done: the silence stays at 400 ms. A setting of 1 s was tried on the device on 2026-10-08, by a person, over six spoken turns, and made the end of a request erratic: one request in three ended on the service's silence signal, 1 to 1.5 s after the speaker stopped, and the others only on its slower signal from word timings, 2 to 4 s after. With synthetic speech and faint noise sent to the service directly, the same setting ended every request 1.7 to 2.1 s after the recording, so the effect needs the microphone's own audio. Measuring the silence on the server from the service's word timings would give a longer silence dependably; it was considered and not taken up |
| A4. The wait measured again, with the desktop client and on the device | Done, 2026-10-08. Nothing built in part A shortens the wait, so these figures stand beside those of stage 6 and do not replace an estimate. Desktop client, a second run of the same five questions as A1's baseline: 3.2 to 5.6 s from the end of speech to the first audio, of which Claude's first text was 1.9 to 4.5 s. On the device, by its own log, four turns spoken by a person at the 400 ms silence: 1.8, 2.8, 2.8 and 5.0 s from the end of the request to the first sound. One reply the same day, during the trial of a longer silence, waited 12 s, of which Claude's first text was 10.7 s. Claude's time to its first text is the whole of the spread |

Set aside: synthesising the first clause instead of the first sentence,
work on Claude's time to its first text, and starting Claude on an early
transcript. They stay listed in KNOWN-ISSUES R19.

**Part B, robustness, mostly firmware.**

| Step | State |
| --- | --- |
| B1. WiFi lost and restored in each of `IDLE`, `CAPTURING`, `THINKING` and `SPEAKING`, on the bench; what is found is corrected | Put off on 2026-10-08: the WiFi could not be switched off for the test. Listed under Before release. The server-drop tests of B2 exercise the same recovery from the moment the connection is lost, but not the loss of WiFi itself or the wait for it to return |
| B2. The server stopped in each phase of a turn | Done on the bench on 2026-10-08, a person speaking, the server's process killed by a script watching its log: 2.1 s into a request, 0.4 s and 7.4 s after a request ended with the device waiting for the reply, and 3.5 s into a spoken reply. Each time the device showed the loss within about 30 ms, held `ERROR` for 4 s, rested on `CONNECTING`, and was back in `IDLE` by itself 12.7 to 21 s after the loss, within 2 to 3 s of the server being up again. The reply that was cut stopped within about 100 ms, and cleanly as heard. A request spoken after the first loss was answered. Nothing needed correcting |
| B3. Watchdogs: a hung task resets the device; the capture, player and state-machine tasks are watched; the reason for the reset is shown after it | Done. A watched task that is silent for 5 s restarts the chip, with a core dump; after a restart nobody asked for, the line under `CONNECTING` names the cause until the first attempt to reach the server has ended. Seen on the bench on 2026-10-08, twice, with a test build that could hang the state-machine task on command, not kept: the watchdog named the task and restarted the chip 5 to 8 s after the hang, the core dump was saved, the boot log gave "task watchdog" as the reset reason, the state read "connecting, Restarted: watchdog", and the device was back in `IDLE` 5 and 10 s after the restart. Over about 70 minutes of ordinary use before that, with thirteen turns and four lost connections, the watchdog did not fire. The capture and player tasks were not hung on purpose |
| B4. Brownout guard: the cause shown at boot, and repeated brownouts ending in `ERROR` with the amplifier off instead of a loop of resets | Done, and seen on the bench on 2026-10-08 with a test build that counted its next three boots as brownouts, not kept: the first two started normally with the state reading "connecting, Restarted: supply dipped"; the third stopped at "error, Power supply too weak" with no attempt to connect; a restart from the console cleared it and the device returned to `IDLE`. No real brownout was caused, so what the chip's memory keeps through one is taken from its documentation, not seen. At the third brownout in a row, each within 60 s of the boot before it, the device does not join WiFi or play anything and shows `ERROR`, "Power supply too weak", until it is powered off and on or restarted from the console. The count is kept in memory that survives a restart but not a loss of power; nothing is written to flash |
| B5. Small known defects: a pause counted twice in a reply's summary line, a dead microphone missed in the first second after boot, a send that blocks for seconds on a stalled network | Done. Seen on the device on 2026-10-08: a reply that paused twice was counted as two. The other two have not been brought about on the bench. A dry spell is counted once; the state machine asks the wake-word component whether the device can hear instead of keeping its own copy; and a request that loses audio to a stalled send, at its start or further in, is given up with "Could not send the request" instead of being answered with a hole in it. The send itself still blocks: its timeout is inside the WebSocket component |
| B6. A run of several hours with turns at intervals: memory, stack headroom, pauses, reconnections and chip temperature | Not yet made; to be done after the review. The longest run so far on the stage 7 firmware is about 70 minutes, with thirteen turns |

**Part C, the assistant's model reached through a Claude Code
subscription, as a trial.** Approved on 2026-10-08 and built on
2026-10-12, on the git branch `stage7c-subscription-trial`, so that it can
be dropped whole if it does not work out. The API remains the default;
the trial is chosen with the server setting `LLM_BACKEND=claude_code`. It
changes no firmware and nothing in the protocol. What it is and how it is
locked down is in [ARCHITECTURE.md](ARCHITECTURE.md), under Claude through
Claude Code; its risks and every figure are in R21 of
[KNOWN-ISSUES.md](KNOWN-ISSUES.md).

| Step | State |
| --- | --- |
| C1. Bench trial with no change to the server: how long `claude -p` takes to its first text, with and without a web search | Done, with `server/tools/claude_cli_bench.py`, which puts written questions to the tool through the server's own client. Measured on 2026-10-12 on the server's machine, a home internet connection, `claude-opus-5` at low effort, Claude Code 2.1.296, timed from the start of the process: the first text of a plain answer came after 1.7 to 2.5 s over five questions, median 1.9 s, and after 1.7 to 4.0 s over fifteen with ten earlier exchanges sent along, median 1.95 s, so history costs nothing that can be told. With a search, the first text came as soon and the whole reply took 10 to 26 s |
| C2. If that is tolerable, a second language-model adapter that runs `claude -p` and feeds the existing sentence pipeline, chosen by one setting | Done: `server/llm/claude_code.py`, one process for each turn, behind the interface the API client already had. The pipeline is unchanged |
| C3. Locked down: web search and web fetch only, no shell or file tools, run from an empty directory, and without the API key in its environment | Done, and checked on every run from what the process says it started with. Reading pages was first left out, then allowed for a short list of sites, then for any site, each step decided on what the bench showed: search alone did not answer the weather in five tries of five; with five listed sites the weather was answered and the news was not, its sites refusing to be read; with any site, all five questions were answered. A list of sites remains a setting, `CLAUDE_FETCH_DOMAINS` |
| C4. The system prompt says the assistant can search; the conversation stays in the server's own store | Done. The prompt also asks for a short sentence before a search, "Let me check.", so that the device is not silent while it runs, and forbids the list of sources the search tool asks the model to add, which the client removes if it comes all the same. The tool tells the model the date, so the assistant now knows the day, and not the hour |
| C5. Measured on the device, reviewed, documented | Done on 2026-10-12, on the breadboard, a person speaking, twelve turns in two runs; the figures are below. Reviewed by both review agents and corrected |

**Part C on the device, 2026-10-12.** Breadboard, USB-powered, the server
on a computer on the same network, a person speaking. All times are from
the end of the request to the first sound, by the server's log.

- First run, eight turns. Four plain answers began after 3.3 to 5.4 s.
  Four that used the web began after 7.3 to 20.0 s, and "Let me check."
  was heard only with the answer: nothing ended its sentence until the
  search was over, so it was not spoken. A one-sentence answer waited the
  same way for the process to finish, about 1.5 s.
- Corrected, by ending the sentence where the tool reports the end of a
  block of text, and run again, four turns. Two plain answers began after
  3.2 and 3.3 s. Two that used the web began after 3.3 and 3.9 s with
  "Let me check.", were silent for 6.2 s each, and then gave the answer.
  A person judged the sentence prompt and the silence acceptable.
- Through both runs no request audio was lost. In the first no reply ran
  out of audio; in the second each searched reply ran out for the 6.2 s
  of the search, as it must, and then two to four more times for 40 to
  80 ms in the first second of the answer (KNOWN-ISSUES, Caveats, "A reply
  can pause briefly").
- Chip temperature: 53.5 °C just after a reset, the board warm; 55.5 to
  56.5 °C through the first run; 55.5 °C idle for the eighteen minutes
  between; 57.5 °C during the last reply (R20).

**What the review of part C changed.** Neither agent found a critical
fault. Both found the same major one: the limit of 20 s on silence from
Claude was counted between any two lines the process printed, and it
prints a line for every step of a search, so a reply that searched again
and again could stay silent past the 40 s a device waits, and the device
would have given up before the server did. The limit is now counted in
text that can be spoken, and was raised from 20 s to 24 s, which is as
far as the device's own limit allows, so that fewer replies that search
twice are cut. Corrected from the first agent besides: a turn
cancelled as its process was ending could leave the process running; a
list of sources in the middle of a reply took the answer after it; a
blank API key passed for a key; a process that could not start left
nothing in the log to say why. From the second: which sites may be read
is one small type with one reader of the setting, where a list had three
meanings and five readers; the web's part of the system prompt moved out
of the prompt the API uses into a file of the trial's own, so that the
trial is dropped by deleting files; what is made of the tool's events is
apart from the starting and stopping of the process, and tested without
one; and the rule the "Let me check." fault broke is written into the
interface. Left as it is: a parameter of the client that exists for the
tests. The corrected code passes its tests and answered eleven questions
put to the tool on the server's machine; it has not been run on the
device since (R21).

Settled with the plan: the start-up buffer stays at 80 ms until B6's log
says otherwise, and the request is not announced while the chime plays.

## Stage 6 as built

Closed on 2026-10-07. Everything is written, reviewed by both review
agents, corrected and run on the breadboard, and a person has held
conversations with the device on two days, each request answered aloud
unless noted below. The chime was added on the second day, after the
first spoken test, and was reviewed by both agents by itself.

| Step | State |
| --- | --- |
| 1. Server: paced replies | Done. At most 2 s of reply audio is sent ahead of playback, and the next sentence is synthesised while the current one is sent |
| 2. Firmware: connection | Done and seen working: finds the server by mDNS, is accepted with its token, reconnects by itself |
| 3. Firmware: state machine | Done and seen working through every state of a turn |
| 4. Firmware: upload | Done and seen working: two requests in a synthetic voice were transcribed word for word |
| 5. Firmware: playback | Done and seen working: replies of 0.6 s and 33 s played to their end, and were heard. At 40 % and then 60 % of the server's level they were too quiet; the audio is now played as sent, at 100 %, which was heard and is better, though a little quiet with the voice now in use |
| 6. Firmware: failure paths | Done. Seen working: a server killed and restarted, a second connection under the device's identifier, a refused token, and a server `error` in the middle of a turn. Not yet seen: the device's own deadlines, an incompatible version; put off until after stage 8 (Before release) |
| 7. Measurements | Done: memory, stack headroom, chip temperature, pacing, the wait for a reply, the long run, taken as made: nine spoken turns in a row over four and a half minutes on 2026-10-06, shorter than the ten minutes planned, and about twenty more turns over several sittings the next day, accepted together in its place, and the supply check, measured on 2026-10-07 with the USB power meter: about 0.582 A at 4.822 V for 10 s of full-scale tone, and about 0.2 A at 4.918 V through a spoken reply of 66.7 s at the full level, with no reset in either (R2) |
| 8. Firmware: chime | Done and heard, added on 2026-10-07 after the first spoken test: two notes when the wake word is heard, and the request starts when they end |

New firmware components: `server_link` (the connection), `player` (reply
playback), `state_machine` (the state machine and the upload) and
`protocol` (the protocol's numbers, one header). New console commands:
`link`, the state of the connection; `temp`, the chip's temperature; `mem`,
free memory and stack headroom; and `display preview`, which draws any
state's screen for a few seconds. `state` now only reads. Two components are added from the
ESP Component Registry and pinned: `esp_websocket_client` 1.8.0 and `mdns`
1.13.1.

**Seen on the breadboard, 2026-10-06.** USB-powered, open air, the server
on a computer on the same network.

- The device found the server, connected and reached `IDLE` 5 s after
  power-on.
- A server killed without warning was noticed within 30 ms. With the
  server gone the screen showed the loss, then "Server not found" once,
  then rested on `CONNECTING`. A restarted server was found again by
  itself, within the wait between attempts.
- A second connection under the device's identifier displaced it; it
  showed why and came back 61 s later.
- A server that did not accept its token put it in `SETUP` with "Server
  refused this device", where it stayed.
- Requests in a synthetic voice played from a computer, reaching the
  microphone at an average level of about -54 dBFS, were transcribed word
  for word: "What is two plus two?" and "Tell me in detail about the
  history of the Eiffel Tower." The voice came from a headset lying near
  the device, so how often the wake word answered it says nothing about
  the wake-word model, and is not recorded.
- Each upload began 10 ms behind the microphone, and no audio was lost.
- A reply of 33 s held at most 2.08 s of the buffer's 4 s and never ran
  dry.
- Memory: 100 KB of internal RAM free, 56 KB at its lowest. Chip
  temperature: 55.5 °C idle, 57.5 °C while speaking (R20).
- The wait for a reply, over five short questions in a synthetic voice:
  2.2 to 3.3 s from the server's `stop_capture` to the first sound, 2.8 s
  on average, of which the device's share is about 0.07 s. From the wake
  word, including about 3.5 s of speaking the question, 5.5 to 6.7 s. The
  budget for the whole wait is 1.3 to 2.3 s (R19).
- Spoken by a person: three requests in English were transcribed word for
  word and answered, with 2.5, 2.9 and 2.9 s from the end of the request
  to the first sound, and no audio lost. A request in Russian was not
  recognised, and the device returned to `IDLE` with no reply: the server
  is set to English.
- A run of nine spoken turns in four and a half minutes, by a person, the
  device freshly powered on. Eight were answered, with 2.4 to 3.2 s from
  the end of the request to the first sound, 2.7 s on average. The first
  failed on the server: its first request to the synthesiser timed out
  while connecting. The device showed "Speech synthesis failed" for 4 s
  and returned to `IDLE` with its connection intact, and the next request
  was answered. Through the run no audio was lost, no reply ran dry, the
  buffer held at most 2.1 s of its 4 s, the connection never dropped, and
  every detection of the wake word was acted on. Memory at the end:
  100 KB of internal RAM free, 56 KB at its lowest. Chip temperature:
  46.5 °C at the start, 52.5 °C at the end.
- A wake word followed by no request: the device listened for 5.8 s, the
  server answered with no reply, and the device returned to `IDLE`.
- With the chime, on 2026-10-07: five spoken turns by a person.
  The sound played each time, for 140 ms, and the request began 220 ms
  after the end of the wake word. Four were answered; in the fifth
  nothing was said, and the device returned to `IDLE` with no reply. No
  request audio was lost. The longest reply, 16.4 s at the full level,
  played to its end with no reset; it ran out of audio three times
  between sentences, for 0.4 s in all (KNOWN-ISSUES, Caveats). Memory:
  100 KB of internal RAM free, 56 KB at its lowest. Chip temperature:
  49.5 °C at the start, 53.5 °C during the long reply.
- After the review agents' corrections to the chime, on 2026-10-07: one
  spoken turn on the corrected firmware. The chime played for 140 ms, the
  request began 220 ms after the wake word, and the reply was heard. The
  log named each phase in turn: chime, capturing, awaiting, buffering,
  playing, draining, idle. Chip temperature: 55.5 °C, twelve minutes
  after power-on.
- With no server running, the screen's state read "connecting, Server not
  found on the network". The `display preview` command was accepted for
  two states and refused an unknown one; the panel itself was not looked
  at then.
- The screens, looked at on 2026-10-07: `display preview` drew each of the
  eight states in turn for 8 s, three of them with a detail line, with no
  server running. A person watched the panel and found every one as
  designed: title, detail line, picture and its movement, the dimmed
  backlight of `IDLE`, and the return to the real state's screen
  afterwards. Chip temperature at the end: 56.5 °C.

**Found and corrected in this stage.** By a first attempt at a spoken test: a send
that timed out after 200 ms ended the connection; the loss was announced
20 s late, with `IDLE` on the screen meanwhile; later lookups of the
server failed because its answers came without its address. By the bench
tests: a refused token was taken for an unreachable server and retried
for ever. By the review agents: the server counted a pause in a reply as
playback, which could send more than the device holds; the device's limit
on a silent reply was shorter than the server's limits in sequence;
several states depended on a single report that could be lost. The second
review also led to the protocol's numbers being gathered in one header
with a test against the server's, to reports reaching the state machine
directly instead of through the event loop, and to the state machine
keeping its own phases.

**Chip temperature.** From 2026-10-06 every test on the device records the
chip's temperature, read with the `temp` console command, with the
conditions it was taken under. The record is kept in R20 of
[KNOWN-ISSUES.md](KNOWN-ISSUES.md).

## Stages

| # | Stage | State | Exit criteria |
| --- | --- | --- | --- |
| 0 | Hardware bring-up | Done, with a deviation (see below) | Circuit assembled on breadboard, checked for shorts, and powered peripheral by peripheral. Supply and cable confirmed adequate, baseline current recorded, pinout corrected to match what was built. Procedure in [docs/BRINGUP.md](docs/BRINGUP.md). |
| 1 | Foundations | Done | ESP-IDF project skeleton, NVS-backed configuration for WiFi, server URL and device identity, logging. Device boots, joins WiFi and logs. |
| 2 | Audio I/O bring-up | Done | INMP441 capture on I2S0 and MAX98357A playback on I2S1. A three-second record-then-play loopback runs cleanly. Also the first point at which peak current can be measured under amplifier load, completing the record started in stage 0. |
| 3 | Display and state machine | Done | LVGL 9 driving the NV3007 panel, backlight under control, and the state animations driven by a mock state machine that cycles on a timer. No network. |
| 4 | Wake word | Done, with a deviation (see below) | microWakeWord integrated with a continuous ring buffer; detection drives the state transition. Custom phrase trained and thresholds tuned against a multi-hour false-accept run. |
| 5 | Server v1 | Done | WebSocket server, device authentication, day-scoped conversation store, and the speech-to-text, Claude and text-to-speech chain, with the reply synthesised sentence by sentence. Validated end to end by a desktop client script with no device involved. |
| 6 | Integration | Done | Device WebSocket client and mDNS lookup, streaming upload during capture, server-driven endpointing, streaming playback, real state machine, and error, timeout and reconnect paths, all as [docs/PROTOCOL.md](docs/PROTOCOL.md) describes. First end-to-end conversation. |
| 7 | Latency and robustness | In progress | Measured end-to-end latency against the budget, with the inexpensive ways of shortening it taken; the end-of-speech silence tuned; WiFi and server-drop recovery, watchdogs, brownout guard. The budget itself need not be met (see Stage 7 plan). |
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

Stage 4 closed on 2026-10-04 without the multi-hour false-accept run its
exit criteria name, and with neither target established. Detection was
observed, not established: the recorded speaker was detected in 7 of 7
tries at 1 m, which is too few tries, by the one speaker the model was
trained on, to show 90 %. False detections are not measured on the
device at all: the cutoff is the training notebook's starting value and
has not been tuned. The false-accept run is recommended before any
release, and after any further training run, since it is made per model.
The open items are in R1 of [KNOWN-ISSUES.md](KNOWN-ISSUES.md); how to
make the run is in [docs/WAKEWORD-TRAINING.md](docs/WAKEWORD-TRAINING.md).

Stage 5 was built without the device, and is testable without it. That
keeps a server defect from being mistaken for a firmware defect during
stage 6. Splitting the reply into sentences, first listed under stage 7,
was built in stage 5, because it shapes the reply pipeline; stage 7 keeps
the tuning.

## Environment

ESP-IDF v5.5.5 is installed and verified by a successful `hello_world` build
targeting `esp32s3`. No other ESP-IDF version remains on the development
machine.

The server runs on Python 3.11 in a virtual environment, `server/.venv`,
with the packages in `server/requirements.txt`. API keys for Anthropic and
Deepgram, a device token and the timezone are in the untracked
`server/.env`. For the trial of part C, Claude Code 2.1.296 is installed
on the same machine and signed in; its login is in the user's profile,
outside the repository.

## Next steps

1. The run of several hours, B6.
2. Decide what becomes of part C: merged, kept on its branch, or dropped.
   Until then `main` does not have it.
3. Work through the Before release list below once the last stage is
   done; nothing in it blocks stages 6 to 8.
4. Obtain a multimeter for the continuity checks before the circuit is
   soldered (R9).

## Before release

Once every stage is complete, a release version of the repository is
prepared. Work that was deferred to that point is listed here, so that it
comes up then.

| Item | From | Detail |
| --- | --- | --- |
| Multi-hour false-accept run, and the cutoff tuned with it | Stage 4 | Made on the model that is released, after any further training run. KNOWN-ISSUES R1; how to make it is in [docs/WAKEWORD-TRAINING.md](docs/WAKEWORD-TRAINING.md) |
| Counted detection test | Stage 4 | 10 tries at each distance, in a quiet room, with scores noted; by more than one speaker if others will use the device. KNOWN-ISSUES R1 |
| Failure paths not yet seen on the device | Stage 6 | The device's own deadlines (15 s of capture, 40 s of waiting, 40 s of a stalled reply) and a server speaking an incompatible protocol version. Written and reviewed, never triggered on the bench. To be done after stage 8 |
| WiFi lost and restored | Stage 7 | With the device idle, and a few seconds into a spoken reply: the router or its radio switched off and on. Expected: the device shows the loss, keeps trying, and returns to `IDLE` by itself. Put off because the WiFi could not be switched off on the day |
| A further training run, if wanted | Stage 4 | For other speakers, and for "hey cloud". The notebook's limits to deal with first are in KNOWN-ISSUES R16 |

## Open decisions

| Decision | Status |
| --- | --- |
| Speech provider | Deepgram for both directions, behind adapter interfaces; built in stage 5 |
| Day rollover | 04:00 local time, settled. Earlier days are deleted |
| Endpointing | Server-side, settled. The silence that ends a request stays at 400 ms: 1 s was tried in stage 7 and made the ending erratic (R18) |
| Interrupting a request | Not possible, settled: a turn runs to its end |
| After a failed turn | Settled: the device shows `ERROR` briefly and returns to `IDLE`; only a lost connection leads to `CONNECTING` |
| Reply audio pacing | Settled and built: the server sends at most 2 s ahead of playback and the device holds 4 s, whatever the length of the reply (R17) |
| WiFi power saving | Off, from stage 6: the device is mains-powered, and the radio's sleep delays streamed audio. It raises the current drawn and the chip's temperature somewhat; neither has been measured against power saving on (R20) |
| Playback volume | Reply audio is played as the server sends it, at 100 %, until volume control in stage 8; 40 % and 60 % were too quiet. If this is still too quiet, the next steps are the amplifier's gain pin, which is hardware, or boosting the audio in software with a limiter |
| Start-up buffer for a reply | Left at 80 ms, decided on 2026-10-07. A reply can pause briefly in its first second (KNOWN-ISSUES, Caveats); raising the buffer to about 300 ms would cover it at the cost of about 0.2 s more wait before every reply. The device now logs each pause; the buffer is to be raised if that log gathers more evidence |
| The chime and volume | Open, for stage 8. The chime has a level of its own, 30 % of full scale, and the playback level is applied on top of it, which changes nothing while that level is 100 %. With a volume control, the chime would fade with the volume while the device still waits for it before listening. To decide then: it follows the volume, follows it down to a floor, or keeps a fixed level |
| Reply voice | Settled: `aura-2-luna-en`. The first voice used had a harsh "s", in the audio itself and not from the device's speaker; this one was chosen by ear from eleven, and was heard on the device and preferred |
| Loudness | Open. With the chosen voice and playback at 100 %, replies are a little quiet. Four ways to raise it are listed in KNOWN-ISSUES under Caveats: the amplifier's gain pin, a boost in the firmware, levelling on the server, or another voice. To be decided if it proves a nuisance |
| Languages | English only: speech-to-text is set to English and the synthesiser's voice is English. Speech in another language is not recognised and the turn ends with no reply. Whether to support others is open |
| Seeing a screen state on demand | Settled and built: `display preview <state>` draws a state's screen for a few seconds without changing the state |
| A reason on the `CONNECTING` screen | Settled and built: `CONNECTING` shows why the last attempt to reach the server failed |
| Component names | Settled: the connection is `server_link` and the state machine `state_machine`, renamed before the first commit of stage 6 |
| Internet access for the assistant | Open, and optional: nothing is committed. The assistant has no tools and is told it has no internet access, so it declines questions such as the weather. If it is taken up, it belongs to stage 7. The ways considered: tools the server runs itself on free data (the date and time from its clock, weather from a free service), at a few hundred tokens a use; Anthropic's web search tool, at $10 per 1,000 searches plus the results as input tokens, estimated at about 4 cents a searched question on `claude-opus-5` and not measured; or both, with the search capped at one a question. Running Claude Code under a subscription login in place of the API was first set aside, because its documentation directs products built on it to API keys, and taken up again on 2026-10-08 as a trial, part C of stage 7: the device has one user, who is the account's owner. The API stays the default, and the trial is built on a branch of its own. Built and measured on 2026-10-12: with the trial's setting the assistant searches the web and reads pages, and answered the weather, the news and a price. Whether it is kept is open |
| Deployment | LAN only for now; off-LAN deployment deferred and would require TLS |
| Over-the-air updates | Undecided, revisited at stage 8; the flash layout already allows it |
| Server discovery | mDNS on the LAN, with `server_url` as an override; the server's side is built, the device's is stage 6 |
| Device settings entry | Serial console for now; stage 8 provisioning reuses the same validation |
| Locking dependency versions | Settled: `firmware/dependencies.lock` is committed, and the wake-word libraries are pinned exactly (R14) |
| Wake phrase | A two-syllable phrase first; a longer one if it cannot meet the R1 targets |
| `SETUP` and `ERROR` detail text | Settled: the device shows its own short wording, chosen by reason code and sized for the panel, for server errors and for those the server never sees. The sentence in the server's `error` is for logs. Built in stage 6 |
