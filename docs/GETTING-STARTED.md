# Getting started

From the parts on the bench to the first spoken answer, in order. Each
step links to where the detail is: [README.md](../README.md) for the
hardware, [OPERATING.md](OPERATING.md) for the rest.

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
   [Running the server](OPERATING.md#running-the-server): create the
   virtual environment, copy `server/.env.example` to `server/.env`, and
   fill in `DEEPGRAM_API_KEY` and `TIMEZONE`.

3. **Start the server** with `server/.venv/Scripts/python -m server`.

4. **Flash the firmware** from an ESP-IDF v5.5 PowerShell, in `firmware/`,
   with the board's **UART** port connected
   ([Flashing the firmware](OPERATING.md#flashing-the-firmware)):

   ```sh
   idf.py -p PORT flash monitor
   ```

5. **Enter the settings.** With no settings, the device starts in setup
   mode: join the WiFi network shown on its screen from a phone. On the
   server's computer, run `server/.venv/Scripts/python -m server pair`,
   which prints a 6-digit code lasting 10 minutes. On the page that opens
   on the phone, give the WiFi name and password and that code. The device
   restarts, joins the network and pairs with the server by itself. The
   console works too (`config set wifi_ssid`, `config set wifi_pass`,
   then `pair <code>`).

6. **Talk to it.** The screen goes from `CONNECTING` to `IDLE` once the
   server is found. Say "Hey Claude", wait for the two-note chime, then
   ask a question. The answer is spoken; one that needs the web begins
   with "Let me check."

## If it does not work

| What is seen | What to check |
| --- | --- |
| `SETUP`, "WiFi password refused", "Pairing code refused" or "Server refused this device" | The WiFi password; or make a new code with `python -m server pair` and enter it, since a code lasts 10 minutes and works once |
| `CONNECTING` and it stays there | That the server is running and on the same network. `link` on the console says whether the server was found or did not answer. A network that blocks mDNS needs `server_url` set to the server's address |
| `ERROR` after each question | The server's log names the failing service: a Deepgram key, or Claude Code not signed in |
| "Hey Claude" not detected | The model is trained on one speaker's voice and works best within 2 m; speak clearly toward the microphone. `wake log on` shows the scores |
| A restart during a loud reply | The supply or its cable is too weak (KNOWN-ISSUES R2) |
| A robotic voice in long replies, on a breadboard | The speaker shaking the breadboard; lift it to check (KNOWN-ISSUES, Caveats) |
