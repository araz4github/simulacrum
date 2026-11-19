/**
 * stack.h — Layer 3: up to 8 ringing voices of quadrature sines +
 * supersaw, a warm soft-clipping VCA per voice, then a resonant
 * pseudo-formant notch filter.
 *
 *   per voice:  quad sines (2 slightly detuned I/Q pairs, dominant)
 *             + supersaw (6 / 4 / 3 BLEP saws as polyphony rises)
 *             + octave layers (−12 / +12)
 *             → attack / decay / sustain / release envelope
 *             → warm VCA (soft clip)
 *                                   Σ voices × Level ─┐
 *   Grains → Filter (stereo) ─────────────────────► (+) ─► formant filter
 *
 *   sub sine (newest note / 4) ─► spacing (bipolar), tape FM, J8 out
 *
 * Notes (Rings-like): with no gate ever seen on J6 the stack drones one
 * voice that follows V/Oct.  Once J6 has gated, each gate (and each V/Oct
 * change while the gate is held) starts a new note on a free voice — the
 * oldest is stolen when all `voices` are busy — and earlier notes keep
 * ringing.  Gate held: attack → decay (2/3 of release) to 50 % sustain.
 * Gate released: every note releases.
 *
 * Filter (original design, restored): two wide, gentle SVF notches
 * with very low resonance and soft saturation inside — lower at Cutoff,
 * upper Spacing octaves above it (swept by the sub sine).  Cutoff is
 * bipolar: noon = parked above the audio band; CCW mono; CW stereo
 * (L/R split, lows held centre).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include "dsp_util.h"
#include "sim_shared.h"

namespace sim {

struct StackControls
{
    float    tune_oct;     ///< −2..+2 around C4
    float    detune;       ///< 0..1
    float    sine_saw;     ///< 0..1 knob: 0 all sine, 1 = 50/50
    uint32_t octaves;      ///< 0 unison, 1 −12/0, 2 −12/0/+12
    float    level;        ///< 0..1 (0 when muted)
    float    cutoff;       ///< 0..1 bipolar
    float    spacing;      ///< 0..1
    float    sub_depth;    ///< 0..1 sub → spacing
    float    attack;       ///< 0..1 (incl. CV)
    float    release;      ///< 0..1 (incl. CV)
    uint32_t voices;       ///< 1..8
    bool     gate;
    bool     gated_mode;   ///< false = drone
    uint32_t note_seq;
    float    note_volts;
    float    voct_live;    ///< live V/Oct (drone follows it)
};

class Stack
{
  public:
    void Init(float fs);

    /** Handle note / gate events for this block; call before LeadHz(). */
    void BeginBlock(const StackControls& c, size_t n);

    /** Pitch of the newest note (drone: live V/Oct), Hz. */
    float LeadHz() const { return lead_hz_; }
    float LeadOct() const { return lead_oct_; }

    /** Fill @p sub with the sub sine (lead / 4) for this block. */
    void ComputeSub(float* sub, size_t n);

    /** Fill @p sub with the deep sub sine (lead / 16, four octaves down)
     *  that modulates the tape microspeed. */
    void ComputeDeepSub(float* sub, size_t n);

    /** Voices + filter.  @p feast_* are already scaled. */
    void Process(const float* feast_l, const float* feast_r, const float* sub,
                 float* out_l, float* out_r, size_t n, const StackControls& c);

    float    Env() const       { return env_.y; }
    float    NotchLo01() const { return notch_lo01_; }
    float    NotchHi01() const { return notch_hi01_; }
    float    SubPhase() const  { return sub_phase_; }
    uint32_t Sounding() const  { return sounding_; }
    float    VoiceEnv(uint32_t v) const { return v < kMaxStackVoices ? voices_[v].env : 0.f; }

  private:
    enum class Stage : uint8_t { Idle, Attack, Decay, Sustain, Hold, Release };

    struct Voice
    {
        Stage    stage = Stage::Idle;
        float    volts = 0.f;
        float    env   = 0.f;   ///< level at the end of the last block
        float    x     = 0.f;   ///< stage progress 0..1
        float    from  = 0.f;   ///< level the stage started from
        uint32_t age   = 0;
        float    sin_a = 0.f, sin_b = 0.37f, sin_lo = 0.f, sin_hi = 0.f;
        BlepSaw  saws[6];
        BlepSaw  saw_lo, saw_hi;
        /* Gentle key-tracked high-pass (per channel), two octaves below
         * the note: trims only the lowest lows. */
        float    hp[2] = {0.f, 0.f};
        /* LPG-style low-pass that follows the voice's own envelope. */
        float    lpg[2] = {0.f, 0.f};
    };

    void  StartNote(uint32_t v, float volts);
    void  ReleaseVoice(Voice& v);
    float StepEnvelope(Voice& v, float samples, bool gate) const;

    float fs_ = 48000.f;

    Voice    voices_[kMaxStackVoices];
    uint32_t last_note_seq_ = 0;
    bool     last_gate_     = false;
    bool     was_gated_     = false;
    uint32_t age_counter_   = 0;
    float    lead_hz_       = 261.6f;
    float    lead_oct_      = 0.f;
    uint32_t sounding_      = 0;

    /* Envelope timing for this block. */
    float ta_ = 0.001f, pa_ = 3.f, tr_ = 0.003f, q_ = 0.6f;

    float sub_phase_ = 0.f;
    float deep_phase_ = 0.f;

    Svf      notch_[2][2];           /* [channel][lo, hi]: wide, driven notches */
    OnePole  xo_mid_, xo_l_, xo_r_;  /* stereo-mode crossover */

    /* Ladder-style low-pass on the summed stack (per channel): four
     * one-poles with a soft-clipped, lightly resonant feedback — trims
     * only the highest highs, tracks the newest note, a little Moog-ish
     * character.  Cutoff glides with note changes. */
    struct Ladder { float y[4] = {0.f, 0.f, 0.f, 0.f}; };
    Ladder  ladder_[2];
    float   ladder_hz_ = 8000.f;

    EnvFollow env_;
    float notch_lo01_ = 1.f, notch_hi01_ = 1.f;
};

} // namespace sim
