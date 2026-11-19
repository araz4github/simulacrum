/**
 * grain.h — Layer 2: clocked granular delay with S950-style crunch.
 *
 *   record source ─► AA low-pass (4-pole, 10 kHz) ─► ↓2 (24 kHz) ─► 12-bit
 *        │                                                    │
 *   (input ⇄ tape output, per 2-A)                     ring buffer (30 s)
 *                                                             │
 *        up to 36 grains ─────────────────────────────────────┴─► grains
 *
 * Each grain covers `size` seconds of recorded audio.  Speed sets how
 * fast the grain moves through that audio WITHOUT changing pitch:
 *
 *   speed 1×   : a 2 s grain plays 2 s of audio in 2 s
 *   speed 2×   : the same 2 s of audio in 1 s, same pitch
 *   speed 1/2× : the same 2 s of audio in 4 s, same pitch
 *
 * Inside every grain a cursor walks the audio at `speed`; two read heads
 * ride ±15 ms around it at the grain's pitch and crossfade (a per-grain
 * time-stretcher).  When speed and pitch agree — 1×, no transposition —
 * one head sits on the cursor and the grain is a plain read.
 * Speed also moves where successive grains start (the scan playhead),
 * so the whole stream is stretched consistently.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include "dsp_util.h"

namespace sim {

struct GrainControls
{
    float    period_s;       ///< grain delay clock: clock period × ratio
    float    size_s;         ///< audio covered by one grain
    uint32_t count;          ///< max simultaneous grains, 1..36
    uint32_t per_clock;      ///< triggers per grain-delay clock
    uint32_t count_knob;     ///< Count as set (before the CPU governor)
    float    shape;          ///< 0..1 envelope morph
    float    speed;          ///< playback speed within grains, 1/16..8
    float    spray;          ///< 0..1 random start position
    float    pitch_st;       ///< grain transposition, semitones
    float    pitch_rand;     ///< 0..1 random semitone spread
    uint32_t dir;            ///< 0 fwd, 1 rev, 2 random
    float    spread;         ///< 0..1 stereo pan randomisation
    bool     freeze;         ///< stop recording
    float    source_mix;     ///< 0 = record input, 1 = record tape output
    uint32_t octaves;        ///< octave layers for the Feast bus (0..2)
    bool     feast_active;   ///< compute the octave layers at all
    bool     render;         ///< grains audible at all (wet or Feast > 0)
};

class Grains
{
  public:
    static constexpr uint32_t kMaxGrains = 36u;
    static constexpr uint32_t kOctVoices = 8u;      /* voices that get octave layers */
    static constexpr float    kBufRate   = 24000.f; /* S950-ish */
    static constexpr size_t   kBufFrames = 24000u * 30u;   /* 30 s stereo */
    static constexpr size_t   kMaxBlock  = 64u;

    void Init(int16_t* buf, float fs);

    void Process(const float* in_l, const float* in_r,
                 const float* tape_l, const float* tape_r,
                 float* out_l, float* out_r,
                 float* feast_l, float* feast_r,
                 size_t n, const GrainControls& c);

    /* Telemetry / jack outputs */
    uint32_t Triggers() const   { return triggers_; }
    uint32_t Ticks() const      { return ticks_; }
    float    Progress() const   { return newest_progress_; }
    float    EnvNow() const     { return newest_env_; }
    float    ScanPhase() const  { return scan_phase01_; }
    bool     TrigGate() const   { return gate_samples_ > 0u; }
    float    EnvCurve() const   { return env_gamma_; }

  private:
    /** Two crossfading read heads at a pitch rate around the cursor. */
    struct Tap
    {
        float phase = 0.5f;
        float rate  = 0.f;   ///< phase change per output sample
    };

    struct Voice
    {
        bool     active  = false;
        uint32_t c_idx   = 0;      ///< cursor, buffer frame (integer part)
        float    c_frac  = 0.f;
        float    c_inc   = 0.5f;   ///< cursor frames per output sample (signed)
        uint32_t len     = 1;      ///< output samples
        uint32_t t       = 0;
        float    inv_len = 1.f;
        float    gain_l  = 0.7f, gain_r = 0.7f;
        float    lp_l = 0.f, lp_r = 0.f;
        Tap      main, lo, hi;     ///< pitch, −12, +12
        uint32_t start_at = 0;     ///< first sample in the block it plays
        uint32_t edge     = 384;   ///< soft-edge length, samples
        float    win      = 720.f; ///< head crossfade window, frames
        float    lift     = 1.f;   ///< makeup gain for short grains
        float    inv_edge = 1.f / 384.f;
    };

    void StartGrain(const GrainControls& c, float base_off, uint32_t at);
    void BuildEnvTable(float shape, float gamma);
    inline void ReadAt(uint32_t idx, float frac, float off, float& l, float& r) const;
    inline void ReadTap(const Voice& g, Tap& tap, float& l, float& r) const;
    static inline void Advance(uint32_t& idx, float& frac, float inc);

    int16_t* buf_   = nullptr;
    float    fs_    = 48000.f;
    uint32_t write_ = 0;          ///< frame index
    bool     odd_   = false;      ///< decimation phase

    Svf      aa_[2][2];           /* 4th-order Butterworth, per channel */
    Svf      recon_[4];           /* output smoothing: main L/R, octaves L/R */

    Voice    voices_[kMaxGrains];
    Rng      rng_;

    float    trig_phase_ = 0.f;
    float    tick_phase_ = 0.f;
    float    scan_off_   = 0.f;   /* frames, relative to the base delay */
    float    scan_phase01_ = 0.f;

    /* Envelope tables (257 points each): LPG decay, triangle, slow-attack. */
    float    tab_lpg_[257];
    float    tab_tri_[257];
    float    tab_sar_[257];
    float    tab_env_a_[257] = {};  /* current morph + auto curve (double-buffered) */
    float    tab_env_b_[257] = {};
    float*   env_tab_      = tab_env_a_;
    int      build_pos_    = 0;
    float    build_shape_  = 0.f, build_gamma_ = 1.f;
    float    env_shape_    = -1.f;
    float    env_gamma_    = 1.f;
    float    built_gamma_  = -1.f;

    uint32_t triggers_ = 0;
    uint32_t ticks_    = 0;
    float    newest_progress_ = 0.f;
    float    newest_env_      = 0.f;
    int8_t   newest_          = -1;
    uint32_t gate_samples_    = 0;
    float    prev_l_ = 0.f, prev_r_ = 0.f, prev_xl_ = 0.f, prev_xr_ = 0.f;  /* upsampler */
};

/** Grain-count knob (1..36) → triggers per grain-delay clock. */
constexpr uint8_t TriggersPerClock(uint32_t count)
{
    constexpr uint8_t kLow[12] = {1, 2, 2, 2, 3, 3, 3, 4, 4, 5, 5, 6};
    if (count < 1u) count = 1u;
    if (count <= 12u) return kLow[count - 1u];
    /* Above 12 the density grows more gently: one more trigger per four
     * grains, reaching 12 per clock at 36. */
    return static_cast<uint8_t>(6u + (count - 12u) / 4u);
}

} // namespace sim
