# Simulacrum

Custom firmware for the **Hermetic Modular Alchemy Lab** (V2) Eurorack module,
built as an accompaniment voice for a **Moog Matriarch**. It runs three layers at
once, in stereo:

- **Tape**: a dark, clock-synced stereo tape delay with tasteful wow/flutter, a
  bipolar DJ-style tone filter that compounds on every repeat, up to 105 %
  feedback with tape saturation, and *microspeed* (V/Oct or a deep sub-sine
  bending the repeats).
- **Grain**: a clocked granular delay. It records into an Akai S950-style
  12-bit / 24 kHz buffer and plays up to 36 grains, with true per-grain time
  stretch (1/16× to 8× without pitch change), a Beads-style envelope morph, and
  semitone pitch locked to the stack's tuning.
- **Stack**: up to 8 ringing voices of quadrature sines and a supersaw (Rings-style
  overlap, played from a gate on J6 and V/Oct on J4). Each voice has its own
  attack/release envelope, a warm VCA and a slightly LPG-style low-pass, then a
  gentle ladder low-pass. They run through a dual-notch filter with a bipolar
  mono/stereo cutoff, notch spacing swept by a sub-oscillator, and "Grains →
  Filter" to feed the grain cloud through the notches.

Version **1.0.0**. Built on the [Alchemy SDK](https://github.com/hermetic-modular/alchemy-sdk)
v0.13.0 and [libDaisy](https://github.com/electro-smith/libDaisy).

---

## Panel

Three main pages (B1 cycles), plus three hidden pages shown while a button is held.
The page colour shows on B1 and in the rings. Values below are the startup defaults.

### Tape (amber)
| Pot | Control | Notes |
|---|---|---|
| P1 | Mix | dry → wet · **50 %** |
| P2 | Clock | delay = clock × ¼, ⅓, ½, ⅔, ¾, **1**, 3/2, 2, 3, 4, 8; changes glide like tape |
| P3 | Feedback | 0–105 %, saturates rather than running away · **25 %** |
| P4 | Tone | bipolar DJ filter in the feedback loop: CCW darker, CW brighter, compounding per repeat · **flat** |
| P5 | Wow/Flutter | subtle → noticeable, never seasick · **10 %** |
| P6 | Microspeed | CCW: V/Oct bends the repeats (up to ⅓ V/oct); CW: a sub-sine 4 octaves below the note modulates them · **off** |

**B2** freeze (lossless loop) · **B3** repeat mode: normal / ping-pong

### Grain (cyan)
| Pot | Control | Notes |
|---|---|---|
| P1 | Mix | dry → 50/50 at noon → 25/75; the second half records the *tape output* into the grain buffer · **25 %** |
| P2 | Clock | grain clock = clock × 1/24 … **1** … 24 (20 steps): how far back grains read, and their rhythm |
| P3 | Size | 5 ms – 4 s of audio per grain · **1 s** |
| P4 | Count | 1–36 grains (more triggers per clock as it rises; dense clouds auto-spread) · **6** |
| P5 | Envelope | square → LPG → triangle → slow attack; shaped automatically by clock, size and count · **square** |
| P6 | Speed | playback speed inside each grain, same pitch: 1/16, 1/8, 1/4, 1/3, 1/2, 2/3, **1×**, 3/2, 2, 3, 4, 6, 8 |

**B2** freeze (stop recording) · **B3 tap** direction: forward / reverse / random · **B3 hold** → Grain Parameters

### Stack (violet)
| Pot | Control | Notes |
|---|---|---|
| P1 | Level | **0 %** |
| P2 | Tuning | free, ±2 octaves around C4 (tune to your V/Oct source); J4 adds |
| P3 | Detune | supersaw spread + sine-pair drift · **10 %** |
| P4 | Cutoff | bipolar dual notch: noon = open; CCW sweeps down in mono; CW sweeps down in stereo (L/R split, lows held centre) |
| P5 | Attack | 1 ms – 4 s (noon ≈ 1 s), exponential → linear · **0** |
| P6 | Release | 3 ms – 8 s (noon ≈ 1.5 s) · **0** |

**B2** V/Oct → grains (grains follow the notes) · **B3 tap** mute · **B3 hold** → Stack Parameters

### Global: hold B2 (warm white)
| P1 Input Gain **1×** | P2 Output Level **1×** | P3 Spread **25 %** | P4 Grains → Filter **25 %** | P5 Sub → Spacing **25 %** | P6 CV6 Function |
|---|---|---|---|---|---|

### Grain Parameters: hold B3 on Grain (blue)
P1 Grain Pitch (±24 semitones, snapped to the stack's tuning) · P2 Pitch Random · P3 Grain Spray

### Stack Parameters: hold B3 on Stack (green)
P1 Sine/Saw (sines always dominate) · P2 Octaves (unison / −12 / −12+12) · P3 Voices (1–8) · P4 Spacing (0–3 octaves)

### Settings: hold B2 + B3 for 2 s
Page 1: LED brightness (30 %), presets. Page 2 "Clock" (press B1): internal **Tempo**
(120 BPM), used when nothing is patched to J3.

Hidden pages and Settings use the SDK's pot catch: a knob takes over once you turn it
past its stored value. Bipolar knobs have a wide centre detent, shown as a 3-LED bar.

---

## Jacks

| Jack | Function |
|---|---|
| J1 / J2 | Audio in L / R. J2 copies J1 after 10 s of silence on J2 (mono → stereo). |
| J3 (CV1) | Clock in. Settles on whole BPM and glides; falls back to the internal tempo. |
| J4 (CV2) | V/Oct |
| J5 (CV3) | Grain Size CV |
| J6 (CV4) | Gate / trigger for the stack notes |
| J7 (CV5) | Grain Spray CV |
| J8 (CV6) | Assignable (Global P6): Feedback CV, Cutoff CV, Spacing CV, Envelopes CV, Sub-Osc Out, Grain Trigger Out |
| J9 / J10 | Audio out L / R |

CV inputs span the full knob range at ±5 V. The Hermetic Modular editor shows every
jack by name, and J8's name follows its function.

### Playing notes (J6 + J4)
- A rising gate starts a note once V/Oct has settled. While the gate is held, each new
  V/Oct value starts another note. Glides only make a note where they land.
- Earlier notes ring out on their own release while the new one plays. **Voices** sets
  how many overlap.
- Gate held: attack, then decay to 50 % sustain (at least 1 s, longer with long
  releases). Gate released: every note releases. A short trigger plays and rings out.
- J6 has no jack-detect switch, so with nothing in J6 the stack stays silent.

---

## Building

Requirements: `git`, `make`, `arm-none-eabi-gcc` (12+), `dfu-util` (optional).

```sh
git clone --recurse-submodules https://github.com/araz4github/simulacrum.git
cd simulacrum
git -C lib/libDaisy submodule update --init Drivers/CMSIS_5 Drivers/CMSIS-Device/ST/STM32H7xx \
    Drivers/STM32H7xx_HAL_Driver Middlewares/ST/STM32_USB_Device_Library Drivers/CMSIS-DSP
make libdaisy     # once
make              # → build/simulacrum.bin
```

The submodules pin the Alchemy SDK at v0.13.0 and libDaisy at the commit the SDK is
tested against. If `lib/` is empty, the Makefile falls back to a sibling
`../alchemy-sdk-main` checkout.

Host-side DSP tests (no hardware needed):

```sh
make -C tests run
```

## Flashing

Use the prebuilt `simulacrum.bin` from the [Releases](https://github.com/araz4github/simulacrum/releases)
page, or your own build.

1. Connect the front USB-C port.
2. Power on and press **B3** while the rings spin a warm-white comet (update mode).
3. Flash with the [Hermetic Modular web programmer](https://hermeticmodular.com/program),
   or `make program-dfu`. Once Simulacrum is running, `make program-live` reflashes
   without the button dance.

Run the board's CV calibration once (hold **B1 + B2** while powering on) for accurate
V/Oct tracking.

## Diagnostics

Over USB, the web programmer console or `tools/hostlink-cli` show live gauges:
- audio CPU (average, peak since boot, peak over the last second), overruns, and the
  per-stage split (tape / grains / stack / rest);
- the grain voice cap (a CPU governor that limits grain overlap when the load runs
  high, never the rhythm);
- clock BPM and external-clock detection;
- J1→J2 copy, gate, notes started, and stack voices sounding.

## Docs

- [`docs/SPEC.md`](docs/SPEC.md): the full design and change log (concept, every page,
  LEDs, DSP decisions, CPU work).
- [`docs/FEASIBILITY.md`](docs/FEASIBILITY.md): the original hardware feasibility study.

## License

Simulacrum is released under the **GNU General Public License v3.0** (see
[LICENSE](LICENSE)). It builds on the MIT-licensed Alchemy SDK by Hermetic Modular and
libDaisy by Electrosmith, which keep their own licenses.
