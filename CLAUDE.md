# Project rules

Read [ARCHITECTURE.md](ARCHITECTURE.md) before design changes and
[STATUS.md](STATUS.md) before starting work.

## Workflow

For each stage or change:

1. **Plan** the steps and get them approved before implementing.
2. **Implement** only the approved steps. Anything else found on the way
   goes into [KNOWN-ISSUES.md](KNOWN-ISSUES.md), not silently fixed.
3. **Review** with the agents in `.claude/agents/`, in order:
   `code-reviewer`, then `challenger`.
4. **Apply** their corrections.
5. **Update the documents** to the current state; stale documentation is a
   failure, not a follow-up.
6. **Commit.** Push only when asked.

## Hard constraints

- No secrets in the repository: run the [SECRETS.md](SECRETS.md) checklist
  before every commit. API keys live on the server only.
- No audio, transcripts or conversation logs in the repository.
- The device stays stateless: no conversation history, no credentials
  beyond its own identity.
- GPIO 26-37 are unusable (flash and in-package PSRAM of the ESP32-S3R8).
  GPIO 0, 3, 45, 46 are strapping pins; GPIO 19, 20 carry native USB.
- Do not disable thinking on `claude-opus-5`; lower effort instead.

## Pinned versions

| Component | Version | Note |
| --- | --- | --- |
| ESP-IDF | v5.5.x | Not v5.3 (predates the NV3007 and LVGL work), not v6.x |
| LVGL | 9.6.0 | Locked in `firmware/dependencies.lock` |
| esp-tflite-micro, esp-nn, esp-micro-speech-features | 1.3.3~1, 1.1.2, 1.2.3 | Exact: the wake-word cutoff depends on their arithmetic |
| Python | 3.11+ | |
| Claude model | `claude-opus-5` | |
| Claude Code | 2.1.303 last benched, not pinned | Default backend; after an update follow README.md, Updating Claude Code |

## Environment

- Check `IDF_PATH` is a v5.5.x checkout before building. Run ESP-IDF from
  PowerShell, never Git Bash (`MSYSTEM` blocks activation). When changing
  ESP-IDF versions, check the persisted user environment too (R7).
- The board's console is UART0, on the CH340 port.

## Conventions

- Documents are for a public audience and never addressed to one reader.
  Human documents open with one line on their purpose, use numbered steps
  for procedures and tables for reference, and describe the present, not
  history; measured values say they are measured, and under what
  conditions.
- Firmware follows ESP-IDF style, the server PEP 8 with type hints.
- **Firmware is commented for a reader new to embedded work:** each file
  opens with what it does and where it fits; hardware and ESP-IDF concepts
  are explained at first use in each file; comments give the reason for
  non-obvious calls, units and magic numbers, and which task or context
  runs the code.
- Server code is commented for intent and non-obvious decisions, with
  docstrings on public functions and classes.
- Chip temperature: record only a reading above 60 °C, with what the device
  was doing, in R20 of KNOWN-ISSUES.md.
