# Operating the device and the server

Reference for flashing the device, entering its settings, its console
commands, running the server, the assistant's access and keeping Claude
Code up to date. For a first build, follow
[GETTING-STARTED.md](GETTING-STARTED.md) instead.

## Flashing the firmware

1. Open an ESP-IDF v5.5 shell from PowerShell. Not from Git Bash or
   another MSYS shell, nor a PowerShell started from one: ESP-IDF refuses
   to activate there.
2. Connect the board's USB-C port marked **UART**. The other, native USB,
   shows the log but ignores keystrokes.
3. In `firmware/`:

   ```sh
   idf.py -p PORT flash monitor
   ```

Settings are kept across reboots and reflashing.

## Settings

| Key | Meaning |
| --- | --- |
| `wifi_ssid` | Network to join. 2.4 GHz only; the ESP32-S3 has no 5 GHz radio |
| `wifi_pass` | 8-63 characters, or 64 hexadecimal digits. Unset for an open network |
| `device_token` | The device's password to the server: 24 to 64 characters. Set by pairing; or typed, the same value as its entry in the server's `DEVICE_TOKENS` |
| `pair_code` | A pairing code from `python -m server pair`, 6 digits. Kept only until the device trades it for a token |
| `server_url` | Optional. Empty: the server is found on the local network. Otherwise its address and port, as `192.168.1.20:8765` or `ws://192.168.1.20:8765` |
| `device_id` | Optional. Defaults to `hc-` and the last six hex digits of the MAC address. Letters, digits and `-` |

### From a phone (setup mode)

1. Open setup mode. A device without its settings starts in it by
   itself; otherwise hold the board's BOOT button for 3 s, or type
   `setup` on the console.
2. The screen shows a WiFi network's name and password, and a page
   address. Join that network from a phone.
3. The settings page usually opens by itself; if not, open the address
   on the screen.
4. Give the WiFi network and password, and a pairing code
   ([Pairing a device](#pairing-a-device)), or a token instead.
5. Save. The device restarts with the new settings, and pairs if a code
   was given.

The stored WiFi password and token are never shown: their fields are
empty, and left empty they stay as they are. A refused WiFi password,
pairing code or token brings the device back to setup mode by itself. Opened with the
button or the console, setup mode closes after 5 minutes if nothing is
saved; holding BOOT again closes it at once.

### From the console

```
config set wifi_ssid "Network name"
config set wifi_pass "password"
config show
reboot
```

Values with spaces go in double quotes; inside one, a backslash is `\\`
and a double quote `\"`. `config show` never prints the password or
token, and the console clears its line history after either is entered.
`config unset <key>` clears a setting. Changes take effect after
`reboot`.

## Console commands

| Command | What it does |
| --- | --- |
| `state` | The current state, with its reason if it has one |
| `link` | WiFi and server connection: up or not, where the server was found, why the last connection ended. Tells "cannot find the server" from "the server does not answer" |
| `pair 123456` | Store a pairing code and restart to pair with the server |
| `volume`, `volume 9` | Print the volume, or set it from 1 to 10, from the next reply on. Also changed by voice ("louder", "quieter", "volume five") and kept through a restart |
| `controls` | How the button and the switch stand, presses counted since boot, and whether continuous conversation is on |
| `temp` | Temperature inside the chip. About 56 °C is normal; a reading above 60 °C is worth noting (KNOWN-ISSUES R20) |
| `mem` | Free memory, and each task's unused stack |
| `wake` | Wake-word detections, rate per hour, scores, model timing |
| `wake reset` | Clear the counters, to start a measurement |
| `wake log on` | Print the highest score once a second |
| `wake cutoff 0.95` | Change the detection threshold until reboot |
| `audio loop 5` | Record 5 s, print the level, play it back boosted (with audible hiss) |
| `audio tone 20 3` | A 440 Hz tone at 20 % of full volume for 3 s. `audio tone 100` draws the most current the device ever needs: the test for a weak supply or cable |
| `display test 30` | A test pattern for 30 s, then frame timing |
| `display preview error "Speech synthesis failed" 10` | Draw a state's screen for 10 s without changing the state |
| `setup` | Restart into setup mode |
| `reboot` | Restart |

The display test draws a 1-pixel frame in a different colour on each
edge: red top, green bottom, blue left, white right. A missing edge means
the drawing offset is wrong; stray or wrongly coloured pixels mean the SPI
clock is too fast for the wiring.

**A false-accept measurement:** `wake reset`, then hours of background
sound with nobody saying the wake phrase, then `wake`. Every detection
counted is a false one.

## Using the device

- **Asking:** say "Hey Claude", wait for the two-note chime, then speak.
  The device takes the wake word only in `idle`, which it reaches only
  while connected to the server.
- **The button** interrupts a turn while the device listens, thinks or
  speaks, and the device returns to `idle`. The server keeps the request
  and the reply as far as it was sent, so a follow-up can refer to it.
  Speech does not interrupt.
- **The switch** turns on continuous conversation. Toward its connected
  leg, the device listens again after each reply, with the chime and
  without the wake word, and the screen shows "Continuous" in its top
  right corner. The conversation ends, back to `idle`, when nothing is
  said for 5 s, on the button, on an error, with the switch moved back,
  or after ten follow-ups in a row. A move during a reply takes effect at
  its end.

## Running the server

### Setting up

From the repository root, once:

```sh
python -m venv server/.venv
server/.venv/Scripts/python -m pip install -r server/requirements-dev.txt
```

On Linux and macOS the interpreter is `server/.venv/bin/python`.

Copy `server/.env.example` to `server/.env` and fill it in:

| Setting | Meaning |
| --- | --- |
| `DEEPGRAM_API_KEY` | Speech-to-text and text-to-speech. How to create it is in the file and in [SECRETS.md](../SECRETS.md) |
| `DEVICE_TOKENS` | Optional with pairing. Each device's identifier and token, as one JSON object, e.g. `{"hc-a1b2c3":"<token>"}`. The file shows how to generate a token. A paired device's token takes the place of its entry |
| `TIMEZONE` | An IANA name such as `Europe/Berlin`. It decides when the day's conversation ends, at 04:00 local time |

### Starting it

```sh
server/.venv/Scripts/python -m server
```

It listens on port 8765 and announces itself on the local network, so a
device finds it without being told its address.

### Pairing a device

1. With the server running or not, run:

   ```sh
   server/.venv/Scripts/python -m server pair
   ```

   It prints a 6-digit code that lasts 10 minutes, works once, and ends
   after 5 wrong tries. A new code replaces the last.
2. Enter the code on the device: in setup mode's page, or `pair <code>`
   on the console.
3. The device finds the server, trades the code for a token, restarts and
   connects. The server logs `device <id>: paired`.

Pairing a device again gives it a new token in place of the old one, or
of its `DEVICE_TOKENS` entry. `python -m server paired` lists the paired
devices, and `python -m server unpair <id>` withdraws one's token, for a
device lost or given away; it then connects only by pairing again.

### How Claude is reached

By default the server runs the Claude Code tool for each request, under
the login of whoever is signed in to it on the server's machine; no
Anthropic API key is needed. The assistant can search the web and read
pages, and an answer that does begins with "Let me check.". A web page can
mislead the assistant, and `CLAUDE_FETCH_DOMAINS` limits what it may read
(KNOWN-ISSUES R21).

With `LLM_BACKEND=api` the server calls Anthropic's API with
`ANTHROPIC_API_KEY` instead; the assistant then has no internet access and
says so.

### Giving Claude full access to the server's machine

The assistant is locked down to web search and reading pages, and no
setting lifts that. Full access, to run commands and read and write files,
is a change to the server's code, in `server/llm/`:

1. Before it, set `CLAUDE_FETCH_DOMAINS` to a short list or to empty, and
   make the false-accept run (WAKEWORD-TRAINING.md, Tuning the cutoff on
   the device). Both were accepted as they are only because the assistant
   cannot act (KNOWN-ISSUES R1, R21).
2. In `build_command`, remove `--tools` and `--allowedTools` and set
   `--permission-mode` to `bypassPermissions`.
3. In `lockdown_problem`, accept any tools and that mode, and keep the
   check that no MCP server is loaded, which keeps the account's mail and
   file connectors out.
4. In `system_prompt_with_search`, replace the sentence saying it has no
   way to act with what it may do on the machine, and ask it to say what
   it will change and wait for a yes.
5. Update the lock-down tests in `server/tests/test_claude_code.py`, then
   run the bench (Updating Claude Code).

The process still runs in an empty temporary directory, with only the
environment variables `child_environment` passes. A reply silent past the
limit (ARCHITECTURE.md) is killed with the command it is running, which
can leave a change half made.

The risks:

- **Whoever the microphone hears gives the commands:** a guest, a
  television, or a false wake. Only the prompt asks first; nothing
  enforces it, and a misheard "yes" counts.
- **A web page can give commands too.** Text written to mislead the
  assistant (R21) can now run programs on the machine.
- **The server's secrets become readable:** `server/.env`, with the API
  keys and device tokens, and the Claude Code login in the user's profile.
- **Anyone on the LAN with a device token** can send requests, over an
  unencrypted connection (R8).

### Testing without the device

The server's tests need no keys and no network:

```sh
server/.venv/Scripts/python -m pytest
```

The desktop client plays a recorded question to a running server, as the
device would. The recording must be a 16 kHz, mono, 16-bit WAV file; keep
it, and the replies, outside the repository.

```sh
server/.venv/Scripts/python -m server.tools.desktop_client question.wav --out-dir replies
```

It needs a `desktop-client` entry in `DEVICE_TOKENS`, prints how long each
stage of the turn took, and saves the reply as a WAV file. Several files
make several turns of one conversation; `--discover` finds the server by
mDNS, as a device does.

## Updating Claude Code

Claude Code updates itself in the background, and an update can change
the options or output the server relies on (KNOWN-ISSUES R21). A broken
update shows as every turn failing with `llm_failed`, the server's log
giving the tool's own complaint.

### After an update

1. `claude --version` gives the version installed; the server also logs
   it at its first turn.
2. From the repository root, run the bench, about a minute:

   ```sh
   server/.venv/Scripts/python -m server.tools.claude_cli_bench
   ```

   It puts fixed questions to the tool through the server's own client,
   with no device, speech service or API key.
3. The update is good when every question is answered, the plain ones'
   first text comes within about 3 s, and those that search open with
   "Let me check.". A run stopped by the lock-down check names the tools
   the process should not have had.
4. Record a version that passes in the Claude Code row of
   [CLAUDE.md](../CLAUDE.md), in STATUS.md's Environment, and in the
   range of versions benched in KNOWN-ISSUES R21.

### If it fails

1. Go back to the last version that passed, the one in CLAUDE.md's
   Claude Code row:

   ```sh
   claude install <version>
   claude --version
   ```

2. Stop it updating itself, by adding to `~/.claude/settings.json`:

   ```json
   {
     "env": {
       "DISABLE_AUTOUPDATER": "1"
     }
   }
   ```

   `claude doctor` then shows auto-updates as disabled. This holds Claude
   Code for everything else on that login too.
3. Once the server suits the new version, remove the line.

### Updating on purpose

Run `claude update`, then the bench as above.
