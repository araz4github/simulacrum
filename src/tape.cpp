/**
 * tape.cpp — Layer 1 tape delay.  See tape.h for the signal path.
 */

#include "tape.h"

namespace sim {

namespace {

/* Wow / flutter shape.  Expressed as peak delay modulation in samples at
 * 48 kHz, chosen for peak pitch deviation at full knob of roughly
 * wow 0.25 % (≈4 cents) and flutter 0.08 % — audible, never seasick. */
constexpr float kWowHz        = 0.55f;
constexpr float kWowSamples   = 34.f;
constexpr float kFlutterHz    = 7.3f;
constexpr float kFlutterSmp   = 0.85f;
constexpr float kDriftSamples = 18.f;   /* slow random component */

constexpr float kPlaybackLpHz = 7000.f; /* "dark" playback head */
constexpr float kLoopHpHz     = 35.f;
constexpr float kGlideSec     = 0.18f;  /* delay-time change glide */
constexpr float kWindowSmp    = 3600.f; /* 75 ms pitch-shift window */

constexpr uint32_t kMask = static_cast<uint32_t>(Tape::kBufLen - 1u);   /* power of two */

} // namespace

void Tape::Init(float* buf_l, float* buf_r, float fs)
{
    buf_[0] = buf_l;
    buf_[1] = buf_r;
    fs_     = fs;
    for (size_t i = 0; i < kBufLen; i++) buf_l[i] = buf_r[i] = 0.f;

    delay_slew_.SetTime(kGlideSec, fs);
    delay_slew_.y = delay_smp_;
    wow_noise_.SetHz(0.35f, fs);
    for (int ch = 0; ch < 2; ch++)
    {
        play_lp_[ch].SetHz(kPlaybackLpHz, fs);
        dc_hp_[ch].Set(kLoopHpHz, 0.707f, fs);
    }
    env_.Init(0.005f, 0.25f, fs);
    rng_.s = 0x51A7u;
}

float Tape::ReadHermite(const float* buf, float delay) const
{
    /* Integer and fractional parts kept apart: a float position at
     * ~10^6 samples would only resolve 1/16 of a sample (audible grit). */
    const uint32_t di = static_cast<uint32_t>(delay);
    const float    fr = delay - static_cast<float>(di);
    const uint32_t i0 = (static_cast<uint32_t>(write_) - di - 1u) & kMask;
    const float    t  = 1.f - fr;
    return Hermite(buf[(i0 - 1u) & kMask], buf[i0],
                   buf[(i0 + 1u) & kMask], buf[(i0 + 2u) & kMask], t);
}

void Tape::SetTone(float tone)
{
    /* Bipolar DJ filter: CCW low-pass 20 kHz → 800 Hz, CW high-pass
     * 20 Hz → 1.5 kHz, a small dead band at noon is fully transparent. */
    const float d = (tone - 0.5f) * 2.f;
    tone_on_ = fabsf(d) > 0.02f;
    tone_hp_ = d > 0.f;
    const float a = fabsf(d);
    const float fc = tone_hp_ ? 20.f * powf(1500.f / 20.f, a)
                              : 20000.f * powf(800.f / 20000.f, a);
    for (int ch = 0; ch < 2; ch++) tone_[ch].Set(fc, 0.8f, fs_);
}

void Tape::Process(const float* in_l, const float* in_r,
                   float* wet_l, float* wet_r,
                   const float* sub, size_t n, const TapeControls& c)
{
    SetTone(c.tone);

    /* Target delay: fold long clock × ratio products down by octaves so
     * they stay musically related while fitting the buffer. */
    float target = c.delay_s;
    while (target > kMaxDelayS) target *= 0.5f;
    float target_smp = Clamp(target * fs_, 48.f, kMaxDelayS * fs_);

    /* Pitch-shift window never exceeds the delay itself. */
    window_ = Clamp(0.9f * target_smp, 64.f, kWindowSmp);

    const float wow2      = c.wow * c.wow;
    const float wow_inc   = kWowHz / fs_;
    const float kMaxDelaySmp = static_cast<float>(kBufLen - 8u);
    const float flut_inc  = kFlutterHz / fs_;

    /* Steady speed shift (V/Oct) → head phase rate. */
    const float ratio     = exp2f(c.voct_shift_st / 12.f);
    const bool  shifting  = fabsf(c.voct_shift_st) > 0.01f;
    const float head_rate = (1.f - ratio) / window_;
    speed_st_ = c.voct_shift_st + c.sub_dev_st * (sub ? sub[n - 1] : 0.f);

    /* Sub-sine FM: constant pitch deviation regardless of sub pitch. */
    const float sub_ratio = exp2f(c.sub_dev_st / 12.f) - 1.f;
    const float sub_amp   = c.sub_hz > 1.f ? sub_ratio * fs_ / (kTwoPi * c.sub_hz) : 0.f;

    const float fb       = c.freeze ? 1.f : c.feedback;
    const float in_amt   = c.freeze ? 0.f : 1.f;

    /* Wow / flutter at block rate (they move at < 8 Hz), interpolated
     * across the block — saves four sines per sample. */
    auto wow_at = [&](float drift_v) {
        return wow2 * (kWowSamples * Sin01(wow_phase_)
                       + kDriftSamples * 4.f * drift_v
                       + kFlutterSmp * Sin01(flutter_phase_));
    };
    const float wow_a = wow_at(wow_noise_.y);
    wow_phase_     = Wrap01(wow_phase_ + wow_inc * static_cast<float>(n));
    flutter_phase_ = Wrap01(flutter_phase_ + flut_inc * static_cast<float>(n));
    for (size_t k = 0; k < n; k += 8) wow_noise_.Process(rng_.Bi());
    const float wow_b  = wow_at(wow_noise_.y);
    const float wow_dn = (wow_b - wow_a) / static_cast<float>(n);
    wow_now_ = wow_b / (kWowSamples + kDriftSamples + kFlutterSmp);

    for (size_t i = 0; i < n; i++)
    {
        const float wow_mod = wow_a + wow_dn * static_cast<float>(i + 1);
        const float fm = sub ? sub_amp * sub[i] : 0.f;

        delay_smp_ = delay_slew_.Process(target_smp);

        /* ── Two-head read (one head when not pitch-shifting) ────────── */
        if (shifting)
            head_phase_ = Wrap01(head_phase_ + head_rate);
        else /* drift back to the single-head position so delay is exact */
            head_phase_ += 0.0005f * (0.5f - head_phase_);

        const float base = delay_smp_ - 0.5f * window_ + wow_mod + fm;
        const float ph0  = head_phase_;
        const float ph1  = ph0 < 0.5f ? ph0 + 0.5f : ph0 - 0.5f;
        const float tri  = 1.f - fabsf(2.f * ph0 - 1.f);
        const float g0   = tri * tri * (3.f - 2.f * tri);            /* smoothstep */
        const float g1   = 1.f - g0;
        const float d0   = Clamp(base + window_ * ph0, 4.f, kMaxDelaySmp);
        const float d1   = Clamp(base + window_ * ph1, 4.f, kMaxDelaySmp);
        const bool  two  = g1 > 0.001f;

        float rd[2];
        for (int ch = 0; ch < 2; ch++)
        {
            const float* b = buf_[ch];
            const float  r = two ? g0 * ReadHermite(b, d0) + g1 * ReadHermite(b, d1)
                                 : ReadHermite(b, d0);
            rd[ch] = play_lp_[ch].Process(r);
        }

        wet_l[i] = rd[0];
        wet_r[i] = rd[1];

        /* ── Feedback path: tone → loop HP → tape saturation ─────────── */
        float fbk[2];
        if (c.freeze)
        {
            /* Lossless loop: re-record the exact sample one loop length
             * back (no interpolation, no filtering), so the frozen loop
             * never decays.  Wow still colours what you hear. */
            const uint32_t k = (static_cast<uint32_t>(write_)
                              - static_cast<uint32_t>(delay_smp_ + 0.5f)) & kMask;
            fbk[0] = buf_[0][k];
            fbk[1] = buf_[1][k];
        }
        else
        {
            for (int ch = 0; ch < 2; ch++)
            {
                float x = rd[ch];
                if (tone_on_) x = tone_hp_ ? tone_[ch].Hp(x) : tone_[ch].Lp(x);
                x = dc_hp_[ch].Hp(x);
                x = SoftClip(x * 1.2f) * (1.f / 1.2f);
                fbk[ch] = x * fb;
            }
        }

        float w_l, w_r;
        if (c.pingpong)
        {
            w_l = in_amt * 0.5f * (in_l[i] + in_r[i]) + fbk[1];
            w_r = fbk[0];
        }
        else
        {
            w_l = in_amt * in_l[i] + fbk[0];
            w_r = in_amt * in_r[i] + fbk[1];
        }
        buf_[0][write_] = w_l;
        buf_[1][write_] = w_r;
        write_ = (write_ + 1u) & kMask;

        /* ── Telemetry ──────────────────────────────────────────────── */
        env_.Process(0.5f * (rd[0] + rd[1]));
        period_count_ += 1.f;
        if (period_count_ >= delay_smp_)
        {
            period_count_ -= delay_smp_;
            taps_++;
        }
    }
    period_phase_ = period_count_ / delay_smp_;
}

} // namespace sim
