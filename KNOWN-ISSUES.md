# Known issues

Open risks, defects and caveats, as they stand. How each was found and
tried is in the git history of this file.

**Confirmed defects:** none.

## Open risks

### R1 — Wake-word accuracy

A custom wake phrase trained with microWakeWord will not match the
accuracy of a commercial assistant trained on real recordings, and the
balance between false accepts and false rejects shifts with room
acoustics and distance. Espressif's own engine cannot supply a custom
phrase without a paid corpus of more than 500 speakers, so there is no
drop-in alternative.

The targets: at most 0.5 false detections per hour against continuous
speech-heavy background sound (TV, podcasts), and at least 90 % detection
at 1 m in a quiet room.

The released model is run 3, trained with recordings of one speaker. Its
standing:

- **Detection.** That speaker was detected in nearly every try at 0.5, 1
  and 2 m, and a later counted test, 10 tries at each distance in a quiet
  room, worked well; its scores were not kept, so the figure is
  unrecorded and holds for that speaker only.
- **False detections** have not been measured on the device. The cutoff
  is the training notebook's starting value, released without the
  multi-hour false-accept run (STATUS.md, Decisions), which stays
  optional
  ([docs/WAKEWORD-TRAINING.md](docs/WAKEWORD-TRAINING.md), Tuning the
  cutoff on the device).
- **Other speakers** should expect more misses: the model is specialised
  to the recorded voice. Strong accents were already missed more often by
  the stock model.
- **"Hey cloud" can trigger.** It scores above any usable cutoff, so the
  remedy would be recordings of it in a further run, not a higher cutoff.

No further run is planned. Each run's results are in the run history of
[docs/WAKEWORD-TRAINING.md](docs/WAKEWORD-TRAINING.md).

### R2 — Brownout under amplifier load

Peak draw can reach about 1.4 A at 5 V when amplifier transients into the
4 Ω load coincide with WiFi transmit bursts. A 500 mA USB port will reset
the board during loud playback, which presents as a reboot mid-sentence
and is easily mistaken for a firmware fault.

Measured with a USB power meter, on the breadboard, on WiFi: a full-scale
tone read about 0.58 A at 4.82 V, and a spoken reply at the full level
about 0.2 A at 4.92 V, both with no reset on a PC USB 3.0 port. The meter
shows averages, so the true peak is unknown (R9).

*Mitigation:* a supply of 1 A or more; 1000 µF at the amplifier's supply
pin; the amplifier fed from 5 V. If resets persist, the supply is the
first suspect. At the third brownout in a row, each within 60 s of the
boot before it, the firmware stops before joining WiFi and rests on
`ERROR`, "Power supply too weak" ([ARCHITECTURE.md](ARCHITECTURE.md),
Recovering by itself); this was tested with simulated brownouts only. A
breadboard's contacts can themselves cause the drop, so peak figures are
to be measured again once the circuit is soldered.

### R3 — Display refresh rate ceiling

A full-screen refresh moves 121,552 bytes and takes 12.5 ms at 80 MHz,
the highest SPI clock the chip supports (measured with `display test`):
about 80 full frames per second at most, before any drawing.

*Mitigation:* animations redraw a partial region, 142 x 142 pixels in
about 4 ms. This constrains the visual design and cannot be optimised
away.

### R6 — Three external services in the latency path

Speech-to-text, Claude and text-to-speech each add latency and each can
fail on its own.

*Mitigation:* adapter interfaces on both speech services; each service's
failure ends the turn with its own error code, and each wait has a limit
([docs/PROTOCOL.md](docs/PROTOCOL.md)). These paths are tested with
stand-ins; none has been seen with a real outage. A connection to the
synthesiser gets a second try within the same 5 s, and a sentence whose
request fails before any audio arrives is tried once more.

### R7 — Stale ESP-IDF environment variables

Installing or removing an ESP-IDF version leaves persisted user
environment variables behind, which break the next install and present
as problems with the new version. Seen in practice: a removed
installation's `IDF_PYTHON_ENV_PATH` made a later `install.bat` abort
with a version mismatch, and its Python directories stayed on the user
`PATH`. Shells of the MSYS family, such as Git Bash, set `MSYSTEM`, and
ESP-IDF refuses to activate when it is present, including in a PowerShell
started from such a shell.

*Mitigation:* when changing ESP-IDF versions, check `IDF_PATH`,
`IDF_TOOLS_PATH` and `IDF_PYTHON_ENV_PATH` in the persisted user
environment, not only in the current shell; run installers from a shell
with no ESP-IDF environment active; remove orphaned toolchains with
`idf_tools.py uninstall`; run ESP-IDF from a PowerShell not started from
an MSYS shell.

### R8 — Unencrypted transport on the LAN

Device tokens are shared secrets sent over plain WebSocket, and anything
on the local network can read conversation audio in both directions.
Pairing sends the code and the new token once over plain HTTP. The
server keeps `DEVICE_TOKENS` in plain text in its untracked `.env`,
beside the API keys; paired tokens it keeps only as hashes. Anything on
the network can also end a pairing code with five wrong tries, since the
pairing request needs no token; the device then shows "Pairing code
refused", and a new code is made.

*Mitigation:* accepted for a LAN-only server. Moving it off the LAN
requires TLS and a stronger device credential.

### R9 — Playback peak known only as an average, continuity not checked

- The USB power meter shows averages refreshed a few times a second, so
  it cannot catch a transient of about 10 ms, and it reads voltage before
  the cable rather than at the board. The board not resetting is the
  decisive result for this supply.
- No continuity or short checks were made, for want of a multimeter. The
  working peripherals show the connections are sound, not that none is
  marginal.

*Mitigation:* a reset during loud playback is put down to the supply
first. Repeat `audio tone 100` after soldering and after any change of
supply or cable. Obtain a multimeter before the circuit is soldered.

### R10 — Device credentials stored unencrypted

The WiFi password and the device token are stored in NVS in plain text.
Anyone with the device and a USB cable can read them out of flash.
Neither is in the source or the firmware image. The token is not an API
key: someone holding it could use the server as the device, but not read
the server's keys.

*Mitigation:* accepted for good. NVS and flash encryption on the ESP32-S3
burn eFuses, which cannot be reset, and the board is to stay reusable. A
device that is lost is cut off with `python -m server unpair <id>`, and
a leaked token is replaced by pairing the device again.

### R13 — The display tears during fast movement

The panel has no tearing-effect output, so a frame can arrive while the
panel is part-way through showing the previous one. The `display test`
bar, which moves across the full height, shows a step; the state
animations, small and redrawn in about 4 ms, show no visible tearing.

*Mitigation:* keep animations small and avoid fast horizontal movement of
tall shapes.

### R15 — Microphone overflow is not counted

The ring buffer counts audio a listener missed by falling more than 2 s
behind. It cannot see a loss one step earlier: if the capture task were
ever more than 75 ms late reading the microphone, the I2S driver would
drop the oldest audio without any count. "No audio lost" in this
repository covers the ring, not the whole path from the microphone.

*Mitigation:* the capture task runs at the highest audio priority on
core 1. If skips are ever heard, the driver's receive-overflow callback
(`on_recv_q_ovf`) can count them.

### R16 — Limits of the training notebook

Version 2 of the notebook (`hey_claude_v2.ipynb`), which trained run 3,
is safe to rerun with changed inputs. Its remaining limits matter only to
a further run, which is not planned:

- Only microWakeWord, the sample generator and two audio libraries are
  pinned; each run records the full list of packages, from which a later
  run can pin.
- After a disconnect, training resumes from the last weights but restarts
  its step count and its record of the best checkpoint (inside
  microWakeWord).
- Checkpoints are chosen by recall on synthetic samples, which does not
  predict the device. The real-voice test reports on the chosen model but
  does not choose it, and is itself only a loose guide: its recordings
  are not made with the device's microphone, and its feature front end is
  microWakeWord's build, not the firmware's. The device test is the final
  word.
- Speech in a recording is found by level alone, at least `above_db`
  (9 dB) over the recording's quietest tenth; a recording with a loud
  background can be dropped as holding no speech.
- The real-voice test may score each step one count low, by at most
  0.4 %, if the model's output is the raw value divided by 256. Not
  confirmed.
- The test set's fingerprint includes every converted test folder on the
  session's disk, so a folder removed during a session can give the same
  set two fingerprints.
- The recordings' `share` adds to the synthetic examples rather than
  replacing part of them, and is drawn from all speakers together.
- The manifest's cutoff is meant to be adjusted by hand after tuning;
  nothing in the file says which value it holds, and downloading the
  results again replaces it.
- The code that accepts version 1's samples can be removed.

### R17 — Reply pacing rests on an assumption of playback

The server sends at most 2 s of reply audio ahead of playback, and the
device holds 4 s (384 KB of PSRAM). The server has no report of what the
device has played: it assumes playback starts with the first chunk and
never pauses, and the device's second 2 s covers a late start or a stall.
A network stall longer than that is not recovered from within a reply:
the device plays silence while the server's count runs on, and beyond the
margin the device abandons the turn. On the device the buffer has held at
most about 2.1 s of its 4 s.

*Mitigation:* the device logs the most held and every pause for each
reply. A report of playback from the device, in a later protocol version,
would close the risk if the logs call for it.

### R18 — A pause between sentences ends the request

The end of a request is taken from 400 ms of silence after speech, so a
speaker who pauses that long between two sentences is cut off after the
first. A longer setting is not a remedy: at 1 s the service's silence
signal was not dependable on the microphone's audio, and requests ended 1
to 4 s after the speaker stopped. Speech-to-text can also report words and
then withdraw them, ending the request with nothing heard.

*Mitigation:* none in place. What would work: the server measuring the
silence itself from the word timings the service reports. The silence is
the `ENDPOINTING_MS` setting; the service's events can be logged, without
their words, with the `server.stt.deepgram` logger at DEBUG.

### R19 — The wait for a reply is over budget

The budget in [ARCHITECTURE.md](ARCHITECTURE.md) is 1.3 to 2.3 s from the
end of speech to the first sound. On the device a plain answer comes after
about 2 to 5 s, and once after 12 s. Claude's time to its first text,
measured at 0.9 to 4.5 s and once 10.7 s, is nearly all of the wait and
all of its spread; the device adds about 0.07 s. The wait was judged
acceptable in use, so the budget is not met.

*Mitigation:* none further planned. The server logs its timings for every
turn and the device each change of phase. Levers left: starting Claude on
an early transcript, synthesising the first clause instead of the first
sentence, and announcing a request at the wake word so that speech-to-text
connects while the chime plays. Keeping a connection to the synthesiser
warm was measured and gained nothing.

### R20 — Chip temperature

The ESP32-S3 runs warm in this firmware: wake-word detection keeps the
processor busy, and the radio never sleeps, WiFi power saving being off.
The chip's own sensor, read with the `temp` console command, measures the
silicon, which is hotter than the surface. The chip is rated for
surrounding air up to 85 °C.

Measured from 2026-10-06 to 2026-10-26 on the breadboard, USB-powered, in
open air, over some 300 readings:

| Condition | Average inside the chip |
| --- | --- |
| Idle and connected, on for 20 min or more | 56 °C |
| In conversation | 56.5 °C |

The highest reading was 57.5 °C. From a cold start the chip takes about
20 minutes to reach the idle figure.

**Only spikes are recorded from now on:** a reading above 60 °C, with
what the device was doing. Normal operation stays below that.

Not measured: inside an enclosure, which will be warmer than open air,
and with WiFi power saving on.

*Mitigation:* none needed. If readings approach 70 °C, the first things
to try are WiFi power saving back on and a lower processor speed outside
conversations. An enclosure is to be designed with a reading in hand.

### R21 — Claude through Claude Code, and the open web

By default (`LLM_BACKEND=claude_code`) the assistant is the Claude Code
tool running under the server owner's subscription login, and it can
search the web and read pages ([ARCHITECTURE.md](ARCHITECTURE.md), Claude
through Claude Code). `LLM_BACKEND=api` chooses the API, with no web
access.

**A page can mislead the assistant.** Text on a page or in a search
result can be written to be taken for instructions. The assistant cannot
run a command, touch a file or reach the account's connectors, and holds
no credential, so such text can bring about three things at most: a wrong
answer spoken with confidence; the day's conversation sent out inside an
address it is told to open; and a page on the local network being read.
Accepted: reading any page is the default, because search alone did not
answer the weather, and a short list of sites answered the weather but
not the news.

*Mitigation:* `CLAUDE_FETCH_DOMAINS` restricts reading to named hosts, or
turns it off with an empty list, which closes the second and third. The
check made on every run covers the tools, MCP servers and permission
mode, not the permission rules themselves. The acceptance assumes this
lock-down; lifting it is in [docs/OPERATING.md](docs/OPERATING.md), Giving
Claude full access to the server's machine.

**The tool updates itself.** The client relies on about fifteen of its
options and on the shape of the events it prints. An update can break
either, and every turn then fails with `llm_failed`; the tests use a copy
of the old shape and cannot notice.

*Mitigation:* run `python -m server.tools.claude_cli_bench` after an
update; it takes a minute. The server logs the tool's version at the first
turn. The versions benched, from 2.1.296 to 2.1.312, showed no change.

**A subscription has usage limits**, shared with the account's other use.
When one is reached every turn fails until it resets. Not seen. There is
no falling back to the API.

**A searched reply that stays silent for 24 s is given up**, with
`llm_failed`. The device hears nothing while the assistant searches. On
the device the silence has been 1.5 to 11.6 s; the longest replies on the
bench took up to 26.5 s in all, with several look-ups. The prompt asks
for at most two look-ups. None has yet been given up.

**Answers from the web are only as good as what was found.** Seen: a
race result reported as unconfirmed or given to the wrong driver, a price
hedged across a wide range, and news not answered when no site with the
day's stories could be read.

**A line that opens with "Sources:" is removed** with the rest of that
part of the reply, to drop the list of sources the search tool asks for.
The server counts removals (`sources_dropped`); none has come since the
prompt forbade the list.

Measured on the server's machine with the bench script, Claude Code
2.1.312, `claude-opus-5` at low effort, a home internet connection, one
run of five plain questions and five that searched or read a page: the
first text of a plain answer after 1.56 to 1.86 s, median 1.69 s, the
whole reply in 3.2 to 3.8 s; a reply that searched or read a page whole
in 7.4 to 24.6 s. Not measured: a usage limit, and the wait after more than an hour unused.

## Resolved

- **R4 — Provisional pinout.** Verified in stage 0; it matched the plan
  ([docs/BRINGUP.md](docs/BRINGUP.md)).
- **R5 — Display power.** `VDD` at 3.3 V; the backlight is switched by an
  on-board transistor and driven from a GPIO.
- **R11 — Backlight pin floating.** Driven low in `display_init()` within
  about a second of reset, lit after the first frame.
- **R12 — Speech arrives quietly from the microphone.** About -52 dBFS at
  0.5-1 m, with the top 16 of 24 bits kept (`CAPTURE_SHIFT`). It does not
  matter: the wake word detects reliably, and requests at about -54 dBFS
  were transcribed word for word.
- **R14 — LVGL's version not locked.** `firmware/dependencies.lock` is
  committed.
- **A request ending in a number waited about 5 s longer.** Speech-to-text
  used `smart_format`, which holds back the end of speech after a number;
  `punctuate` is used instead. Turning `smart_format` back on would bring
  the wait back.
- **Log redaction broke formatted log lines**, and **a rejected
  `DEVICE_TOKENS` setting printed token values.** Redaction runs on the
  finished line; configuration errors quote no value.
- **A device deaf from its first second could show `IDLE`.** The state
  machine asks the wake-word component whether the device can hear.
- **A reply's summary line could count one pause as two.** Corrected in
  stage 7; earlier logs over-count.

## Caveats

**Using the device**

- **Speak after the chime.** Listening starts when the two notes end,
  220 ms after the wake word; words before that are not sent.
- **The wake word is needed for every request unless the switch is
  closed.** Closed, the device listens again after each reply, up to ten
  follow-ups in a row.
- **Continuous conversation can be kept going by speech not meant for the
  device**, such as a television or people talking. It ends when nothing
  is said for 5 s, and after ten follow-ups at most. Not tried with a
  television on.
- **Only the button interrupts.** A wake word or speech during a reply
  does nothing.
- **English only.** A request in another language is not recognised, and
  the device returns to idle with no reply.
- **A volume request in other words goes to Claude**, which says it cannot
  change the volume. Phrasings found missing are added to
  `server/pipeline/volume.py`.
- **A pairing answer lost on the way** leaves the server with the new
  token and the device without it: the device tries again, is refused,
  and shows "Pairing code refused", and a device that was paired before
  has lost its old token too. Pairing again with a new code mends it.
- **The wake word works only with a server**, since the device takes it
  only in `IDLE`, which it reaches only while connected.
- **The assistant knows the date but not the time** through Claude Code;
  through the API it knows neither, the system prompt being kept free of
  anything that varies, for prompt caching.

**Conversation and data**

- **Conversation resets at 04:00 local time.** Earlier days are deleted
  when the first exchange of a new day is stored, and deleted rows are not
  overwritten in the database file.
- **Only completed and interrupted turns are remembered.** A failed or
  declined request is not part of the conversation. An interrupted one is
  kept as far as it was sent, which may be up to 2 s more than was heard.
- **Captured audio and transcripts are personal data.** They are sent to
  third-party services and kept out of version control.

**Playback**

- **A reply can pause briefly in its first second**, and in the first
  second after a search's silence: the player starts again from an empty
  buffer with 80 ms in hand. Seen in about one reply in twenty without a
  search, for 10 to 240 ms, and in most replies after a search, for up to
  110 ms. The cause is not found: the server sends the audio on time, so
  it reaches the player late, by WiFi or the device's receiving. A larger
  start-up buffer would cover it (STATUS.md, Decisions). When a reply is
  stopped part-way, its summary line reports "most held at once 0 ms".
- **The speaker shakes the breadboard, and that can be heard** as a
  robotic voice in long replies. Lifting the breadboard clears it; to be
  dealt with when the speaker is mounted apart from the electronics.
- **If level 10 proves too quiet**, the ways left are: the amplifier's
  `GAIN` pin, unconnected (9 dB), tied to ground (12 dB) or to ground
  through 100 kΩ (15 dB), after which `audio tone 100` is repeated as the
  supply check (R2); levelling each reply on the server; or another voice,
  such as `aura-2-andromeda-en`, about 3 dB louder.

**Setup and console**

- **Settings apply after a reboot.** `config set` changes what is stored,
  not what is running.
- **The console echoes what is typed**, including a password entered with
  `config set`, though not into its line history. It accepts input only
  on the UART port; the native USB port shows the log only.
- **No state can be set from the console.** `display preview <state>`
  draws a state's screen for a few seconds without changing it.
- **The console's audio tests and a reply share the amplifier.**
- **`audio loop` playback includes hiss**, from boosting quiet recordings
  by up to 36 dB for the test.
- **The setup page travels unencrypted over the device's own hotspot**,
  protected only by the hotspot's WPA2, whose password is on the screen.
  Accepted, like R8.
- **Some phones do not open the setup page by themselves.** The address
  is always on the screen, and typing it works. Possible causes, not
  checked: Android's "Private DNS", or a phone that reports "no internet"
  instead of offering a sign-in page.
- **Setup mode for a network that is away can wait a long time.** WiFi
  settings that have never connected are given up after 10 minutes, and
  the device then waits in setup mode with no deadline.
- **The `temp` and `mem` commands are diagnostic aids** in files of their
  own, `firmware/main/temp_cmd.c` and `mem_cmd.c`, each saying what to
  delete to remove it.

**Firmware**

- **A send to the server can block for several seconds on a stalled
  network**, since the WebSocket client applies its timeout more than
  once. A request that loses audio that way is given up with "Could not
  send the request".
- **Nothing in the firmware can be stopped once started.** Setup mode is
  entered by restarting into it.
- **The watchdog and the brownout guard were tested with a test build**,
  not kept. Not tested: the capture and player tasks hung, and a real
  brownout. A watchdog restart loses the turn in progress.
- **The microphone is not usable for about 2 s after boot**, and **the
  screen is dark for about 2 s after reset.** About one second also passes
  between the boot banner and WiFi start, for a reason not investigated.
- **The board's RGB LED can light up by itself**, after several restarts
  in a row, and stays lit until power is removed. The firmware never
  drives its pin (likely GPIO 48, not confirmed); sending it "off" at
  every start is undecided.
- **Speech synthesis failed once, after an interrupted turn**, with
  `RemoteProtocolError`, and could not be reproduced in 41 tries. The
  likeliest cause is covered by the retry described under R6.

**Server**

- **The desktop client is not the device.** It sends audio from the
  request's start and takes the reply as fast as it comes.
- **The server's dependencies are pinned one level deep.**
- **The server needs the `tzdata` package on Windows**; it is in the
  requirements.
