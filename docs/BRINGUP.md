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
| Supply rating (printed on charger) | 5 V, 1 A or greater | | Not a 500 mA USB 2.0 port |
| Bare board, idle | ~50-100 mA | | Rail holds 4.9 V or above |
| Plus microphone | Small increase | | Rail holds 4.9 V or above |
| Plus display, backlight off | Small increase | | Rail holds 4.9 V or above |
| Plus display, backlight full | Noticeable increase | | Rail holds 4.8 V or above |
| Plus amplifier, idle | Small increase | | Rail holds 4.8 V or above |
| **Playback peak (stage 2)** | **~1 A or more** | | **Rail never falls below 4.7 V** |

The criterion that matters is **rail voltage, not current**. Brownout is caused
by voltage collapse. A supply rated well above the draw can still fail through
a thin or long cable, so measure the rail rather than trusting the rating.

## Cable

Test with a short, thick USB cable. A thin or long one drops meaningful voltage
at 1 A, and a 2 A charger behind a poor cable behaves like a weak one. If the
rail sags, substitute the cable before replacing the supply.

## Before leaving this stage

- [ ] Pinout in [../README.md](../README.md) updated to match what was actually
      built, with the provisional marking removed (R4)
- [ ] Display supply and backlight requirements confirmed and recorded (R5)
- [ ] Measurement table filled in as far as stage 0 allows
- [ ] Supply and cable confirmed adequate at idle
- [ ] Any deviation from the documented pinout recorded and explained
