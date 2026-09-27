# Bring-up test programs

Standalone ESP-IDF programs used during stage 0 to exercise each peripheral
once it is wired. Each is its own project and uses the pinout in
[../../../README.md](../../../README.md). None is part of the device firmware.

| Program | Peripheral | What it does | Healthy output |
| --- | --- | --- | --- |
| `mic_test` | INMP441 on I2S0 | Reads stereo at 16 kHz and prints the RMS, minimum and maximum of each slot every few hundred milliseconds | Left slot rises with sound in the room; right slot reads zero |
| `disp_test` | NV3007 on SPI2 | Holds the backlight off for 3 s, turns it on, initialises the panel and fills the screen red, green, blue, white and black in a 2 s loop | Colours appear in that order |
| `amp_test` | MAX98357A on I2S1 | Enables the amplifier and plays a 440 Hz beep every 2 s at 5 %, 10 % and 20 % of full scale | Three clearly different volumes, no resets |

For a check of the supply alone, use ESP-IDF's own `hello_world` example.

## Usage

From an ESP-IDF v5.5 shell, in the program's directory:

```sh
idf.py -p PORT flash monitor
```

The target and flash size come from `sdkconfig.defaults`. Every program prints
its reset reason at boot, either in the ROM banner (`rst:`) or explicitly.
A brownout reset there points at the supply, not at the program (R2 and R9 in
[../../../KNOWN-ISSUES.md](../../../KNOWN-ISSUES.md)).

Faults and what they usually mean:

| Program | Symptom | Likely cause |
| --- | --- | --- |
| `mic_test` | Both slots zero | No power to the microphone, or `SD` not connected |
| `mic_test` | Signal in the right slot instead of the left | `L/R` not tied to ground |
| `mic_test` | Full-scale or stuck values | `SD` floating, or `SCK`/`WS` swapped |
| `disp_test` | Backlight on, screen blank or unchanged | A data line (`SCL`, `SDA`, `DC`, `CS`, `RES`) or low `VDD` |
| `disp_test` | Colours cycle in the wrong order | Wiring is correct; colour order or inversion is a firmware setting |
| `amp_test` | Silence | `Vin`, `GND`, `SD` or `DIN` |
| `amp_test` | Resets during beeps | Supply or cable |
