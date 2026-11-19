# Simulacrum — hardware feasibility (step 5)

> Historical report from the first build (2026-11-17). Hardware testing
> later showed the Cortex-M7 runs this code about 3× slower per
> instruction than estimated here; the CPU work that followed (24 kHz
> grains, governor headroom, per-stage gauges) is logged in SPEC.md.

Date: 2026-11-17 · SDK v0.13.0 · libDaisy 08f2965 · arm-none-eabi-gcc 15.3

Verdict: **the Alchemy Lab V2 should handle Simulacrum**, with real but
manageable CPU headroom in the worst case. Memory is comfortable. This is
an estimate from the build and host tests. It has not been measured on
the module; the on-device CPU gauges below are the final check.

## Memory (from the linker, `make`)

| Region | Used | Size | Notes |
|---|---|---|---|
| SRAM (code, boot-SRAM app) | 296 KB | 480 KB | 62 % |
| DTCM (data + stack) | 108 KB | 128 KB | 84 %; ~20 KB left for the stack |
| SDRAM | 9.1 MB | 64 MB | tape 6.5 MB (17 s stereo float) + grains 2.9 MB (30 s, 24 kHz, 12-bit) |

**Stack depth** (`-fstack-usage`, app + SDK): the deepest likely chain is
the main loop answering a HostLink descriptor request (≈3.7 KB) plus the
audio interrupt (≈0.6 KB) and USB/HAL interrupts. That is under 8 KB,
against about 20 KB available. The 3 KB start-up sweep runs once, before the loop.

**Presets:** knob-motion recording takes 30 slots × (7 + 8 s × 20 Hz × 2 B) =
9.8 KB of the 20.4 KB slot. The SDK checks this at compile time. Pager,
buttons, settings and names add well under 1 KB.

## Behaviour (host tests: `make -C tests run` → 25 passed, 0 failed)

The DSP sources compile natively against two tiny libDaisy stubs and run
scripted scenarios:

| Area | Result |
|---|---|
| Dry path | output follows input when both mixes are dry |
| Tape delay time | first repeat at 250.1 ms for a 250 ms clock |
| Feedback | repeats decay at 50 %; at 105 % it self-oscillates, stays ≤ full scale, no NaN over 20 s |
| Freeze | 100 % of the level after 10 s (lossless loop) |
| Tone | CCW darkens each repeat: HF/LF falls by >10⁴ from repeat 1 to repeat 4 |
| Microspeed | +1 V at full CCW moves the repeats up an octave (+43 dB at the new pitch) |
| Grains | play; +12 st lands on the octave; ×12 scan speed keeps the pitch |
| 12-bit / 24 kHz | grain output 22 dB down at 16 kHz vs 3 kHz (S950-style band limit) |
| Stack pitch | C4 = 262.0 Hz, +1 V = 523.0 Hz, −1 oct = 131.0 Hz |
| Sine dominance | at max saw blend the fundamental is still 28 dB over the 2nd harmonic |
| Notches | noon transparent (0.0 dB); CCW: −33 dB / −36 dB at the two set frequencies, −1.2 dB away from them |
| CW stereo cutoff | lows centred (side −11 dB at 60 Hz), highs wide (−0.8 dB at 5 kHz) |
| Everything maxed, 30 s | stable, peak 0.86 |

### Fixed during this step
- **Freeze lost ~44 % in 10 s.** The playback-head filter and interpolation
  ran on every pass. Freeze now re-records the exact samples (lossless),
  while wow still colours what you hear.
- **Grain images above 11 kHz.** Playing a 24 kHz buffer at 48 kHz left
  images. Added an S950-style 11 kHz output smoothing filter.
- **Pitch-shift window** lengthened from 50 → 75 ms (less warble on large
  V/Oct microspeed shifts).

## CPU (the real risk)

Budget: one 24-sample block every 0.5 ms = **240 000 cycles** at 480 MHz.

Method: count instructions retired for the DSP compiled as scalar ARM64
(vectorisation off, close to the M7's scalar FPU), per 24-sample block,
then scale to the Cortex-M7 at roughly 1.2× more instructions for Thumb-2 and
1.0–1.6 cycles per instruction, plus SDRAM cache misses (~6 k cycles).

| Scenario | ARM64 instr / block | Est. M7 cycles | Est. load |
|---|---|---|---|
| Everything at rest (dry, stack silent) | 30 k | 37–58 k | **15–24 %** |
| Worst case without Feast octaves | 71 k | 90–142 k | **38–59 %** |
| Worst case: 12 × 4 s grains, ×12 scan, Feast with −12/0/+12, stereo notch, max detune, pitch-shifting tape, 105 % feedback | 100 k | 126–198 k | **53–83 %** |

Optimisations already in, after profiling (worst case 117 k → 100 k):
cached envelope-morph table, cheaper buffer stepping, no grain synthesis
when grains are inaudible, notch coefficients updated at 12 kHz, fast
exp2 in the per-sample notch spacing.

**Safety net: CPU governor.** If the audio callback averages above 85 %
of its budget, the number of simultaneous grains is lowered (never below
4). Voices come back once the load falls under 65 %. The grain rhythm
(triggers per clock) never changes; only overlap is limited. In the
worst case this trades a little density for no dropouts.

Main-loop share: the control loop (pots, LEDs, USB) runs in whatever the
audio interrupt leaves. At the worst-case estimate that is about 20–45 % of
the CPU, which is plenty for a 60 fps panel.

## Other checks
- **J7/J8 outputs:** the STM32 DAC is written once per audio block (2 kHz).
  It's fast, with no I²C. The sub-osc out is stepped at audio pitches, as
  noted in the spec.
- **Clock in (J3):** edges are timestamped at the 1 ms poll, so there is ±1 ms
  jitter, smoothed by period averaging and the tape-style glide. External
  clock is detected automatically; it falls back to the internal tempo after
  about 2 missed pulses.
- **V/Oct (J4):** 16-bit calibrated input with light smoothing. Tracking is
  as good as the board calibration; run calibration once (B1 + B2 at
  power-on) if you haven't.
- **Boot:** SDRAM buffers are cleared in well under 0.1 s, before audio
  starts; then the 2.1 s start-up sweep.

## What needs the real module
1. Flash `build/simulacrum.bin` with the
   [web programmer](https://hermeticmodular.com/program), or with `make program-dfu`
   once `dfu-util` is installed.
2. Open the web programmer's console (or `tools/hostlink-cli`) and watch
   the gauges: **Audio CPU avg / peak**, **Grain voice cap (CPU)**,
   **Clock BPM**, **External clock**.
3. Push the worst case above. If *Grain voice cap* drops below 12, the
   governor is working, and we can optimise the octave-layer reads further
   in step 6.
4. Listen for: freeze clicks at the loop point, wow/flutter taste,
   notch character, 12-bit crunch amount, LED busyness.
