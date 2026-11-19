/**
 * sim_shared.h — state shared between the control loop and the audio
 * callback.
 *
 * Params:    written by the control loop once per frame (and the 1 ms
 *            poll for clock / V/Oct), read by the audio callback.
 *            Every field is a single aligned 32-bit word, so reads are
 *            tear-free on the Cortex-M7; the DSP smooths what needs it.
 * Telemetry: written by the audio callback once per block, read by the
 *            LED ring callbacks ("animate from truth").
 */

#pragma once

#include <cstdint>

namespace sim {

constexpr float kSampleRate = 48000.f;

/** Centre detent for bipolar knobs: ±6 % of travel (≈ ±18° of the pot). */
constexpr float kDetent = 0.06f;

inline bool InDetent(float norm) { return norm > 0.5f - kDetent && norm < 0.5f + kDetent; }

/** Bipolar knob with the centre detent → −1..+1 (exactly 0 in the detent). */
inline float BipolarDetent(float norm)
{
    const float x = norm - 0.5f;
    if (x > -kDetent && x < kDetent) return 0.f;
    const float s = x > 0.f ? 1.f : -1.f;
    const float a = (x > 0.f ? x : -x) - kDetent;
    return s * a / (0.5f - kDetent);
}

/** Tape / Grain Mix knobs: the first 20 % of travel is the layer's level
 *  (silent → full, fully dry); the remaining 80 % is the original
 *  dry → wet mix law. */
constexpr float kMixLevelZone = 0.2f;

struct MixZone
{
    float level;   ///< 0..1 layer level (audio taper), 1 above the zone
    float mix;     ///< 0..1 original mix-law position, 0 inside the zone
};

inline MixZone SplitMixKnob(float norm)
{
    if (norm < 0.f) norm = 0.f;
    if (norm > 1.f) norm = 1.f;
    if (norm < kMixLevelZone)
    {
        const float z = norm / kMixLevelZone;
        return {z * z, 0.f};
    }
    return {1.f, (norm - kMixLevelZone) / (1.f - kMixLevelZone)};
}

/* Function choices for the assignable jack J8 (Global page, P6). */
enum JackFn : uint8_t
{
    kJackCvFeedback  = 0,
    kJackCvCutoff    = 1,
    kJackCvSpacing   = 2,
    kJackCvEnvelopes = 3,   ///< attack + release together
    kJackSubOut      = 4,
    kJackTrigOut     = 5,
    kNumJackFns      = 6,
};

constexpr uint32_t kMaxStackVoices = 8u;

struct Params
{
    /* ── Clock ─────────────────────────────────────────────────────── */
    float clock_period_s = 0.5f;   ///< one clock pulse (= one beat)
    uint32_t clock_epoch = 0;      ///< bumps on every external clock edge

    /* ── Tape (page 1) ─────────────────────────────────────────────── */
    float tape_mix      = 0.f;     ///< 0..1 dry → wet
    float tape_ratio    = 1.f;     ///< × clock period
    float tape_feedback = 0.4f;    ///< 0..1.05
    float tape_tone     = 0.5f;    ///< 0..1, 0.5 neutral
    float tape_wow      = 0.2f;    ///< 0..1
    float tape_micro    = 0.5f;    ///< 0..1, 0.5 off; <0.5 V/Oct, >0.5 sub
    uint32_t tape_freeze   = 0;
    uint32_t tape_pingpong = 0;

    /* ── Grain (page 2) ────────────────────────────────────────────── */
    float grain_mix     = 0.f;     ///< 0..1 (see spec 2-A)
    float grain_ratio   = 1.f;     ///< × clock period
    float grain_size_s  = 0.25f;   ///< seconds
    uint32_t grain_count = 6;      ///< 1..36
    float grain_shape   = 0.33f;   ///< 0..1 envelope morph
    float grain_speed   = 1.f;     ///< playback speed in grains 1/16..8
    uint32_t grain_freeze = 0;
    uint32_t grain_dir    = 0;     ///< 0 fwd, 1 rev, 2 random

    /* ── Stack (page 3) ────────────────────────────────────────────── */
    float stack_tune_oct = 0.f;    ///< −2..+2 octaves around C4
    float stack_detune   = 0.2f;   ///< 0..1
    float stack_level    = 0.f;    ///< 0..1
    float stack_cutoff   = 0.5f;   ///< 0..1 bipolar, 0.5 = full spectrum
    float stack_spacing  = 0.3f;   ///< 0..1
    float stack_feast    = 0.f;    ///< 0..1 "Grains → Filter"
    float stack_attack   = 0.f;    ///< 0..1 knob
    float stack_release  = 0.f;    ///< 0..1 knob
    float env_cv         = 0.f;    ///< J8 "CV → Envelopes", −1..+1
    uint32_t stack_voices = 1;     ///< 1..8 notes ringing at once
    uint32_t stack_mute  = 0;

    /* ── Global (B2 hidden) ────────────────────────────────────────── */
    float in_gain       = 1.f;
    float spread        = 0.5f;    ///< 0..1
    float grain_spray   = 0.f;     ///< 0..1
    float sub_depth     = 0.3f;    ///< sub-sine → notch spacing, 0..1
    float out_level     = 1.f;

    /* ── Grain pitch / stack extras (B3 hidden) ────────────────────── */
    float grain_pitch_st   = 0.f;  ///< −24..+24 (already quantized)
    float grain_pitch_rand = 0.f;  ///< 0..1
    float sine_saw         = 0.6f; ///< 0..1 knob; 0 all sine, 1 = 50/50
    uint32_t octaves       = 0;    ///< 0 unison, 1 −12/0, 2 −12/0/+12
    uint32_t voct_grains   = 0;    ///< B2 toggle on the Stack page

    /* ── Live CV ───────────────────────────────────────────────────── */
    float voct_volts = 0.f;        ///< J4, calibrated volts

    /* ── Notes (J6 gate + J4 V/Oct, detected at 1 ms in the control loop) */
    uint32_t gate        = 0;      ///< J6 held
    uint32_t gated_mode  = 1;      ///< notes need a J6 gate (no drone)
    uint32_t note_seq    = 0;      ///< bumps on every new note
    float    note_volts  = 0.f;    ///< V/Oct of the newest note

    /* ── J8 role (set by the control loop) ─────────────────────────── */
    uint32_t j8_fn = kJackCvCutoff;
};

struct Telemetry
{
    /* Tape */
    float    tape_env        = 0.f;  ///< wet level, 0..1
    float    tape_phase      = 0.f;  ///< position inside the delay period
    uint32_t tape_taps       = 0;    ///< bumps once per delay period
    float    tape_wow        = 0.f;  ///< current wow/flutter, −1..1
    float    tape_speed_st   = 0.f;  ///< current microspeed offset, semitones

    /* Grain */
    uint32_t grain_triggers  = 0;    ///< bumps per grain started
    uint32_t grain_ticks     = 0;    ///< bumps per grain-delay period
    float    grain_progress  = 0.f;  ///< newest grain, 0..1 through its length
    float    grain_env       = 0.f;  ///< newest grain's current envelope
    float    grain_scan      = 0.f;  ///< scan playhead phase 0..1
    uint32_t feast_triggers  = 0;    ///< triggers while Feast > 0

    /* Stack */
    float    stack_env       = 0.f;
    float    stack_pitch_oct = 0.f;  ///< actual pitch, octaves from C4
    float    notch_lo01      = 0.f;  ///< live notch positions, log 0..1
    float    notch_hi01      = 0.f;
    float    sub_phase       = 0.f;  ///< sub-sine phase 0..1
    float    sub_value       = 0.f;  ///< sub-sine −1..1
    uint32_t voices_sounding = 0;    ///< stack voices currently audible
    float    voice_env[kMaxStackVoices] = {};

    /* Output */
    float    out_env         = 0.f;
    float    in_peak         = 0.f;
    bool     mono_in         = false; ///< J1 copied to J2 (J2 silent)

    /* CPU load of the audio callback, 0..1 of the block budget. */
    float    cpu_load        = 0.f;
    float    cpu_peak        = 0.f;   ///< all-time slowest block since boot
    float    cpu_peak_1s     = 0.f;   ///< slowest block in the last second
    uint32_t overruns        = 0;     ///< blocks over 100 % (audible glitch)
    uint32_t voice_cap       = 36;   ///< grains allowed by the CPU governor
    /* Share of the audio block budget per stage (1 s averages). */
    float    prof_tape  = 0.f, prof_grain = 0.f, prof_stack = 0.f, prof_other = 0.f;
};

extern Params    g_params;
extern Telemetry g_tel;

} // namespace sim
