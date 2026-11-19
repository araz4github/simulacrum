/**
 * tape.h — Layer 1: dark, clock-synced stereo tape delay.
 *
 * Signal path per channel:
 *
 *   in ─► (+) ─► write head ─► buffer ─► two-head read ─► playback LP ─► wet
 *          ▲                    (glide, wow/flutter,                 │
 *          │                     sub-sine FM, V/Oct speed)           │
 *          └── × feedback ◄── saturation ◄── DJ tone (bipolar) ◄─────┘
 *
 * The tone filter sits only in the feedback path, so its effect
 * compounds on every repeat.  Freeze loops the buffer unchanged.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include "dsp_util.h"

namespace sim {

struct TapeControls
{
    float delay_s;        ///< target delay (clock period × ratio)
    float feedback;       ///< 0..1.05
    float tone;           ///< 0..1, 0.5 neutral
    float wow;            ///< 0..1
    float voct_shift_st;  ///< steady pitch shift of repeats, semitones
    float sub_dev_st;     ///< sub-sine FM depth, semitones
    float sub_hz;         ///< sub-sine frequency (for constant-deviation FM)
    bool  freeze;
    bool  pingpong;
};

class Tape
{
  public:
    static constexpr float  kMaxDelayS = 16.f;
    static constexpr size_t kBufLen    = 1u << 20;       /* per channel: 21.8 s, power of two */

    void Init(float* buf_l, float* buf_r, float fs);

    /** @param sub per-sample sub-sine values (−1..1), length n. */
    void Process(const float* in_l, const float* in_r,
                 float* wet_l, float* wet_r,
                 const float* sub, size_t n, const TapeControls& c);

    /* Telemetry */
    float Env() const        { return env_.y; }
    float PeriodPhase() const { return period_phase_; }
    uint32_t Taps() const    { return taps_; }
    float WowNow() const     { return wow_now_; }
    float SpeedSt() const    { return speed_st_; }

  private:
    float ReadHermite(const float* buf, float delay) const;
    void  SetTone(float tone);

    float* buf_[2]  = {nullptr, nullptr};
    float  fs_      = 48000.f;
    size_t write_   = 0;

    OnePole delay_slew_;      /* tape-style glide on delay time */
    float   delay_smp_ = 24000.f;

    /* Two-head pitch shifter for steady V/Oct speed offsets. */
    float head_phase_ = 0.5f;
    float window_     = 2400.f;

    /* Wow / flutter */
    float wow_phase_  = 0.f, flutter_phase_ = 0.f;
    OnePole wow_noise_;
    Rng   rng_;
    float wow_now_    = 0.f;

    /* Feedback-path filters, per channel */
    Svf     tone_[2];
    bool    tone_hp_ = false;
    bool    tone_on_ = false;
    OnePole play_lp_[2];          /* playback-head darkness */
    Svf     dc_hp_[2];            /* keeps the loop free of rumble */

    /* Telemetry */
    EnvFollow env_;
    float    period_phase_ = 0.f;
    float    period_count_ = 0.f;
    uint32_t taps_         = 0;
    float    speed_st_     = 0.f;
};

} // namespace sim
