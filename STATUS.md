# Status

**Current stage:** none. All eight stages are done, and the device works
end to end on the breadboard. One item remains before release: the
multi-hour false-accept run (Before release, below).

**Last updated:** 2026-10-26

The history of each stage, with its step tables, bench sessions and review
findings, is in the git history of this file.

## Where the project stands

The hardware is assembled on a breadboard: an ESP32-S3 board with 8 MB of
PSRAM, an INMP441 microphone, a MAX98357A amplifier and speaker, an NV3007
panel, and a button and a slide switch. Every peripheral was exercised by
a test program, kept in `firmware/tools/bringup/`, and the pinout matched
the plan.

The firmware runs a whole conversation:

- It detects "Hey Claude" on the device with a microWakeWord model,
  `hey_claude_run3`, trained in Google Colab with recordings of one
  speaker ([docs/WAKEWORD-TRAINING.md](docs/WAKEWORD-TRAINING.md)). No
  audio leaves the device before the wake word.
- It plays a chime, streams the request to the server, plays the reply
  as it arrives, and shows each state on the panel with a placeholder
  animation.
- The volume is changed by voice ("louder", "volume five"), in ten levels.
- The button interrupts a turn while the device listens, thinks or
  speaks. With the switch closed, the device listens again after each
  reply, without the wake word, up to ten follow-ups in a row.
- Settings are entered through the serial console, or from a phone
  through a hotspot and a page the device serves (setup mode).
- It recovers by itself from a lost server, lost WiFi and a hung task,
  and stops on `ERROR` rather than looping when its supply is too weak.

The server, `python -m server`, takes each request through Deepgram
speech-to-text, Claude and Deepgram text-to-speech, and sends the reply
back sentence by sentence as it is generated. It decides when the speaker
has finished, keeps each device's conversation for the current day, and
advertises itself on the LAN by mDNS. By default it reaches Claude through
the Claude Code tool, which lets the assistant search the web
([ARCHITECTURE.md](ARCHITECTURE.md), Claude through Claude Code); the API,
with no web access, is one setting away. Its own tests run the whole path
with stand-ins for the three services and need no keys or network. A
desktop client, `server/tools/desktop_client.py`, plays recorded requests
to it as the device does.

The design is in [ARCHITECTURE.md](ARCHITECTURE.md), the protocol in
[docs/PROTOCOL.md](docs/PROTOCOL.md), and the open risks and caveats in
[KNOWN-ISSUES.md](KNOWN-ISSUES.md).

## Measured on the device

On the breadboard, USB-powered, open air, the server on a computer on the
same LAN with a home internet connection, a person speaking, from
2026-10-06 to 2026-10-26.

- **Wait for a reply**, from the end of the request to the first sound:
  about 2 to 5 s for a plain answer, once 12 s; a reply that searches the
  web says "Let me check." after 2.6 to 6.3 s, falls silent for 1.5 to
  11.6 s, then answers. The budget of 1.3 to 2.3 s is not met, and that is
  accepted (R19). Claude's time to its first text is nearly all of the
  wait and all of its spread.
- **Chime to request:** the request begins 220 ms after the wake word,
  and a follow-up's 210 to 225 ms after the reply ends.
- **Stability:** over a run of 3 h 5 min with twelve turns, no reset, no
  lost connection and no audio lost from any request; internal RAM free
  stayed at about 100 KB with no trend, about 56 KB at its lowest.
- **Recovery:** a server killed at any point of a turn was noticed within
  about 30 ms, and the device was back in `IDLE` within 2 to 3 s of the
  server's return. With the router switched off for 3.5 min, the device
  was back in `IDLE` within a second of rejoining the WiFi.
- **The device's own deadlines**, set off on 2026-10-26 against a
  stand-in server that misbehaved on purpose: capture ended at 15.0 s, a
  wait for the reply at 40.0 s, and a stalled reply 40.0 s after its last
  audio, each ending in `ERROR` and then `IDLE`. A server stating another
  protocol version was refused, with "Server and device versions differ".
- **Continuous conversation** ended in `IDLE` after ten follow-ups in a
  row, as designed (2026-10-26).
- **Interrupting:** the button stopped a turn and showed `IDLE` within
  the millisecond of the press; the server's `reply_end` followed 30 to
  210 ms later.
- **Supply:** about 0.58 A at 4.82 V for a full-scale tone and 0.2 A
  through a spoken reply, with no reset (R2).
- **Chip temperature:** about 56 °C idle and 56.5 °C in conversation
  (R20).

## Stages

| # | Stage | Outcome |
| --- | --- | --- |
| 0 | Hardware bring-up | Done. Circuit assembled and each peripheral powered in turn; baseline 0.10-0.14 A with the rail at 4.93 V or above ([docs/BRINGUP.md](docs/BRINGUP.md)). Continuity checks not made, for want of a multimeter (R9) |
| 1 | Foundations | Done. ESP-IDF project, settings in NVS, WiFi with reconnection, logging |
| 2 | Audio I/O bring-up | Done. Capture on I2S0, playback on I2S1, a record-then-play loopback |
| 3 | Display | Done. LVGL 9 on the NV3007; a full frame in 12.5 ms at 80 MHz (R3) |
| 4 | Wake word | Done, with a deviation: closed without the multi-hour false-accept run, so the cutoff is the notebook's starting value (R1, Before release) |
| 5 | Server v1 | Done. Built and tested without the device |
| 6 | Integration | Done. The device's connection, upload, playback, state machine and failure paths, as [docs/PROTOCOL.md](docs/PROTOCOL.md) describes |
| 7 | Latency and robustness | Done. The inexpensive ways of shortening the wait were taken and the budget is not met (R19); watchdogs and the brownout guard; Claude reached through Claude Code by default |
| 8 | Polish | Done. Volume by voice, setup mode, interrupting with the button, continuous conversation. The animations stay as they are; over-the-air updates are not adopted |

## Before release

| Item | Detail |
| --- | --- |
| Multi-hour false-accept run, and the cutoff tuned with it | Open. Made on run 3's model, the one released. How to make it is in [docs/WAKEWORD-TRAINING.md](docs/WAKEWORD-TRAINING.md); the targets are in R1 |

Done on the way: a counted detection test, by the speaker the model was
trained on, which worked well (its scores were not kept); the device's own
deadlines and an incompatible version; WiFi lost and restored; the cap on
continuous conversation. Not wanted: a further training run, and WiFi lost
during a reply, which the server-drop tests already cover.

## Next steps

1. The multi-hour false-accept run (Before release).
2. Obtain a multimeter for the continuity checks before the circuit is
   soldered (R9).
3. Bench the Claude Code version now installed (Environment;
   docs/OPERATING.md, Updating Claude Code).

## Environment

ESP-IDF v5.5.5 is installed and verified; no other ESP-IDF version
remains on the development machine.

The server runs on Python 3.11 in a virtual environment, `server/.venv`,
with the packages in `server/requirements.txt`. API keys for Anthropic and
Deepgram, a device token and the timezone are in the untracked
`server/.env`. Claude Code is installed on the same machine and signed in;
its login is in the user's profile, outside the repository. Version
2.1.303 was the last benched. The tool has since updated itself to
2.1.310, which ran the device tests of 2026-10-26 without fault but has
not been benched.

## Decisions

| Decision | State |
| --- | --- |
| Speech provider | Deepgram for both directions, behind adapter interfaces |
| Reply voice | `aura-2-luna-en`, chosen by ear from eleven for a clean "s" |
| Languages | English only. Speech in another language is not recognised and the turn ends with no reply. **Open:** whether to support others |
| Endpointing | On the server. The silence that ends a request stays at 400 ms; 1 s made the ending erratic (R18) |
| Day rollover | 04:00 local time; earlier days are deleted |
| Interrupting | The button only, while listening, thinking or speaking; speech does not interrupt. An interrupted turn stays in the conversation, marked as interrupted |
| Continuous conversation | Chosen with the switch, up to ten follow-ups in a row; the screen marks the mode |
| After a failed turn | `ERROR` briefly, then `IDLE`; only a lost connection leads to `CONNECTING` |
| Reply audio pacing | The server sends at most 2 s ahead of playback; the device holds 4 s (R17) |
| Start-up buffer for a reply | 80 ms. A reply can pause briefly in its first second (Caveats); about 300 ms would cover it at the cost of 0.2 s more wait before every reply. **Open:** to be raised if the device's pause log gathers more evidence |
| Playback volume | Ten levels 3 dB apart; level 8, the default, plays the audio as sent, the top two add 3 and 6 dB with a limiter. The chime follows it down to -9 dB |
| WiFi power saving | Off: the device is mains-powered, and the radio's sleep delays streamed audio |
| Internet access for the assistant | Through Claude Code, the default: web search and reading any page (R21). With the API it has no tools and says it has no internet access |
| Device settings entry | The serial console, and setup mode's page, through the same checks |
| Server discovery | mDNS on the LAN, with `server_url` as an override |
| `SETUP` and `ERROR` detail text | The device's own short wording, chosen by reason code; the server's `error` sentence is for logs |
| State animations | The placeholder animations stay |
| Deployment | LAN only; off-LAN would require TLS (R8) |
| Over-the-air updates | Not adopted; the device is flashed over USB. The flash layout keeps two application slots, so adopting them later needs no change to it |
| Encrypting the stored settings | Not done: on this chip it burns eFuses, and the board is to stay reusable (R10) |
| Dependency versions | `firmware/dependencies.lock` is committed, and the wake-word libraries are pinned exactly |
| Wake phrase | "Hey Claude", two syllables, kept with run 3's model |
