# Hardware bring-up (stage 0)

Procedure for assembling the circuit on a breadboard and verifying it
electrically before any firmware exists. The goal of this stage is to reach
stage 1 knowing that the wiring is correct and the supply is adequate, so that
later faults can be attributed to software.

Pin assignments are in [../README.md](../README.md). Risks referenced by
number are in [../KNOWN-ISSUES.md](../KNOWN-ISSUES.md).

## What this stage cannot do

Peripherals cannot be functionally verified here. Confirming that the
microphone captures audio, that the amplifier reproduces it and that the panel
renders requires firmware, which arrives in stages 2 and 3. Stage 0 verifies
continuity, supply integrity and baseline current draw only.

The peak current measurement under amplifier load also belongs to stage 2,
because nothing draws that current until audio is played. Stage 0 establishes
the baseline it will be compared against.

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
| Supply rating (printed on charger) | 5 V, 1 A or greater | Not recorded; PC USB port used | Not a 500 mA USB 2.0 port |
| Bare board, idle | ~50-100 mA | Not measured | Rail holds 4.9 V or above |
| Plus microphone | Small increase | Not measured | Rail holds 4.9 V or above |
| Plus display, backlight off | Small increase | Not measured | Rail holds 4.9 V or above |
| Plus display, backlight full | Noticeable increase | Not measured | Rail holds 4.8 V or above |
| Plus amplifier, idle | Small increase | Not measured | Rail holds 4.8 V or above |
| **Playback peak (stage 2)** | **~1 A or more** | | **Rail never falls below 4.7 V** |

## Results of the first bring-up (2026-09-27)

The first assembly was carried out without a multimeter or a USB power meter,
so the continuity checks and every figure in the measurement table above are
missing. The procedure was adapted as follows, and the gap is tracked as R9 in
[../KNOWN-ISSUES.md](../KNOWN-ISSUES.md).

- **Short protection.** Every first power-on used a PC USB port, which
  cuts power on overcurrent, instead of a charger, which would keep
  feeding a short.
- **Supply integrity.** The chip's brownout detector stood in for rail voltage
  measurement: a sagging 3.3 V rail resets the chip with a brownout reset
  reason, which is visible on the serial console.
- **Heat.** A touch test after a minute of running at each step.
- **Function.** Each peripheral was exercised by a temporary test program
  flashed after it was wired. These programs are not kept in the repository.

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

## Cable

Test with a short, thick USB cable. A thin or long one drops meaningful voltage
at 1 A, and a 2 A charger behind a poor cable behaves like a weak one. If the
rail sags, substitute the cable before replacing the supply.

## Before leaving this stage

- [x] Pinout in [../README.md](../README.md) updated to match what was actually
      built, with the provisional marking removed (R4)
- [x] Display supply and backlight requirements confirmed and recorded (R5)
- [ ] Measurement table filled in as far as stage 0 allows — deferred for
      lack of a meter (R9)
- [x] Supply and cable confirmed adequate at idle — by absence of brownout
      only, not by measurement (R9)
- [x] Any deviation from the documented pinout recorded and explained — none
