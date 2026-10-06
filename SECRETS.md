# Secrets and pre-commit checklist

This repository is public. Everything below must be verified before every
commit, and again before any push.

The rule that matters most: **`.gitignore` protects nothing that is already
tracked.** A file added before it was ignored stays in the repository and in
history. Check what is tracked, not only what is staged.

## Never commit

### Credentials

| Item | Where it tends to leak from |
| --- | --- |
| Anthropic API key (`sk-ant-...`) | Server environment, pasted into a config file or a test script |
| Speech provider API keys | Same |
| Device tokens | The server's `DEVICE_TOKENS` setting, firmware configuration, test fixtures, a command line |
| WiFi SSID and passphrase | `sdkconfig`, NVS CSV files, hardcoded during bring-up |

### ESP-IDF specific traps

- **`sdkconfig`** is generated and contains every value entered through
  `menuconfig`, including WiFi credentials. It must stay untracked. Only
  `sdkconfig.defaults` is committed, and only with placeholder values.
- **NVS partition CSV files and generated `.bin` images** carry WiFi
  credentials and device tokens in plain text.
- **`build/` directories** contain compiled binaries with configuration values
  embedded in them, plus a copy of `sdkconfig`.
- **Firmware binaries** of any kind, for the same reason.

### Personal data

Captured audio, transcripts and conversation history are personal data and are
excluded regardless of whose voice they contain.

- Audio files: `.wav`, `.pcm`, `.raw`, `.mp3`, `.opus`, and the formats
  phone and computer recorders produce: `.m4a`, `.aac`, `.flac`, `.ogg`,
  `.3gp`, `.amr`, `.webm`
- The conversation SQLite database (`server/data/`) and any dump of it
- Server logs. They name devices and network addresses, and contain
  transcripts when `LOG_TRANSCRIPTS` is on
- Recordings played to the server with the desktop client, and the replies
  it saves
- Wake-word training recordings and datasets

### Network and identity details

- Internal IP addresses and hostnames of the development network
- WiFi network names
- Device MAC addresses and device identifiers
- Personal email addresses and account identifiers

### Miscellaneous

- `.env` files of any kind
- Private keys and certificates
- Editor and IDE directories containing local paths or tokens
- Training notebooks with embedded credentials or personal file paths

## How configuration is supplied instead

| Component | Mechanism |
| --- | --- |
| Server | Environment variables, loaded from the untracked `server/.env`. The tracked `server/.env.example` lists the variable names with empty values. Device tokens are one of them, `DEVICE_TOKENS`. The server never prints a setting's value, in a start-up error or a log line, and the desktop client reads its token from the same file, never from the command line. |
| Firmware | Values entered through `menuconfig` into the untracked `sdkconfig`, or provisioned into NVS at flash time. `sdkconfig.defaults` carries placeholders only. |

Any new configuration value gets added to `.env.example` or
`sdkconfig.defaults` as an empty placeholder in the same commit that
introduces it.

## Pre-commit checklist

Run in order. Do not skip the first step because the change seems unrelated.

**1. Review what is staged.**

```sh
git diff --cached --name-only
git diff --cached
```

Read the diff. Do not rely on the file list alone — a credential added to an
otherwise routine file is the common case.

**2. Scan the staged content for credential patterns.**

```sh
git diff --cached -U0 | grep -nEi \
  'sk-ant-|api[_-]?key|secret|passw(or)?d|passphrase|psk|bearer|token|BEGIN [A-Z ]*PRIVATE KEY'
```

**3. Scan for network and identity details.**

```sh
git diff --cached -U0 | grep -nE \
  '192\.168\.|10\.[0-9]+\.|172\.(1[6-9]|2[0-9]|3[01])\.|([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}'
```

**4. Confirm nothing forbidden is tracked.**

```sh
git ls-files | grep -Ei \
  '(^|/)(sdkconfig|\.env)$|\.(wav|pcm|raw|mp3|opus|m4a|aac|flac|ogg|3gp|amr|webm|db|sqlite3?|bin|elf|pem|key|p12)$|(^|/)build/'
```

This must return nothing. Anything it returns is already in the repository and
needs removing from tracking and from history, not merely ignoring.

**5. Confirm placeholders are still placeholders.**

```sh
cat server/.env.example
grep -i -E 'ssid|passw|token|key' firmware/sdkconfig.defaults
```

Every value must be empty or an obvious placeholder.

**6. Before a first push, audit the whole history**, not just the tip:

```sh
git log -p --all | grep -nEi 'sk-ant-|api[_-]?key|passw(or)?d|psk|BEGIN [A-Z ]*PRIVATE KEY'
```

## If something leaked

Order matters.

1. **Rotate the credential first.** Revoke the key at the provider and issue a
   new one. Treat any key that reached a public repository as compromised
   permanently, whether or not anyone is believed to have seen it. Automated
   scrapers find committed keys within minutes.
2. **Then remove it from history** with `git filter-repo` or an equivalent, and
   force-push. Note that forks, clones and caches may retain it — which is why
   rotation comes first and is not optional.
3. **Record the incident** in [KNOWN-ISSUES.md](KNOWN-ISSUES.md) if anything
   about the project's handling of secrets needs to change as a result.

Removing a secret in a later commit without rotating it achieves nothing. It
remains readable in history.
