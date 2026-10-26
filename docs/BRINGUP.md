# Hardware bring-up

How to assemble the circuit on a breadboard and check it, one peripheral
at a time, before the device firmware goes on. A fault found right after a
single addition has a known cause; three peripherals wired at once and a
board that will not start do not.

The pinout is in the [README](../README.md#pinout). Risks referred to by
number are in [KNOWN-ISSUES.md](../KNOWN-ISSUES.md).

## What is needed

| Tool | For | |
| --- | --- | --- |
| Multimeter | Continuity and short checks | Required |
| USB power meter | Rail voltage and current | Required |
| A short, thick USB cable and a 1 A supply | Power | Required; a PC USB 3.0 port will do, a 500 mA USB 2.0 port will not |
| Magnifier or phone camera | Reading module silkscreens | Recommended |
| Bench supply with a current display | Better readings | Optional |

The test programs in [firmware/tools/bringup/](../firmware/tools/bringup/)
check each peripheral as it is added.

## Before starting

- **Pin orders come from each module's own silkscreen**, not a photograph
  or another board of the same name; vendors reorder pins between batches.
- **The breadboard is marginal for the amplifier.** Its contacts and rails
  are not made for 1 A transients, and their voltage drop can itself cause
  a brownout (R2). Run the amplifier's 5 V and ground with their own leads
  to the supply, not through the shared rails.
- **Power off between changes**, and use a PC USB port for every first
  power-on: it cuts power on a short, where a charger keeps feeding it.

## Steps

1. **Power rails only.** Wire 5 V, 3.3 V and ground, nothing else.
   - With power off, measure each rail against ground: near zero ohms is
     a short, to be found before powering on.
   - Power on and measure 5 V and 3.3 V at the far end of the rails.
   - Flash ESP-IDF's `hello_world`: it should boot with a power-on reset,
     not a brownout.

2. **Microphone (INMP441, I2S0).** Tie `L/R` to ground; floating, it gives
   silence or noise that looks like a driver fault. Check for shorts, power
   on, and run `mic_test`: the left slot rises with sound in the room and
   the right reads zero.

3. **Display (NV3007, SPI2)**, with the backlight pin. Run `disp_test`:
   the backlight comes on after 3 s, then the screen fills red, green,
   blue, white and black in turn. The panel is write-only, so a wiring
   fault shows only as a blank screen.

4. **Amplifier and speaker (MAX98357A, I2S1)**, last, being the only part
   that can draw an ampere.
   1. Feed it from **5 V, not 3.3 V**. At 3.3 V it works but quietly,
      which invites raising the gain into distortion.
   2. Fit the 1000 µF capacitor directly across its `Vin` and `GND` pins.
      **It is polarised:** the striped side is negative and goes to ground.
      Reversed, it can burst.
   3. Leave `GAIN` unconnected (9 dB). Volume is set in the firmware, and
      at 4 Ω full analogue gain gives distortion, not loudness.
   4. Connect the speaker, then run `amp_test`: a beep every 2 s at three
      clearly different volumes, with no resets.

5. **Supply under load.** Flash the device firmware
   ([OPERATING.md](OPERATING.md#flashing-the-firmware)) and run
   `audio tone 100 10`, a full-scale tone, the most current the device
   ever draws. The board must not reset, and the rail should stay above
   4.7 V. If it sags, change the cable before the supply. Repeat this
   after soldering and after any change of supply or cable.

What each test program's faults usually mean is in
[firmware/tools/bringup/README.md](../firmware/tools/bringup/README.md).

## Expected figures

Measured on the breadboard as built, from a PC USB 3.0 port through a USB
power meter (KEWEISI KWS-MX19), which shows averages a few times a second
and reads the voltage before the cable. The decisive result is the rail
voltage and the absence of a brownout reset, not the current: a supply
rated well above the draw can still fail through a poor cable.

| State | Rail | Current |
| --- | --- | --- |
| Microphone capturing | 4.946 V | 0.103 A |
| Display, backlight off / full | 4.943 V | 0.104 / 0.115 A |
| Amplifier enabled, silent | 4.945 V | 0.118 A |
| Tone at 20 % | 4.945 V | up to 0.138 A |
| WiFi connected, power saving on (stage 1 firmware) | 4.93-4.94 V | 0.124-0.143 A |
| Full-scale tone (`audio tone 100`) | 4.82-4.83 V | 0.56-0.58 A, no reset |
| Spoken reply at the full level | 4.918 V | about 0.2 A, no reset |

The backlight costs about 10 mA and the idle amplifier a few. The true
playback peak is above what the meter shows (R9).

## Notes on this hardware

- **The board** is an ESP32-S3R8 with 8 MB of PSRAM in the chip package,
  16 MB of flash and a CH340K USB-serial bridge on its UART port.
- **The display module** has a regulator in its `VDD` path and a
  transistor switching the backlight, so `VDD` is 3.3 V and `BL` is driven
  straight from a GPIO. Its `CS-LOW` solder jumper stays open, with `CS`
  wired.
- **The microphone** reads about -68 dBFS in a quiet room with a PC
  running nearby.
- **Continuity checks** have not been made on the existing build, for want
  of a multimeter (R9).
