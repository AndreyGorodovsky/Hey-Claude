# Project rules

Read [ARCHITECTURE.md](ARCHITECTURE.md) before proposing design changes and
[STATUS.md](STATUS.md) before starting work. This file holds only what those
documents do not.

## Workflow

Work proceeds in numbered stages listed in [STATUS.md](STATUS.md).

1. **Plan before acting.** Break the stage into steps and get them approved.
   No implementation begins until the steps are accepted.
2. **Implement** the approved steps only. Anything discovered mid-stage that
   falls outside them is recorded in [KNOWN-ISSUES.md](KNOWN-ISSUES.md), not
   silently fixed.
3. **Review agents**, in this order, after every stage or major feature:
   1. `code-reviewer` — correctness, memory safety, task and DMA discipline,
      error paths, resource leaks.
   2. `challenger` — reads the code cold and argues against anything that is
      not clean: awkward abstractions, duplicated state, decisions that will
      hurt later.
4. **Apply corrections** arising from both agents.
5. **Documentation pass.** Bring all five documents to the current state.
   Stale documentation is a stage failure, not a follow-up task.
6. **Commit.** Push only when explicitly asked.

## Hard constraints

- **No secrets in the repository.** Run the [SECRETS.md](SECRETS.md) checklist
  before every commit. API keys live on the server only, never in firmware.
- **No audio, transcripts or conversation logs in the repository.** They are
  personal data.
- **The device stays stateless.** No conversation history, no credentials
  beyond its own identity.
- **GPIO 26-37 are unusable**; they belong to the flash and the in-package
  PSRAM of the board's ESP32-S3R8. GPIO 0, 3, 45 and 46 are strapping pins; GPIO 19 and 20 carry native
  USB.
- **Do not disable thinking on `claude-opus-5`.** Lower effort instead.

## Pinned versions

| Component | Version | Note |
| --- | --- | --- |
| ESP-IDF | v5.5.x | Not v5.3, which predates the NV3007 and LVGL reference work. Not v6.x, which is a beta or an untested major. |
| LVGL | 9.4+ | Ships the NV3007 driver |
| Python | 3.11+ | |
| Claude model | `claude-opus-5` | |

Confirm `IDF_PATH` points at a v5.5.x checkout before building. Run ESP-IDF
from PowerShell, never from Git Bash (`MSYSTEM` blocks activation). The board's
console is UART0, on the CH340 port. When changing
ESP-IDF versions, also check `IDF_TOOLS_PATH` and `IDF_PYTHON_ENV_PATH` in the
persisted user environment, not only in the current shell — see R7 in
[KNOWN-ISSUES.md](KNOWN-ISSUES.md).

## Conventions

- Documentation is written for a public audience in general language, never
  addressed to a particular reader.
- Firmware follows ESP-IDF style; server code follows PEP 8 with type hints.
- Every measured value that replaces an estimate in these documents is marked
  as measured, with the conditions under which it was taken.
