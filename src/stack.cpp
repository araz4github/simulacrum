/**
 * stack.cpp — Layer 3 voices, envelopes, warm VCA and formant filter.
 */

#include "stack.h"

namespace sim {

namespace {

constexpr float kC4Hz         = 261.6256f;
constexpr float kXoverHz      = 200.f;     /* lows held centre in CW mode */
constexpr float kTopHz        = 20000.f;
/* The original dual notch (first build), restored: two wide, gentle SVF
 * notches with very little resonance and soft saturation inside the
 * filter for character.  Smooth, never harsh. */
constexpr float kNotchQ       = 0.55f;
constexpr float kNotchDrive   = 1.6f;
constexpr float kVcaDrive     = 1.4f;      /* warm VCA soft clip */
constexpr uint32_t kCoefEvery = 4u;        /* filter coefficient rate: 12 kHz */

constexpr float kSpread6[6] = {-1.f, -0.6f, -0.2f, 0.2f, 0.6f, 1.f};
constexpr float kSpread4[4] = {-1.f, -0.33f, 0.33f, 1.f};
constexpr float kSpread3[3] = {-1.f, 0.f, 1.f};

/** Cutoff knob (distance from centre, 0..1) → lower-notch frequency.
 *  Log-interpolated through anchor points so the musical middle
 *  (≈3 kHz → 400 Hz) takes the travel just either side of centre. */
inline float CutoffHz(float d)
{
    constexpr float kT[5]  = {0.f,     0.15f,  0.55f, 0.8f,  1.f};
    constexpr float kHz[5] = {10000.f, 3000.f, 400.f, 120.f, 35.f};
    if (d <= 0.f) return kTopHz;           /* centre: parked above audio */
    int i = 0;
    while (i < 3 && d > kT[i + 1]) i++;
    const float u = (d - kT[i]) / (kT[i + 1] - kT[i]);
    return kHz[i] * FastExp2(u * FastLog2(kHz[i + 1] / kHz[i]));
}

inline float Log01(float hz)
{
    return Clamp(FastLog2(hz / 20.f) / 9.966f, 0.f, 1.f);   /* 20 Hz..20 kHz */
}

/** Quadrature oscillator by phasor rotation: 4 mul + 2 add per sample. */
struct Rot
{
    float c = 1.f, s = 0.f;
    void Step(float cw, float sw)
    {
        const float nc = c * cw - s * sw;
        s = s * cw + c * sw;
        c = nc;
    }
    void Renorm()
    {
        const float k = 1.5f - 0.5f * (c * c + s * s);
        c *= k;
        s *= k;
    }
};

} // namespace

/* ── Voices and envelopes ───────────────────────────────────────────── */

void Stack::Init(float fs)
{
    *this = Stack();          /* fresh voices, envelopes and note counter */
    fs_ = fs;
    Rng r;
    r.s = 0x5EED5u;
    for (auto& v : voices_)
    {
        for (auto& s : v.saws) s.phase = r.Uni();
        v.saw_lo.phase = r.Uni();
        v.saw_hi.phase = r.Uni();
    }
    xo_mid_.SetHz(kXoverHz, fs);
    xo_l_.SetHz(kXoverHz, fs);
    xo_r_.SetHz(kXoverHz, fs);
    env_.Init(0.005f, 0.2f, fs);
}

void Stack::StartNote(uint32_t idx, float volts)
{
    Voice& v = voices_[idx];
    v.from  = v.env;          /* restart from where it is: no click */
    v.x     = 0.f;
    v.stage = Stage::Attack;
    v.volts = volts;
    v.age   = ++age_counter_;
}

void Stack::ReleaseVoice(Voice& v)
{
    if (v.stage == Stage::Idle || v.stage == Stage::Release) return;
    v.from  = v.env;
    v.x     = 0.f;
    v.stage = Stage::Release;
}

float Stack::StepEnvelope(Voice& v, float n, bool gate) const
{
    switch (v.stage)
    {
        case Stage::Attack:
            v.x += n / (ta_ * fs_);
            if (v.x >= 1.f)
            {
                v.x = 0.f;
                v.from = 1.f;
                if (!was_gated_)  { v.stage = Stage::Hold; }
                else if (gate)    { v.stage = Stage::Decay; }
                else              { v.stage = Stage::Release; }   /* trigger: ring out */
                return 1.f;
            }
            return v.from + (1.f - v.from) * FastPow01(v.x, pa_);

        case Stage::Decay:
            /* Decay to 50 %: at least 1 s, following the release time
             * above that (up to 8 s).  Release itself can be shorter. */
            v.x += n / ((tr_ > 1.f ? tr_ : 1.f) * fs_);
            if (v.x >= 1.f) { v.stage = Stage::Sustain; return 0.5f; }
            return 0.5f + 0.5f * FastPow01(1.f - v.x, q_);

        case Stage::Sustain:
            return 0.5f;

        case Stage::Hold:
            return 1.f;

        case Stage::Release:
            v.x += n / (tr_ * fs_);
            if (v.x >= 1.f) { v.stage = Stage::Idle; return 0.f; }
            return v.from * FastPow01(1.f - v.x, q_);

        case Stage::Idle:
        default:
            return 0.f;
    }
}

void Stack::BeginBlock(const StackControls& c, size_t /*n*/)
{
    /* Envelope timing.  Attack 1 ms → 4 s, exponential → linear as CW.
     * Release 3 ms → 8 s, semi-log → linear → slightly exponential. */
    const float a = Clamp(c.attack, 0.f, 1.f);
    const float r = Clamp(c.release, 0.f, 1.f);
    /* Power-law tapers: fine steps in the first half, longer times in
     * larger steps through the second.  Noon: attack 1 s, release 1.5 s;
     * ¾: 2.25 s / 4 s; full: 4 s / 8 s. */
    ta_ = 0.001f + 4.f * a * a;                               /* a^2      */
    pa_ = 1.f + 2.f * (1.f - a);
    tr_ = 0.003f + 8.f * FastPow01(r, 2.415f);                /* 8·0.5^p = 1.5 */
    q_  = 0.6f + 0.8f * r;

    const uint32_t nv = c.voices < 1u ? 1u : (c.voices > kMaxStackVoices ? kMaxStackVoices : c.voices);

    if (!c.gated_mode)
    {
        /* Drone: voice 0 holds and follows V/Oct live. */
        Voice& v = voices_[0];
        if (v.stage == Stage::Idle || v.stage == Stage::Release) StartNote(0, c.voct_live);
        v.volts = c.voct_live;
        for (uint32_t i = 1; i < kMaxStackVoices; i++) ReleaseVoice(voices_[i]);
        lead_oct_ = c.tune_oct + c.voct_live;
    }
    else
    {
        if (!was_gated_)
            ReleaseVoice(voices_[0]);           /* leave drone mode */
        was_gated_ = true;

        if (last_gate_ && !c.gate)
            for (auto& v : voices_) ReleaseVoice(v);

        if (c.note_seq != last_note_seq_)
        {
            /* Free voice first, then the oldest releasing, then oldest. */
            int pick = -1;
            for (uint32_t i = 0; i < nv && pick < 0; i++)
                if (voices_[i].stage == Stage::Idle) pick = static_cast<int>(i);
            if (pick < 0)
            {
                uint32_t best = 0xFFFFFFFFu;
                for (uint32_t i = 0; i < nv; i++)
                    if (voices_[i].stage == Stage::Release && voices_[i].age < best)
                    { best = voices_[i].age; pick = static_cast<int>(i); }
            }
            if (pick < 0)
            {
                uint32_t best = 0xFFFFFFFFu;
                for (uint32_t i = 0; i < nv; i++)
                    if (voices_[i].age < best) { best = voices_[i].age; pick = static_cast<int>(i); }
            }
            /* Earlier notes ring out on their own release envelope while
             * the new one plays (Rings-like); Voices caps the overlap. */
            for (uint32_t i = 0; i < kMaxStackVoices; i++)
                if (static_cast<int>(i) != pick) ReleaseVoice(voices_[i]);
            StartNote(static_cast<uint32_t>(pick), c.note_volts);
            lead_oct_ = c.tune_oct + c.note_volts;
        }
        else
        {
            lead_oct_ = c.tune_oct + c.note_volts;
        }
    }
    was_gated_ = c.gated_mode;
    last_gate_ = c.gate;
    last_note_seq_ = c.note_seq;

    /* Voices above the count ring out. */
    for (uint32_t i = nv; i < kMaxStackVoices; i++)
        if (c.gated_mode) ReleaseVoice(voices_[i]);

    lead_hz_ = kC4Hz * FastExp2(Clamp(lead_oct_, -5.f, 5.f));
}

void Stack::ComputeDeepSub(float* sub, size_t n)
{
    const float inc = 0.0625f * lead_hz_ / fs_;
    for (size_t i = 0; i < n; i++)
    {
        sub[i] = Sin01(deep_phase_);
        deep_phase_ = Wrap01(deep_phase_ + inc);
    }
}

void Stack::ComputeSub(float* sub, size_t n)
{
    const float inc = 0.25f * lead_hz_ / fs_;
    for (size_t i = 0; i < n; i++)
    {
        sub[i] = Sin01(sub_phase_);
        sub_phase_ = Wrap01(sub_phase_ + inc);
    }
}

void Stack::Process(const float* feast_l, const float* feast_r, const float* sub,
                    float* out_l, float* out_r, size_t n, const StackControls& c)
{
    constexpr size_t kMaxBlock = 64u;
    if (n > kMaxBlock) n = kMaxBlock;
    float acc_l[kMaxBlock] = {}, acc_r[kMaxBlock] = {};

    const uint32_t nv = c.voices < 1u ? 1u : (c.voices > kMaxStackVoices ? kMaxStackVoices : c.voices);
    const float w_saw  = 0.5f * Clamp(c.sine_saw, 0.f, 1.f);
    const float w_sine = 1.f - w_saw;
    const bool  oct_lo = c.octaves >= 1u;
    const bool  oct_hi = c.octaves >= 2u;
    const bool  audible = c.level > 0.0005f;
    const float dt2 = c.detune * c.detune;
    const float sine_det = 1.f + 0.012f * c.detune;      /* up to ~20 cents */
    const float layer_norm = oct_hi ? 0.6f : (oct_lo ? 0.75f : 1.f);
    const float vca_norm = 1.f / SoftClip(kVcaDrive);
    const float poly_norm = 1.f / sqrtf(static_cast<float>(nv));

    /* Fewer saws per voice as polyphony rises keeps the CPU in check. */
    const int     n_saw  = nv <= 2u ? 6 : (nv <= 4u ? 4 : 3);
    const float*  spread = n_saw == 6 ? kSpread6 : (n_saw == 4 ? kSpread4 : kSpread3);
    const float   saw_l_norm = 1.f / static_cast<float>((n_saw + 1) / 2);
    const float   saw_r_norm = 1.f / static_cast<float>(n_saw / 2 > 0 ? n_saw / 2 : 1);

    sounding_ = 0;
    const float fn = static_cast<float>(n);

    for (uint32_t vi = 0; vi < kMaxStackVoices; vi++)
    {
        Voice& v = voices_[vi];
        const float e0 = v.env;
        const float e1 = StepEnvelope(v, fn, c.gate);
        v.env = e1;
        if (v.stage == Stage::Idle && e0 <= 0.f) continue;
        sounding_++;
        if (!audible) continue;

        /* Per-voice pitch and phasor steps for this block. */
        const float f   = kC4Hz * FastExp2(Clamp(c.tune_oct + v.volts, -5.f, 5.f));
        const float inc = Clamp(f, 8.f, 8000.f) / fs_;

        /* LPG-style: the voice's own low-pass opens with its envelope —
         * bright at full level, closing gently as the note fades
         * (≈ 3 harmonics at silence → 40 at full, 21 at the 50 % sustain;
         * slight, not a wah). */
        const float lpg_lvl = 0.5f * (e0 + e1);
        const float lpg_hz  = Clamp(f * (3.f + 37.f * lpg_lvl), 80.f, 18000.f);
        const float lpg_k   = 1.f - FastExp2(-9.0647f * lpg_hz / fs_);

        /* Gentle high-pass two octaves below each voice's own note. */
        const float hp_k  = 1.f - FastExp2(-9.0647f * Clamp(0.25f * f, 8.f, 2000.f) / fs_);   /* 1-e^(-2πfc/fs) */
        Rot ra, rb, rlo, rhi;
        /* The phasors keep their phase in the voice via sin_* (0..1). */
        ra.c = Cos01(v.sin_a);  ra.s = Sin01(v.sin_a);
        rb.c = Cos01(v.sin_b);  rb.s = Sin01(v.sin_b);
        const float ca = Cos01(inc), sa = Sin01(inc);
        const float cb = Cos01(inc * sine_det), sb = Sin01(inc * sine_det);
        float clo = 0.f, slo = 0.f, chi = 0.f, shi = 0.f;
        if (oct_lo) { rlo.c = Cos01(v.sin_lo); rlo.s = Sin01(v.sin_lo); clo = Cos01(0.5f * inc); slo = Sin01(0.5f * inc); }
        if (oct_hi) { rhi.c = Cos01(v.sin_hi); rhi.s = Sin01(v.sin_hi); chi = Cos01(2.f * inc);  shi = Sin01(2.f * inc); }

        float saw_inc[6];
        for (int k = 0; k < n_saw; k++) saw_inc[k] = inc * (1.f + 0.035f * dt2 * spread[k]);

        for (size_t i = 0; i < n; i++)
        {
            const float env = e0 + (e1 - e0) * (static_cast<float>(i + 1) / fn);

            float sine_l = 0.5f * (ra.s + rb.s);
            float sine_r = 0.5f * (ra.c + rb.c);
            ra.Step(ca, sa);
            rb.Step(cb, sb);

            float saw_l = 0.f, saw_r = 0.f;
            if (w_saw > 0.f)
            {
                for (int k = 0; k < n_saw; k += 2) saw_l += v.saws[k].Tick(saw_inc[k]);
                for (int k = 1; k < n_saw; k += 2) saw_r += v.saws[k].Tick(saw_inc[k]);
                saw_l *= saw_l_norm;
                saw_r *= saw_r_norm;
            }
            if (oct_lo)
            {
                sine_l += 0.6f * rlo.s;  sine_r += 0.6f * rlo.c;
                rlo.Step(clo, slo);
                if (w_saw > 0.f) { const float sw = 0.6f * v.saw_lo.Tick(0.5f * inc); saw_l += sw; saw_r += sw; }
            }
            if (oct_hi)
            {
                sine_l += 0.45f * rhi.s;  sine_r += 0.45f * rhi.c;
                rhi.Step(chi, shi);
                if (w_saw > 0.f) { const float sw = 0.45f * v.saw_hi.Tick(2.f * inc); saw_l += sw; saw_r += sw; }
            }

            const float xl = layer_norm * (w_sine * sine_l + 0.6f * w_saw * saw_l);
            const float xr = layer_norm * (w_sine * sine_r + 0.6f * w_saw * saw_r);
            /* Trim the lowest lows. */
            v.hp[0] += hp_k * (xl - v.hp[0]);
            v.hp[1] += hp_k * (xr - v.hp[1]);
            v.lpg[0] += lpg_k * ((xl - v.hp[0]) - v.lpg[0]);
            v.lpg[1] += lpg_k * ((xr - v.hp[1]) - v.lpg[1]);
            /* Warm VCA: the envelope drives a gentle soft clip. */
            acc_l[i] += SoftClip(kVcaDrive * v.lpg[0] * env) * vca_norm;
            acc_r[i] += SoftClip(kVcaDrive * v.lpg[1] * env) * vca_norm;
        }

        /* Store phases back (phasor angle → 0..1). */
        ra.Renorm(); rb.Renorm();
        v.sin_a = Wrap01(v.sin_a + inc * fn);
        v.sin_b = Wrap01(v.sin_b + inc * sine_det * fn);
        if (oct_lo) v.sin_lo = Wrap01(v.sin_lo + 0.5f * inc * fn);
        if (oct_hi) v.sin_hi = Wrap01(v.sin_hi + 2.f * inc * fn);
    }
    (void)nv;

    /* ── Filter ──────────────────────────────────────────────────────── */
    const float d      = fabsf(c.cutoff - 0.5f) * 2.f;
    const bool  stereo = c.cutoff > 0.5f;
    /* Leaving the centre is gradual: the filter fades in over the first
     * 15 % of travel each way, and the CW stereo image (sine/cosine
     * width, L/R split, centred lows) fades in over the first 25 %. */
    const float a01    = Clamp(d / 0.15f, 0.f, 1.f);
    const float amount = a01 * a01 * (3.f - 2.f * a01);
    const float w01    = Clamp(d / 0.25f, 0.f, 1.f);
    const float width  = stereo ? w01 * w01 * (3.f - 2.f * w01) : 0.f;
    const float f_lo_c = CutoffHz(d);
    const float split  = 0.5f * d * width;                /* L/R offset, octaves */
    const float ml = FastExp2(-split), mr = FastExp2(split);
    const float sp_base = 3.f * Clamp(c.spacing, 0.f, 1.f);
    const float sub_amt = 1.5f * Clamp(c.sub_depth, 0.f, 1.f);
    const float gain = c.level * poly_norm;

    float last_lo = kTopHz, last_hi = kTopHz;

    /* Ladder LP: ~5 octaves above the newest note (≥ 6 kHz, ≤ 18 kHz),
     * gliding over ~20 ms when the note changes. */
    const float lad_target = Clamp(32.f * lead_hz_, 6000.f, 18000.f);
    ladder_hz_ += (1.f - FastExp2(-1.4427f * fn / (0.02f * fs_))) * (lad_target - ladder_hz_);
    const float lad_g = 1.f - FastExp2(-9.0647f * ladder_hz_ / fs_);
    constexpr float kLadderRes  = 0.7f;    /* of 4: a slight resonant lift */
    constexpr float kLadderComp = 1.f + kLadderRes;      /* unity gain below cutoff */

    for (size_t i = 0; i < n; i++)
    {
        float sl = gain * acc_l[i], sr = gain * acc_r[i];
        {
            /* Mono below / at centre, widening smoothly as CW opens. */
            const float m = 0.7071f * (sl + sr);
            sl = m + width * (sl - m);
            sr = m + width * (sr - m);
        }
        if (audible)
        {
            /* Ladder low-pass with soft-clipped feedback (character). */
            float* ch_in[2] = {&sl, &sr};
            for (int ch = 0; ch < 2; ch++)
            {
                float* y = ladder_[ch].y;
                const float u = SoftClip(*ch_in[ch] * kLadderComp - kLadderRes * y[3]);
                y[0] += lad_g * (u    - y[0]);
                y[1] += lad_g * (y[0] - y[1]);
                y[2] += lad_g * (y[1] - y[2]);
                y[3] += lad_g * (y[2] - y[3]);
                *ch_in[ch] = y[3];
            }
        }
        env_.Process(0.5f * (sl + sr));

        float xl = sl + feast_l[i];
        float xr = sr + feast_r[i];

        if (amount > 0.f)
        {
            if ((i % kCoefEvery) == 0u)
            {
                float sp = Clamp(sp_base + sub_amt * sub[i], 0.f, 4.f);
                const float f_lo = f_lo_c;
                const float f_hi = f_lo_c * FastExp2(sp);
                for (int ch = 0; ch < 2; ch++)
                {
                    const float m = ch == 0 ? ml : mr;
                    if (ch == 1 && split == 0.f)
                    {
                        for (int k = 0; k < 2; k++) notch_[1][k].CopyCoeffs(notch_[0][k]);
                        break;
                    }
                    notch_[ch][0].Set(f_lo * m, kNotchQ, fs_);
                    notch_[ch][1].Set(f_hi * m, kNotchQ, fs_);
                }
                last_lo = f_lo;
                last_hi = f_hi;
            }

            float yl = notch_[0][1].NotchDriven(notch_[0][0].NotchDriven(xl, kNotchDrive), kNotchDrive);
            float yr = notch_[1][1].NotchDriven(notch_[1][0].NotchDriven(xr, kNotchDrive), kNotchDrive);

            {
                /* CW: lows to the centre, highs keep their L/R difference
                 * (faded in with the stereo width). */
                const float low_mid = xo_mid_.Process(0.5f * (yl + yr));
                const float cl = low_mid + (yl - xo_l_.Process(yl));
                const float cr = low_mid + (yr - xo_r_.Process(yr));
                yl += width * (cl - yl);
                yr += width * (cr - yr);
            }
            xl = Lerp(xl, yl, amount);
            xr = Lerp(xr, yr, amount);
        }

        out_l[i] = xl;
        out_r[i] = xr;
    }

    notch_lo01_ = amount > 0.f ? Log01(last_lo) : 1.f;
    notch_hi01_ = amount > 0.f ? Log01(last_hi) : 1.f;
}

} // namespace sim
