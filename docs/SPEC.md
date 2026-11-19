# Simulacrum — design spec (living document)

Alchemy Lab V2 firmware. Stereo in (J1/J2) → stereo out (J9/J10).
Three layers run simultaneously; pages only choose which controls the pots edit.

Status: steps 2–5 done. Firmware builds (`make` → build/simulacrum.bin),
host DSP tests pass (`make -C tests run`), feasibility in FEASIBILITY.md.
Next: on-module check, then step 6 (finalize).

Panel: P1 top-left, P2 top-right, P3 mid-left, P4 mid-right, P5 bottom-left,
P6 bottom-right. Controls A–F map to P1–P6.

## Signal flow

```
In L/R ──┬──────────────────────────────────────────────► dry
         ├─► TAPE (stereo, clocked) ──────────────────────► tape wet
         │        │
         │        └─(grain record source, 2nd half of 2-A)
         ├─► GRAIN buffer (12-bit/24 kHz) ─► grains (stereo) ─► grain wet
         │                                 │
         │                    Granular Feast (3-F) steals ─┐
         │                                                 ▼
         └   STACK osc (quad sines + 6 supersaw) ──► dual-notch SVF ─► stack out
                 sub sine (−2 oct) ─► notch spacing (bipolar)
                                   ─► tape microspeed (via 1-F)
                                   ─► J7/J8 when set to "sub out"
```

Layers are in parallel; the only series path is the grain layer recording
the tape output in the second half of control 2-A.

## Page 1 — Tape (clock synced, "dark")
| Pot | Control | Behaviour |
|---|---|---|
| P1 | Mix | dry → wet |
| P2 | Clock ÷/× | stepped ¼, ⅓, ½, ⅔, ¾, **1**, 3/2, 2, 3, 4, 8 × clock period; time changes glide pitch like tape |
| P3 | Feedback | 0 → 105 %, tape saturation tames self-oscillation (decided) |
| P4 | Tone | bipolar DJ filter in the loop: noon flat, CCW darken (→ ~800 Hz LP), CW brighten (→ ~1.5 kHz HP); compounds per repeat |
| P5 | Wow/Flutter | 0 → capped tasteful maximum |
| P6 | Microspeed | bipolar, noon off: CCW = V/Oct depth on tape speed, CW = sub-sine depth on tape speed |

## Page 2 — Grain (up to 12 grains, stereo)
| Pot | Control | Behaviour |
|---|---|---|
| P1 | Mix | dry → 50/50 (noon) → 25/75; second half crossfades grain record source from input to tape output |
| P2 | Clock ÷/× | stepped ⅛, ⅙, ¼, ⅓, ⅜, ½, ⅔, ¾, **1**, 3/2, 2, 4 (own set, denser/shorter than tape) |
| P3 | Size | ~5 ms → 4 s, exponential (CV: J5) |
| P4 | Count | 12 steps; triggers per delay clock `1,2,2,2,3,3,3,4,4,5,5,6` |
| P5 | Envelope | continuous morph: square → LPG → triangle → slow-attack/fast-release |
| P6 | Speed | ÷12 ← 1× (noon, small dead zone) → ×12, exponential, no pitch change |

## Page 3 — Stack → dual-notch filter
Oscillators: quadrature sines (dominant) + 6-voice supersaw (supporting),
smooth not aggressive. Sub **sine** 2 octaves below the fundamental.
Filter: characterful SVF, very low resonance, two 6 dB/oct notches.

| Pot | Control | Behaviour |
|---|---|---|
| P1 | Tuning | continuous (no snapping) so it can be matched to any V/Oct source; C4 at noon, ±2 octaves (C2–C6); J4 V/Oct adds |
| P2 | Detune | supersaw spread + slight sine drift |
| P3 | Level | 0 → full |
| P4 | Cutoff | bipolar: CCW mono notches ↓, noon full spectrum, CW stereo notches ↓ with highs wide / lows centred |
| P5 | Spacing | notches together → ~3 octaves apart; sub-sine modulates around the knob |
| P6 | Feast | 0 → all grain output through the notch filter |

## Hidden page — B2 held (any page)
| Pot | Control |
|---|---|
| P1 | Input gain |
| P2 | Internal tempo (when J3 unpatched) |
| P3 | Stereo spread (tape ping-pong amount, grain panning) |
| P4 | Grain spray — random buffer position (CV: J6) |
| P5 | Sub-sine → notch spacing depth |
| P6 | Output level |

## Hidden page — B3 held (Stack page only)
| Pot | Control |
|---|---|
| P1 | Grain pitch, ±24 semitones, semitone steps offset by the stack tuning's fine offset so incoming audio can be pitched to the stack; with the B3 toggle on, V/Oct moves grains along with the stack |
| P2 | Grain pitch randomness |
| P3 | Sine/saw balance: CCW all sine → CW ~50/50, default ~70/30 (sines always dominate) |
| P4 | Octave layers, 3 steps: unison → −12/0 → −12/0/+12. Applies to the stack; grains that Granular Feast (3-F) pulls into the filter get the same octave layers, in proportion to Feast. Grains on the normal grain output are unaffected |
| P5 | J7 function (5 options, see Jacks) |
| P6 | J8 function (5 options, see Jacks) |

## Jacks (decided)
| Jack | Dir | Function |
|---|---|---|
| J1/J2 | in | audio L/R |
| J3 | in | clock (internal tempo when unpatched) |
| J4 | in | V/Oct |
| J5 | in | grain size CV |
| J6 | in | grain spray CV |
| J7 | in/out | assignable, 5 options (below) |
| J8 | in/out | assignable, 5 options (below) |
| J9/J10 | out | audio L/R |

J7/J8 options (selected on the B3 hidden page, P5/P6):
1. CV in → feedback
2. CV in → cutoff
3. CV in → notch spacing
4. Sub-oscillator out (±5 V)
5. Grain trigger out (gate per grain)

The jack switches between input and output live (SDK
`EnableCvOutput` / `DisableCvOutput`). J7/J8 are the STM32 DACs, the fast
outputs on the board.

## Buttons (decided)
- B1: tap = next page (Tape → Grain → Stack); hold + move pot =
  param-lock record, main three pages only (rarely used)
- B2: tap = freeze (Tape / Grain) or stack mute (Stack, still modulates);
  hold = B2 hidden page
- B3: tap = Tape repeat mode (normal / ping-pong) · Grain direction
  (fwd / rev / random) · Stack: V/Oct also drives grain pitch (toggle);
  hold on Stack page = B3 hidden page
- B2+B3 hold 2 s = Settings (SDK reserved)

## Mixing (decided)
- 1-A and 2-A each set their own layer's wet level.
- Dry level = the lower of the two knobs' dry amounts.
- Stack output (3-C level) is added on top.

## Grain buffer character (decided)
- Grain buffer only; tape stays smooth and dark.
- Built in, no setting: 12-bit linear quantize on write, recorded at
  24 kHz (decimate by 2, within the S950's variable-rate range) with a
  fixed anti-alias low-pass (~10 kHz) before decimation. Constants kept in
  one place so they can be retuned by ear.
- Side benefit: half the memory bandwidth and twice the buffer length.

## Bipolar cutoff (decided)
CW stereo mode keeps content below a ~150–250 Hz crossover identical in
L/R. Quadrature sines may feed sine→L / cosine→R in CW mode for width.

## Param-lock (decided)
Main three pages only (18 slots), 16 s default length. Low priority.

## Ideas parked for later
- Dedicated sub-oscillator output jack (user likes it; currently available
  as J7/J8 option 4).
- Freeze gate input (dropped in favour of grain size / spray CV).

## Step 5 watch-list
- First build: DTCM 83 % (stack shares DTCM, ~19 KB headroom), code
  SRAM 61 %, SDRAM 14 % (tape 6.5 MB + grain 2.9 MB buffers).
- B3 on the Stack page is both a tap toggle and a hold-for-page; a hold
  without turning a knob still counts as a tap (SDK release contract).
- J7/J8 outputs are written per audio block (24 samples → 2 kHz), so the
  sub-osc out is stair-stepped at audio pitches; fine as modulation. A
  per-sample DMA path to the STM DAC would fix it if needed.
- Jack-function selectors live on a pot page, so they are saved per preset.
- Feast octave layers: worst case is 12 grains × 3 octave reads (−12/0/+12),
  i.e. up to 36 grain reads. Only needed when Feast > 0 and octaves ≠
  unison; skip the extra reads otherwise. Budget this in the CPU test.

## LEDs (step 4 — decided, to be tuned on hardware)
General:
- Brightness 30 % (SDK default; Settings brightness control keeps it
  adjustable, default 0.30).
- Base fill always shows the real value including CV; animation is driven
  by live DSP state. SDK catch indicator on top after page changes.
- Page colours: Tape amber · Grain cyan · Stack violet · B2 hidden warm
  white · B3 hidden lime. B1 shows the current page colour.
- Animations stay on for now; revisit after hardware testing.

Tape (amber):
- P1 Mix: amber fill, breathes with repeat level
- P2 Clock: 11-step selector, ping per delay tap; bottom pip flashes on
  incoming clock (dimmer on internal tempo)
- P3 Feedback: amber fill → red past 100 %; fading light travels per repeat
- P4 Tone: centre-out, dark red left / pale gold right
- P5 Wow/Flutter: amber fill, drifting ripple from the real wow/flutter
- P6 Microspeed: centre-out, lime left (V/Oct) / violet right (sub);
  dot at the live speed offset

Grain (cyan):
- P1 Mix: fade cyan → white (noon) → amber (tape feeding in)
- P2 Clock: 12-step selector, ping per grain-delay tick
- P3 Size: cyan fill, dot sweeping each grain's length
- P4 Count: 12-step selector, sparkle per grain trigger
- P5 Envelope: colour fade across the 4 shapes, brightness follows the
  latest grain envelope
- P6 Speed: centre-out, deep blue left / bright cyan right; bottom pip
  white at 1×; dot laps at the real scan speed

Stack (violet):
- P1 Tuning: centre-out from C4, dim octave marks C2–C6, dot at real pitch
  incl. V/Oct
- P2 Detune: violet fill, shimmer speeds up with detune
- P3 Level: violet fill, pulses with stack output
- P4 Cutoff: centre-out, violet left (mono) / pink right (stereo), white
  centre = full spectrum
- P5 Spacing: dim fill + two dots at live notch positions (move with sub)
- P6 Feast: fade violet → cyan, sparkle per stolen grain

B2 hidden (warm white): P1 gain fill + red clip flash · P2 tempo fill +
beat dot, dimmed when J3 patched · P3 spread fill + bouncing dot at delay
rate · P4 spray fill, sparkle density = spray · P5 sub depth fill + dot
bouncing with the sub-sine · P6 output fill + green/yellow/red meter

B3 hidden (lime): P1 grain pitch centre-out + octave marks, bottom pip at
0 st · P2 randomness fill + sparkle · P3 sine/saw fade pale blue → orange ·
P4 octave layers 3-step selector · P5/P6 J7/J8 5-step selector, each option
in its target's colour (feedback amber, cutoff violet, spacing pink, sub
out deep violet, trigger out cyan)

Buttons:
- B1: top = page colour; bottom = page colour pulsing on each clock beat
- B2: top = this page's freeze/mute (dim page colour off, bright white
  breathing on); bottom = lit when the *other* layer is frozen
- B3: mode colour — Tape normal amber / ping-pong amber↔white;
  Grain fwd cyan / rev magenta / random flicker; Stack V/Oct→grains
  off dim / on lime
- While B2/B3 is held, its LEDs show that hidden page's colour.
- Settings mode: SDK display.

Startup sweep (~2 s, after hw.Init, before the control loop):
- Must look different from the bootloader's warm-white comet, which runs
  in the 2 s window before the app starts.
- A violet comet travels the rings in a serpentine
  P1 → P2 → P4 → P3 → P5 → P6, leaving delayed "copies" behind it in
  amber then cyan (echoes, like the delay itself).
- Buttons light B1 → B2 → B3 in amber, cyan, violet as it passes.
- All rings flash briefly in their page colour and fade into the live UI.
- Audio starts immediately; only the knobs wait for the sweep.

## Editor jack names (2026-11-19)
All ten jacks are named in the Hermetic Modular editor: J1/J2 In L/R,
J3 Clock In, J4 V/Oct In, J5 Grain Size CV, J6 Grain Spray CV, J9/J10
Out L/R. J7/J8 show their current function ("J7: Sub-Osc Out" etc.) and
update live when the B3-page selector changes (descriptor refresh).

## Startup defaults (2026-11-19, under review page by page)
Used when no preset is saved; also the editor's factory values. Pots
pick a value up once turned through it.
- Tape (confirmed): mix 50 %, clock ×1, feedback 25 %, tone flat, wow 10 %,
  micro off, freeze off, repeat mode normal
- Grain (confirmed): mix 25 %, clock ×1, size 1 s, count 6, envelope 0
  (square), speed 1×, freeze off, direction forward; spray 0 %, pitch
  random 0 %, grain pitch 0 st
- Stack: C4, detune 25 %, level 0, cutoff open, spacing 1 oct, feast 0
- Global: gain 1×, 120 BPM, spread 50 %, spray 10 %, sub depth 30 %, out 1×
- B3 page: pitch 0, random 0, 70/30 sine/saw, unison, J7 feedback CV,
  J8 cutoff CV
- Buttons: all off / normal / forward

## Page review — Tape (2026-11-19)
User: Mix, Feedback, Tone, Wow/Flutter, B2 freeze, B3 repeat mode are right.
- Microspeed CCW: V/Oct scaling now 1/3 V/oct (4 st per volt at full CCW),
  cubic taper so it is gentler near the centre. CW unchanged.
- Clock: at ×1 a repeat should land on every clock pulse. User heard ~6
  repeats per 4 beats at 88 BPM, which points to the clock not being
  detected (internal 120 BPM). Thresholds lowered to +1.5 V / +0.7 V;
  new gauges "Clock edges seen (J3)" and "J3 volts" to confirm on hardware.

## General changes (2026-11-19)
- Centre detents ±6 % of travel (was ±1–3 %) on Tone, Microspeed, Grain
  Speed, Cutoff, Grain Pitch, Input Gain, Output Level (Tuning stays free).
  Inside the detent the ring shows a 3-LED white bar at 12 o'clock.
- J2 normalled from J1: J2 silent (< −60 dBFS for 10 s) while J1 has
  signal → J1 copied to J2; J2 above −50 dBFS takes over. Gauge
  "J1 copied to J2". The editor shows J2 as normalled from J1.
- CV inputs J5–J8: ±5 V now spans the full knob range (this board reads
  ±5 V as 0.25..0.75, so the routes use ×2).
- Tempo (B2 page, P2) follows an external clock 1:1: its stored value is
  set to the incoming BPM (shown on the ring and in the editor), and the
  internal clock keeps that tempo if the cable is pulled. The ring blinks
  on every beat (bright white = J3 clock, warm white = internal).
- Open: user's clock through a buffered mult read as "External clock: No"
  at the old +3 V threshold; retest with +1.5 V.

## Page review — Grain (2026-11-19)
User: Mix, Size, B2 freeze, B3 direction are right.
- Clock: ratios now 1/32, 1/24, 1/16, 1/8, 1/6, 1/4, 1/3, 3/8, 1/2, 2/3, 3/4,
  **1**, 3/2, 2, 4 (15 steps). The grain clock (clock × ratio) sets how far
  back the grains read and the trigger rhythm (Count → triggers per tick).
- Count: 1–36. Triggers per grain-clock tick: 1,2,2,2,3,3,3,4,4,5,5,6 up to
  12, then +1 per 4 grains, reaching 12 at 36. Ring is a level fill.
- Speed is now true per-grain time-stretch: a grain covers `size` seconds
  of audio and plays it in size/speed seconds at the same pitch (two
  crossfading read heads, 30 ms window, inside each grain). Stepped:
  1/16, 1/8, 1/4, 1/3, 1/2, 2/3, **1**, 3/2, 2, 3, 4, 6, 8; 1× in the centre
  detent. The scan playhead also moves at `speed`. Very slow grains that
  would be lapped by the 30 s record buffer are shortened to fit.
- Envelope: automatic curve on top of the morph (additive, clamped
  0.5–2.0, >1 more exponential/shorter, <1 logarithmic/fuller):
  base 0.85; +0.15 per octave the grain clock is faster than 0.5 s;
  −0.12 per octave the size is above 250 ms (+ below it); +0.25 × (count−6)/30.
  LPG decay lengthened. The square station is unaffected (stays square).
- CPU: estimated M7 load (ARM64 instructions × ~1.56 / 240 k): idle ≈23 %,
  12 grains ≈46 %, 24 ≈68 %, 36 ≈90 %; time-stretched (speed ≠ 1 or pitch
  ≠ 0) 12 ≈62 %, 24 ≈100 %+. The governor caps simultaneous grains
  (trims at 80 % average, cuts 25 % on any block above 95 %, never below
  4); rhythm is unchanged, only overlap is limited.
- Grain reads are now drop-sample (no interpolation, S950 style) and each
  grain renders a whole 24-sample block at a time. Estimated M7 load
  after this: idle ≈19 %, 12 grains ≈39 %, 24 ≈58 %, 36 ≈77 %;
  time-stretched 12 ≈52 %, 24 ≈84 %, 36 ≈117 % (governor caps to ~28).
  Everything maxed incl. Feast octaves ≈177 % → governor limits overlap.

## Layout v2 — Matriarch accompaniment (2026-11-19)
Simulacrum now accompanies a Moog Matriarch (up to 4 paraphonic voices).

Pages (B1 cycles Tape → Grain → Stack; hidden pages while a button is held):
| Page | P1 | P2 | P3 | P4 | P5 | P6 |
|---|---|---|---|---|---|---|
| Stack (violet) | Level 0 % | Tuning C4 | Detune 10 % | Cutoff (open) | Attack 0 % | Release 0 % |
| Global, B2 held (warm white) | Input Gain 1× | Output Level 1× | Spread 25 % | Grains → Filter 25 % | Sub → Spacing 25 % | CV6 Function (cutoff CV) |
| Grain Parameters, B3 held on Grain (blue) | Grain Pitch 0 st | Pitch Random 0 % | Grain Spray 0 % | dark | dark | dark |
| Stack Parameters, B3 held on Stack (green) | Sine/Saw noon | Octaves unison | Voices 1 | Spacing 1 oct | dark | dark |
| Settings page 2 "Clock" (B2+B3 2 s, then B1) | Tempo 120 BPM | | | | | |

Buttons: Stack B2 tap = V/Oct → grains, Stack B3 tap = mute (swapped).
B3 reaches a different hidden page on Grain and Stack through two
"routed" views of B3 (the SDK allows one hold-page per button); a hold
that turned a knob does not also fire the tap.

Jacks: J3 CV1 Clock · J4 CV2 V/Oct · J5 CV3 Grain Size · J6 CV4 Gate/Trigger
· J7 CV5 Grain Spray · J8 CV6 assignable: Feedback CV, Cutoff CV, Spacing
CV, Envelopes CV (attack + release together), Sub-Osc Out, Grain Trigger
Out. Editor names follow ("CV6: Sub-Osc Out" etc.).

Notes (Rings-like): no drone. J6 has no jack-detect switch, so notes only
play from gates/triggers on J6. A rising gate starts a note 2 ms later
(V/Oct settle); while the gate is held, a V/Oct move > 25 mV that stays
3 ms starts another note; earlier notes keep ringing up to Voices (1–8,
free voice first, then oldest releasing, then oldest). Gate held: attack
→ decay over 2/3 of the release time → sustain 50 %. Gate released: all
notes release. A short trigger: attack, then release.

Envelopes: Attack 1 ms → 4 s, exponential → linear as CW. Release
3 ms → 8 s, semi-log → linear → slightly exponential. Warm soft-clip VCA
per voice. Saws per voice drop 6 → 4 → 3 as Voices rises (CPU).

Detune: sine-pair drift up to ~20 cents (was ~5).

Filter (reworked): two resonant biquad notches, lower at Cutoff, upper
Spacing octaves above (swept by the sub sine). Zeros on the unit circle;
poles at 1.2× with Q 4, giving each dip a ~+5 dB resonant shoulder a
third-octave up — formant-ish, not vowel-like. No overall tilt beyond
about ±3 dB. CCW mono, CW stereo with centred lows, as before.

Grains → Filter is now independent of the grain mix (heard even with
the grain layer down) and slightly louder.

Tempo moved to Settings page 2. When an external clock stops, the
internal clock keeps its last tempo until the Tempo setting is moved.

CPU estimate (ARM64 instr × 1.56 / 240 k): idle ≈19 %, 1 stack voice
≈27 %, 4 ≈38 %, 8 ≈51 % (octaves + saws on). The governor trims grains
when the total runs high. Fast RAM (DTCM) 89 %.

## Clock filter (2026-11-19)
Incoming clock: median of the last 5 pulse intervals → lands on a whole
BPM, changing only when the estimate is more than 0.6 BPM away → the
tempo in use glides there (τ 0.4 s; 0.12 s for jumps over 10 %). A tempo
jump of more than 25 % flushes the history so it is followed quickly.
The internal Tempo setting also lands on whole BPM and glides. Simulated
with ±1 ms jitter: 88, 88.4 and 133 BPM clocks settle on 88, 88 and 133
with no flicker. The J2 copy now waits for 10 s of silence.

## Notes and filter fixes (2026-11-19, after hardware test)
- Atonal/stacking voices fixed. Cause: a note's pitch was frozen 2 ms
  after the gate from a still-smoothing V/Oct (landing between notes),
  and slews/glides fired extra notes at in-between pitches. Now a note
  takes the V/Oct only once it has been still within ±4 mV for 3 ms
  (average of that window; at most 10 ms after the gate), and glides
  only make a note where they land.
- Independent envelopes: each new note sends the earlier notes into
  their own release (Rings-like ring-out); Voices caps how many overlap.
  This replaces "all notes sustain while the gate is held".
- Filter "happy medium": each notch = wide, softly driven SVF notch
  (Q 0.8) + resonant shoulder (band-pass Q 3 at 1.7×, added back ×1.2).
  Measured: notches −24…−27 dB, shoulder +2.3 dB, lows −3 dB an octave
  below, no treble tilt (6 kHz −0.4 dB). Between the original (1.4 oct,
  no resonance) and the narrow resonant version (0.4 oct, +5 dB, bass
  loss).
- Notes ring out (confirmed by user).

## CPU + grain smoothness pass (2026-11-19, after 113 overruns on hardware)
Hardware: avg ~68 %, peak 107 %, 113 overruns, grain cap at 4 with slow
grains. The M7 runs this dependent float code ~3× slower per instruction
than first assumed. Changes:
- Tape: read position kept as integer + fraction (a float position at
  ~10^6 samples only resolved 1/16 sample — a grit source); buffer is a
  power of two (2^20 = 21.8 s) with mask wrapping; wow/flutter computed
  per block and interpolated; second read head only while pitch-shifting.
- Division-free soft clip everywhere (tape loop, VCAs, filter, output).
- Grain anti-alias filter 4-pole → 2-pole (9 kHz).
- Envelope table rebuilt 32 points per block into a spare table (a full
  rebuild in one block, e.g. during a tempo glide, caused CPU spikes).
- Governor may now go down to 2 grains.
- Host instruction counts per block: idle 29.7k → 16.4k, 12 grains
  59k → 45k, 12 stretched 79k → 64k, 4 stack voices 42.5k → 29.5k.
- New gauges: CPU tape %, grains %, stack + filter %, input/mix/rest %.
Grain sound:
- Soft edges on every grain: raised-cosine, 8 % of the grain, 8–40 ms
  (was 1 ms) — removes clicks/static in dense or slow grains.
- Head crossfade window scales with the grain: a quarter of its audio,
  10–60 ms (was fixed 30 ms) — smoother slow-speed overlaps.
- Auto-spread with Count: start-position jitter up to ±200 ms and +0.4
  pan spread at 36 grains (scaled from 1).
- Stack decay (gate held) to the 50 % sustain takes at least 1 s, and follows the release time when that is longer (up to 8 s). Release itself can still be shorter than 1 s.

## CPU pass 2 + filter restored (2026-11-19)
Hardware: avg ~75 %, peak 101.6 %, overruns continuing, grain cap 3–5.
Profile of a typical heavy patch (tape on, 12 slow grains, 4 notes):
grains 69 % of the audio work, stack + filter 15 %, tape 8 %, rest 8 %.
- Grains now render at 24 kHz (the buffer's own rate) and are linearly
  interpolated up to 48 kHz before the 11 kHz smoothing filter: half the
  per-grain work, same sound. Heavy patch 83k → 54k instructions per
  block (−35 %); 12 grains 45k → 31k; 12 stretched 64k → 41k. Grains are
  now 56 % of the heavy patch.
- Governor keeps headroom: trims when the average passes 68 % (was 80 %),
  cuts 25 % when a single block passes 88 % (was 95 %), recovers under
  52 %. Fewer overruns; the grain cap will drop a little sooner.
- Filter: the original first-build dual notch is back (two wide SVF
  notches, Q 0.55, soft saturation inside, bipolar mono/stereo cutoff,
  spacing 0–3 oct + sub-sine). The resonant-notch experiments are gone.
- Saturation curve back to the smooth tanh-style curve everywhere (the
  cheaper cubic clip had a hard corner that added harshness).

## Refinements (2026-11-19)
- Stack voices: key-tracked wide band-pass per voice and channel (1-pole
  high-pass an octave below the note, 2-pole low-pass ~5 octaves above),
  so the stack sits back and the filter follows every note.
- Grain Clock: 20 steps 1/24, 1/16, 1/12, 1/8, 1/6, 1/4, 1/3, 1/2, 2/3,
  3/4, **1**, 3/2, 2, 3, 4, 6, 8, 12, 16, 24.
- Cutoff: log-interpolated anchors (just past the detent 10 kHz → 3 kHz
  at 15 % → 400 Hz at 55 % → 120 Hz at 80 % → 35 Hz), so 3 kHz–400 Hz
  takes the travel nearest the centre. Leaving the centre is gradual:
  the filter fades in over the first 15 % each way; CW stereo width
  (sine/cosine, L/R split, centred lows) fades in over the first 25 %.
- Attack / Release: power-law tapers, fine first half, larger steps in
  the second (attack 1 ms + 4 s·a², release 3 ms + 8 s·r^2.415):
  ¼ ≈ 0.25 s / 0.28 s, noon 1 s / 1.5 s, ¾ ≈ 2.25 s / 4 s, full 4 s / 8 s.

## Refinements 2 (2026-11-19)
- Stack band-pass replaced (too strong): per voice, a gentle 1-pole
  high-pass two octaves below the note; on the summed stack, a
  ladder-style 4-pole low-pass ~5 octaves above the newest note
  (6–18 kHz, glides 20 ms on note changes) with soft-clipped, lightly
  resonant feedback (k = 0.7 of 4, unity passband) — trims only the
  highest highs with a little Moog-ish character. Test: 64th harmonic
  −39 dB vs the 8th; mids untouched.
- Cutoff and Spacing glide in the DSP over ~20 ms: the panel updates
  every ~16 ms, and the filter used to jump to each new value (the
  remaining jumpiness around the centre).
- Short grains lifted: each grain's gain = 1 / the peak its envelope can
  reach after the soft edges (≤ +12 dB); the LPG opens with the lifted
  level too. Test: a 20 ms LPG grain peaks at 0.44 vs 0.52 for 500 ms
  (was 0.29).
- Grain make-up gain capped at +6 dB to start (was +12).
- Stack voices are slightly LPG-style: each voice's own 1-pole low-pass
  follows its envelope (≈ 3× the note at silence → 21× at the 50 %
  sustain → 40× at full), so notes darken gently as they fade.
- Microspeed (Tape P6, CW) now uses its own sub sine four octaves below
  the newest note (was two). Notch spacing and the CV6 sub-osc out stay
  two octaves down.
