# Getting started

From the parts on the bench to the first spoken answer, in order. Each
step links to the part of [README.md](../README.md) that holds the detail.

## What is needed

- **The hardware** in the README's [Hardware](../README.md#hardware)
  table, with the button and the switch, and a supply of at least 1 A.
- **A Deepgram API key**, for speech-to-text and text-to-speech.
- **A Claude account signed in to Claude Code** on the computer that runs
  the server. The assistant runs under that login; no Anthropic API key is
  needed.
- **ESP-IDF v5.5.x** to build the firmware. Not v5.3 or v6.x
  ([CLAUDE.md](../CLAUDE.md), Pinned versions).
- **Python 3.11** or later, on the same local network as the device.

## Steps

1. **Wire the circuit** as the README's [Pinout](../README.md#pinout)
   gives it. [docs/BRINGUP.md](BRINGUP.md) checks each peripheral in turn.

2. **Set up the server** as in
   [Running the server](../README.md#running-the-server): create the
   virtual environment, copy `server/.env.example` to `server/.env`, and
   fill in `DEEPGRAM_API_KEY` and `TIMEZONE`.

3. **Give the device a token.** Generate one:

   ```sh
   python -c "import secrets; print(secrets.token_urlsafe(32))"
   ```

   and put it in `server/.env` under the device's identifier, which is
   `hc-` followed by the last six hex digits of the board's MAC address.
   If it is not known yet, the setup page of step 5 shows it; add the
   entry then and restart the server:

   ```
   DEVICE_TOKENS={"hc-a1b2c3":"<token>"}
   ```

   Then start the server with `server/.venv/Scripts/python -m server`.

4. **Flash the firmware** from an ESP-IDF v5.5 PowerShell, in `firmware/`,
   with the board's **UART** port connected
   ([Building and setting up the device](../README.md#building-and-setting-up-the-device)):

   ```sh
   idf.py -p PORT flash monitor
   ```

5. **Enter the settings.** With no settings, the device starts in setup
   mode: join the WiFi network shown on its screen from a phone, and on
   the page that opens give the WiFi name and password and the token from
   step 3. The device restarts and joins the network. The console works
   too (`config set`, then `reboot`).

6. **Talk to it.** The screen goes from `CONNECTING` to `IDLE` once the
   server is found. Say "Hey Claude", wait for the two-note chime, then
   ask a question. The answer is spoken; one that needs the web begins
   with "Let me check."

## If it does not work

| What is seen | What to check |
| --- | --- |
| `SETUP`, "WiFi password refused" or "Server refused this device" | The WiFi password, or that the token and identifier match `DEVICE_TOKENS` exactly |
| `CONNECTING` and it stays there | That the server is running and on the same network. `link` on the console says whether the server was found or did not answer. A network that blocks mDNS needs `server_url` set to the server's address |
| `ERROR` after each question | The server's log names the failing service: a Deepgram key, or Claude Code not signed in |
| "Hey Claude" not detected | The model is trained on one speaker's voice and works best within 2 m; speak clearly toward the microphone. `wake log on` shows the scores |
| A restart during a loud reply | The supply or its cable is too weak (KNOWN-ISSUES R2) |
| A robotic voice in long replies, on a breadboard | The speaker shaking the breadboard; lift it to check (KNOWN-ISSUES, Caveats) |
