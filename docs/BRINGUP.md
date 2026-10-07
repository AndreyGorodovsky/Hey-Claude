# Hardware bring-up (stage 0)

Procedure for assembling the circuit on a breadboard and verifying it
electrically before any firmware exists. The goal of this stage is to reach
stage 1 knowing that the wiring is correct and the supply is adequate, so that
later faults can be attributed to software.

Pin assignments are in [../README.md](../README.md). Risks referenced by
number are in [../KNOWN-ISSUES.md](../KNOWN-ISSUES.md).

## What this stage cannot do

Stage 0 verifies continuity, supply integrity and baseline current draw. The
test programs in [../firmware/tools/bringup/](../firmware/tools/bringup/) add a
basic functional check of each peripheral: signal present, panel fills, tone
audible. Audio quality, sustained throughput and rendering belong to the
project firmware in stages 2 and 3.

The peak current measurement under amplifier load also belongs to stage 2,
because nothing draws that current until audio is played. Stage 0 establishes
the baseline it will be compared against. The baseline figures are in the
[measurement record](#measurement-record).

## Tools

| Tool | Purpose | Necessity |
| --- | --- | --- |
| USB power meter, ideally with peak hold | Rail voltage and current under load | Required |
| Multimeter | Continuity and short checks | Required |
| Bench supply with current display | Better than a USB meter where available | Optional |
| Magnifier or phone camera zoom | Reading module silkscreens | Recommended |

A multimeter alone is not sufficient for the stage 2 peak measurement. Its
averaging window is far too slow to catch a 10 ms transient, so it reports a
comfortable figure while the board browns out.

## Breadboard caveat

A breadboard is acceptable for the microphone and the display, and marginal for
the amplifier. Contact resistance at each connection is on the order of tens of
milliohms, and the power rails are not designed for 1 A transients. The
resulting voltage drop can itself cause the brownout described in R2, making
the breadboard the fault rather than the supply.

Two consequences:

- Run the amplifier's 5 V and ground with dedicated leads directly to the
  supply source, not through the breadboard rails shared with everything else.
- Treat any peak measurement taken on a breadboard as pessimistic. Repeat it
  once the circuit is soldered before concluding the supply is inadequate.

## Order of operations

Add one peripheral at a time, powering down between changes. When a fault
appears immediately after a single addition, its cause is known. When three
peripherals are wired at once and the board fails to enumerate, it is not.

### Step 1 — Inspect before wiring

1. Identify every module's pinout from its own silkscreen. Breakout vendors
   reorder pins between batches; do not assume the pin order from a photograph
   or from another board of the same name.
2. Confirm the display module's supply and backlight requirements (R5). Small
   NV3007 panels often require a backlight boost rail rather than direct 3.3 V.
   The panel is write-only over SPI, so a wiring fault produces a blank screen
   and no diagnostic.
3. Confirm the speaker is 4 Ω and note its power rating.

### Step 2 — Power rails only

Wire 5 V, 3.3 V and ground. Connect nothing else.

- Verify 5 V and 3.3 V at the far end of the breadboard rails, not at the board
  pins, so that rail continuity is included in the measurement.
- Confirm ground is common across both rails.
- Measure resistance between each supply rail and ground with power removed. A
  reading near zero is a short and must be found before applying power.

Record the idle current of the bare board.

### Step 3 — Microphone

Wire the INMP441 to I2S0. Tie its `L/R` pin to ground to select the left
channel; leaving it floating produces silence or noise that resembles a driver
fault.

Re-check for shorts, power on, and record current.

### Step 4 — Display

Wire the NV3007 to SPI2, including the backlight control pin.

The backlight is the dominant consumer here. Record current with the backlight
both off and at full brightness, since the difference sets the ceiling for the
animation design in stage 3.

### Step 5 — Amplifier and speaker

Wired last, because it is the only part that can draw an ampere.

1. Feed the MAX98357A from **5 V, not 3.3 V**. At 3.3 V it will work and sound
   quiet, which invites raising the gain and arriving at distortion.
2. Place the 1000 µF capacitor directly across the amplifier's supply and
   ground pins, as close to the device as the breadboard allows.
   **It is electrolytic and polarised.** Reversed, it will vent or burst. The
   marked stripe is the negative terminal and goes to ground.
3. Set the `GAIN` pin for 6-9 dB rather than leaving it at maximum. Volume is
   controlled digitally in firmware. At 4 Ω, maximum analogue gain yields
   distortion, not loudness.
4. Connect the speaker last.

Record idle current with the amplifier powered but not driven.

## Measurement record

Fill in during bring-up and keep the completed table with the project.

| Checkpoint | Expected | Measured | Pass criterion |
| --- | --- | --- | --- |
| Supply rating (printed on charger) | 5 V, 1 A or greater | PC USB 3.0 port (900 mA by specification) | Not a 500 mA USB 2.0 port |
| Bare board, idle | ~50-100 mA | Not measured separately; see below | Rail holds 4.9 V or above |
| Microphone capturing, backlight off | Small increase | 4.946 V, 0.103 A | Rail holds 4.9 V or above |
| Display, backlight off | Small increase | 4.943 V, 0.104-0.105 A | Rail holds 4.9 V or above |
| Display, backlight full | Noticeable increase | 4.943 V, 0.115 A | Rail holds 4.8 V or above |
| Amplifier enabled, silent | Small increase | 4.945 V, 0.118 A | Rail holds 4.8 V or above |
| 440 Hz tone at 20 % of full scale | — | 4.945 V, up to 0.138 A | Rail holds 4.8 V or above |
| WiFi connected, power save on | — | 4.932-4.940 V, 0.124-0.143 A | Rail holds 4.8 V or above |
| **Playback peak (stage 2)** | **~1 A or more** | **4.831 V, 0.556 A average; no reset** | **Rail never falls below 4.7 V** |
| Playback peak, repeated (stage 6) | As above | 4.822 V, about 0.582 A; no reset | Rail never falls below 4.7 V |
| Spoken reply at the full level (stage 6) | Below the tone | 4.918 V, about 0.2 A; no reset | Rail never falls below 4.7 V |

Every figure is measured, on 2026-09-28 unless its row names a later stage,
under the conditions described in
[Baseline measurement](#baseline-measurement-2026-09-28). The two stage 6
rows were measured on 2026-10-07 on the breadboard, with the stage 6
firmware, WiFi connected and power saving off: the tone with
`audio tone 100 10`, and the reply a spoken one of 66.7 s through the
server. Both are readings off the meter's display, as below. The stage 2 row was
taken with the device firmware's `audio tone 100 5`: a 440 Hz sine at full
scale, played at 24 kHz for 5 s, with WiFi connected. It is an average read
off the meter's display, so the true peak is higher (R9); the absence of a
brownout reset is the result that counts.

## Results of the first bring-up (2026-09-27)

The first assembly was carried out without a multimeter or a USB power meter,
so the continuity checks and every figure in the measurement table above were
missing when the stage closed. The figures were taken the next day (see
[Baseline measurement](#baseline-measurement-2026-09-28)); the continuity
checks remain undone. The procedure was adapted as follows, and the remaining
gap is tracked as R9 in [../KNOWN-ISSUES.md](../KNOWN-ISSUES.md).

- **Short protection.** Every first power-on used a PC USB port, which
  cuts power on overcurrent, instead of a charger, which would keep
  feeding a short.
- **Supply integrity.** The chip's brownout detector stood in for rail voltage
  measurement: a sagging 3.3 V rail resets the chip with a brownout reset
  reason, which is visible on the serial console.
- **Heat.** A touch test after a minute of running at each step.
- **Function.** Each peripheral was exercised by a test program flashed after
  it was wired. The programs are kept in
  [../firmware/tools/bringup/](../firmware/tools/bringup/) for re-checking the
  hardware later.

The pinout in [../README.md](../README.md) was built without deviation.

| Step | Check | Result |
| --- | --- | --- |
| Board identity | `esptool` chip query | ESP32-S3 (QFN56) rev v0.2, 8 MB in-package PSRAM, 16 MB flash detected; USB-serial bridge is a CH340K |
| Power only | `hello_world`, three boots | Power-on reset only, no brownout; no component warm |
| Microphone | I2S0 stereo read at 16 kHz, 24-bit, per-channel level | Left slot live, responding to room sounds; right slot constant zero, confirming `L/R` is tied to ground. Measured: quiet-room level of about 2,000-4,000 RMS counts at 24 bits (about −68 dBFS) with a PC running nearby |
| Display | Backlight switched from GPIO14, then panel init and full-screen colour fills at 10 MHz SPI | Backlight responds; red, green, blue, white and black fill correctly with `VDD` at 3.3 V |
| Amplifier | 440 Hz tone on I2S1 at 16 kHz, 5 %, 10 % and 20 % of full scale, `SD` driven high | Clean tone at three distinct levels; no brownout on a PC USB port; amplifier not warm |

### Display module findings (R5)

The back of the module carries a three-terminal device with input and output
capacitors in the `VDD` path, probably a linear regulator, and a transistor
(Q1) switching the backlight. The regulator's marking could not be read.
The panel works fully with `VDD` at 3.3 V, so 3.3 V is used. `BL` drives only
the transistor, so a GPIO can control it directly. A solder jumper marked
`CS-LOW` can tie chip select low permanently. It is left open, and `CS` is
wired.

The criterion that matters is **rail voltage, not current**. Brownout is caused
by voltage collapse. A supply rated well above the draw can still fail through
a thin or long cable, so measure the rail rather than trusting the rating.

## Baseline measurement (2026-09-28)

Taken with a KEWEISI KWS-MX19 USB power meter placed between a PC USB 3.0
port and the cable to the board's UART port, with every peripheral wired on
the breadboard as built in stage 0. Each state was produced by the program
named below rather than by wiring peripherals one at a time, which the
procedure above assumes; unwiring a verified build to measure draws too small
to separate on this meter was not worth the risk of a wiring fault.

| State | Program | Backlight |
| --- | --- | --- |
| Microphone capturing | `mic_test` | Off (pin not driven) |
| Display, backlight off | `disp_test`, first 3 s after reset | Off |
| Display, backlight full | `disp_test`, colours cycling | On |
| Amplifier enabled, silent; tone at 20 % | `amp_test`, between and during beeps | On (pin not driven) |
| WiFi connected, power save on | Stage 1 firmware | On (pin not driven) |

What the figures show:

- **The rail held 4.93 V or above in every state**, with no reset.
- **The backlight costs about 10-11 mA**, much less than the procedure
  expected. It sets no meaningful ceiling on animation in stage 3.
- **The amplifier idles at a few milliamps** once enabled, and a tone at 20 %
  of full scale adds about 20 mA.
- **WiFi in power-save mode adds roughly 10-25 mA on average**, fluctuating
  with radio activity.

Limitations:

- **The meter reads voltage at its own position**, before the cable. The
  voltage at the board is lower by the cable's drop, which is negligible at
  these currents but not at the stage 2 peak.
- **The display updates a few times per second**, so transients of tens of
  milliseconds, such as WiFi transmit bursts, are averaged away. The figures
  are averages, not peaks. Whether the meter can hold a peak reading was not
  checked.
- **No bare-board figure exists**, because the peripherals stayed wired. The
  lowest reading, 0.103 A, includes the microphone and the idle display and
  amplifier logic.
- **In three of the five states the backlight pin was not driven**, and
  whether the backlight lit depended on where the floating pin settled (R11).
  The table records what was observed.

## Cable

Test with a short, thick USB cable. A thin or long one drops meaningful voltage
at 1 A, and a 2 A charger behind a poor cable behaves like a weak one. If the
rail sags, substitute the cable before replacing the supply.

## Before leaving this stage

- [x] Pinout in [../README.md](../README.md) updated to match what was actually
      built, with the provisional marking removed (R4)
- [x] Display supply and backlight requirements confirmed and recorded (R5)
- [x] Measurement table filled in as far as stage 0 allows — completed
      2026-09-28, after the stage closed
- [x] Supply and cable confirmed adequate at idle — measured on 2026-09-28;
      rail at 4.93 V or above in every state
- [ ] Continuity and short checks — not done; requires a multimeter (R9)
- [x] Any deviation from the documented pinout recorded and explained — none
