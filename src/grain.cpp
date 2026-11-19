/**
 * grain.cpp — Layer 2 granular delay.  See grain.h.
 */

#include "grain.h"

namespace sim {

namespace {

constexpr float kAaHz         = 9000.f;   /* anti-alias before ↓2 */
constexpr float kReconHz      = 11000.f;  /* S950-style output smoothing */
constexpr float kHeadWindow   = 1440.f;   /* max head crossfade window, frames (60 ms @ 24 kHz) */
constexpr float kHeadWindowMin = 240.f;   /* 10 ms */
constexpr float kMarginFrames = 480.f + 0.5f * kHeadWindow;  /* clear of the write head */
constexpr uint32_t kGateLen   = 240u;     /* 5 ms trigger-out pulse */
constexpr float kMinGrainS    = 0.004f;

inline float FromQ(int32_t v) { return static_cast<float>(v) * (1.f / 32768.f); }

/* 12-bit quantise (no dither — that's the point) stored left-aligned. */
inline int16_t To12(float x)
{
    int32_t q = static_cast<int32_t>(lrintf(Clamp(x, -1.f, 0.9995f) * 2048.f));
    if (q > 2047)  q = 2047;
    if (q < -2048) q = -2048;
    return static_cast<int16_t>(q * 16);
}

inline float TableLookup(const float* tab, float x)
{
    const float f = x * 256.f;          /* x is always in [0, 1) here */
    const int   i = static_cast<int>(f);
    if (i >= 256) return tab[256];
    const float t = f - static_cast<float>(i);
    return tab[i] + t * (tab[i + 1] - tab[i]);
}

/** Envelope curve from the musical context (spec "Grain envelope"):
 *  >1 = more exponential (shorter, peakier), <1 = logarithmic (fuller).
 *  A faster grain clock and more grains push exponential; longer grains
 *  push logarithmic.  The base sits slightly full. */
inline float EnvGamma(float period_s, float size_s, uint32_t count)
{
    float g = 0.85f;
    g += 0.15f * Clamp(FastLog2(0.5f / Clamp(period_s, 0.01f, 30.f)), 0.f, 4.f);
    g -= 0.12f * Clamp(FastLog2(Clamp(size_s, kMinGrainS, 4.f) / 0.25f), -3.f, 4.f);
    g += 0.25f * (static_cast<float>(count) - 6.f) / 30.f;
    return Clamp(g, 0.5f, 2.f);
}

} // namespace

void Grains::Init(int16_t* buf, float fs)
{
    buf_ = buf;
    fs_  = fs;
    for (size_t i = 0; i < kBufFrames * 2u; i++) buf_[i] = 0;

    for (int ch = 0; ch < 2; ch++)
    {
        aa_[ch][0].Set(kAaHz, 0.7071f, fs);
    }
    for (auto& r : recon_) r.Set(kReconHz, 0.707f, fs);

    for (int i = 0; i <= 256; i++)
    {
        const float x = static_cast<float>(i) / 256.f;
        /* LPG: 1.5 % attack, then a vactrol-ish exponential fall. */
        constexpr float a = 0.015f;
        tab_lpg_[i] = x < a ? x / a : expf(-4.f * (x - a) / (1.f - a));
        tab_tri_[i] = 1.f - fabsf(2.f * x - 1.f);
        /* Slow attack (curved) then a fast 10 % release. */
        tab_sar_[i] = x < 0.9f ? (x / 0.9f) * (x / 0.9f) : (1.f - x) / 0.1f;
    }
    tab_lpg_[256] = 0.f;
    rng_.s = 0xC0FFEEu;
}

void Grains::BuildEnvTable(float shape, float gamma)
{
    /* Four stations: square → LPG → triangle → slow-attack/fast-release,
     * then the automatic curve, baked into a table so a grain pays one
     * lookup per sample.  The square station is flat here (1^γ = 1);
     * every grain gets soft edges applied separately.
     *
     * Built 32 points per audio block into a spare table and swapped in
     * when complete, so a knob move or a tempo glide never costs one
     * block a burst of work (that caused CPU spikes). */
    if (build_pos_ == 0)
    {
        build_shape_ = shape;
        build_gamma_ = gamma;
    }
    const float s  = Clamp(build_shape_, 0.f, 1.f) * 3.f;
    const int   i  = s >= 3.f ? 2 : static_cast<int>(s);
    const float fr = s - static_cast<float>(i);
    const float* tabs[4] = {nullptr, tab_lpg_, tab_tri_, tab_sar_};
    float* dst = env_tab_ == tab_env_a_ ? tab_env_b_ : tab_env_a_;
    const int end = build_pos_ + 32 > 257 ? 257 : build_pos_ + 32;
    for (int k = build_pos_; k < end; k++)
    {
        const float a = i == 0 ? 1.f : tabs[i][k];
        const float b = tabs[i + 1][k];
        dst[k] = FastPow01(Lerp(a, b, fr), build_gamma_);
    }
    build_pos_ = end;
    if (build_pos_ >= 257)
    {
        env_tab_     = dst;
        env_shape_   = build_shape_;
        built_gamma_ = build_gamma_;
        build_pos_   = 0;
    }
}

inline void Grains::ReadAt(uint32_t idx, float frac, float off, float& l, float& r) const
{
    /* Drop-sample read, S950 style: the nearest-earlier stored frame, no
     * interpolation.  Pitch moves change the step through the samples
     * (the grit and aliasing are the character); the 11 kHz output
     * filter plays the part of the sampler's output filter. */
    const float   f     = frac + off;
    int32_t       whole = static_cast<int32_t>(f);
    if (f < 0.f) whole -= 1;
    int32_t i0 = static_cast<int32_t>(idx) + whole;
    if (i0 < 0) i0 += static_cast<int32_t>(kBufFrames);
    else if (i0 >= static_cast<int32_t>(kBufFrames)) i0 -= static_cast<int32_t>(kBufFrames);

    /* One 32-bit load fetches L and R together. */
    const uint32_t a = reinterpret_cast<const uint32_t*>(buf_)[i0];
    l = FromQ(static_cast<int16_t>(a & 0xFFFFu));
    r = FromQ(static_cast<int16_t>(a >> 16));
}

inline void Grains::ReadTap(const Voice& g, Tap& tap, float& l, float& r) const
{
    if (tap.rate != 0.f)
    {
        float ph = tap.phase + tap.rate;
        if (ph >= 1.f) ph -= 1.f;
        else if (ph < 0.f) ph += 1.f;
        tap.phase = ph;
    }
    else
    {
        /* Single head on the cursor: a plain interpolated read. */
        ReadAt(g.c_idx, g.c_frac, 0.f, l, r);
        return;
    }

    /* Two heads half a window apart; smoothstep crossfade sums to 1. */
    const float tri = 1.f - fabsf(2.f * tap.phase - 1.f);
    const float g0  = tri * tri * (3.f - 2.f * tri);
    const float g1  = 1.f - g0;

    l = r = 0.f;
    float a, b;
    if (g0 > 0.001f)
    {
        ReadAt(g.c_idx, g.c_frac, (tap.phase - 0.5f) * g.win, a, b);
        l += g0 * a;  r += g0 * b;
    }
    if (g1 > 0.001f)
    {
        const float ph1 = tap.phase < 0.5f ? tap.phase + 0.5f : tap.phase - 0.5f;
        ReadAt(g.c_idx, g.c_frac, (ph1 - 0.5f) * g.win, a, b);
        l += g1 * a;  r += g1 * b;
    }
}

inline void Grains::Advance(uint32_t& idx, float& frac, float inc)
{
    frac += inc;
    int32_t whole = static_cast<int32_t>(frac);
    if (frac < 0.f) whole -= 1;                  /* floor for negatives */
    frac -= static_cast<float>(whole);
    int32_t ni = static_cast<int32_t>(idx) + whole;
    if (ni < 0) ni += static_cast<int32_t>(kBufFrames);
    else if (ni >= static_cast<int32_t>(kBufFrames)) ni -= static_cast<int32_t>(kBufFrames);
    idx = static_cast<uint32_t>(ni);
}

void Grains::StartGrain(const GrainControls& c, float base_off, uint32_t at)
{
    triggers_++;
    gate_samples_ = kGateLen;
    if (!c.render) return;   /* inaudible: keep clock + trigger-out only */

    /* Find a free voice among the first `count`. */
    int v = -1;
    for (uint32_t k = 0; k < c.count && k < kMaxGrains; k++)
        if (!voices_[k].active) { v = static_cast<int>(k); break; }
    if (v < 0) return;

    Voice& g = voices_[v];

    /* Pitch: base transposition + optional random semitone steps. */
    float st = c.pitch_st;
    if (c.pitch_rand > 0.001f)
        st += roundf(rng_.Bi() * c.pitch_rand * 12.f);
    const float ratio = exp2f(st / 12.f);

    const bool  reverse = c.dir == 1u || (c.dir == 2u && rng_.Uni() < 0.5f);
    const float sign    = reverse ? -1.f : 1.f;
    const float speed   = Clamp(c.speed, 1.f / 16.f, 8.f);
    const float c_inc   = 0.5f * speed;            /* 24 kHz buffer, 48 kHz out */
    const float wrate   = c.freeze ? 0.f : 0.5f;   /* write head, frames/sample */
    const float buf     = static_cast<float>(kBufFrames);

    /* Output length: the grain covers size seconds of audio at `speed`. */
    float len = Clamp(c.size_s, kMinGrainS, 4.f) * fs_ / speed;

    /* Keep the whole grain clear of the write head, both ahead (a fast
     * forward cursor catching up) and behind (the write head lapping a
     * slow grain); shorten very long slow grains until they fit. */
    float min_off = 0.f, max_off = 0.f;
    for (int tries = 0; tries < 12; tries++)
    {
        const float span = c_inc * len;
        if (reverse)
        {
            min_off = span + kMarginFrames;
            max_off = buf - kMarginFrames - wrate * len;
        }
        else
        {
            const float gain = c_inc - wrate;
            min_off = kMarginFrames + (c.freeze ? span : (gain > 0.f ? gain * len : 0.f));
            max_off = buf - kMarginFrames - (gain < 0.f ? -gain * len : 0.f);
        }
        if (min_off <= max_off) break;
        len *= 0.75f;
    }
    if (len < kMinGrainS * fs_) len = kMinGrainS * fs_;
    if (min_off > max_off) min_off = max_off;

    g.len     = static_cast<uint32_t>(len);
    g.inv_len = 1.f / len;
    {
        float e = Clamp(0.08f * len, 0.008f * fs_, 0.04f * fs_);
        if (e > 0.45f * len) e = 0.45f * len;
        g.edge     = static_cast<uint32_t>(e) + 1u;
        g.inv_edge = 1.f / static_cast<float>(g.edge);
    }

    /* Start offset: base delay + scan + spray, inside the safe window. */
    /* More grains spread out automatically (up to ±200 ms at 36) so
     * dense clouds don't pile onto the same instant of audio. */
    const float crowd = static_cast<float>(c.count_knob > 1u ? c.count_knob - 1u : 0u) / 35.f;
    float off = base_off + scan_off_;
    const float jitter_s = c.spray * c.spray + 0.2f * crowd;
    if (jitter_s > 0.001f)
        off += rng_.Bi() * jitter_s * kBufRate;              /* spray: up to ±1 s */
    off = Clamp(off, min_off, max_off);

    float start = static_cast<float>(write_) - off;
    if (reverse) start += c_inc * len;
    while (start < 0.f) start += buf;
    while (start >= buf) start -= buf;

    g.c_idx  = static_cast<uint32_t>(start);
    g.c_frac = start - static_cast<float>(g.c_idx);
    g.c_inc  = sign * c_inc;
    g.t      = 0;
    g.lp_l   = g.lp_r = 0.f;

    /* Heads move at the pitch rate; the cursor at the speed rate.  The
     * crossfade window scales with the grain (a quarter of its audio,
     * 10–60 ms): long, slow grains get smooth, unhurried crossfades;
     * short grains keep a tight one. */
    g.win = Clamp(0.25f * c_inc * len, kHeadWindowMin, kHeadWindow);
    const float p = sign * 0.5f * ratio;
    auto set_tap = [&](Tap& tap, float pitch_rate) {
        const float d = 2.f * (pitch_rate - g.c_inc) / g.win;   /* per 24 kHz step */
        tap.rate  = fabsf(d) < 1e-7f ? 0.f : d;
        tap.phase = 0.5f;   /* start on the cursor */
    };
    set_tap(g.main, p);
    set_tap(g.lo, 0.5f * p);
    set_tap(g.hi, 2.f * p);

    /* Random equal-power pan scaled by Spread. */
    const float spread = Clamp(c.spread + 0.4f * crowd, 0.f, 1.f);
    const float pan = 0.5f + 0.5f * spread * rng_.Bi();

    /* Short grains whose envelope never reaches full level (the soft
     * edges cut into the shape's peak) are lifted to normal loudness:
     * gain = 1 / the highest level this grain's envelope reaches, at
     * most +6 dB. */
    float peak = 0.f;
    for (int k = 0; k <= 32; k++)
    {
        const float x = static_cast<float>(k) * (1.f / 32.f);
        float e = TableLookup(env_tab_, x < 0.9999f ? x : 0.9999f);
        const float tt   = x * len;
        const float edge = tt < len - tt ? tt : len - tt;
        if (edge < static_cast<float>(g.edge))
        {
            const float u = edge * g.inv_edge;
            e *= u * u * (3.f - 2.f * u);
        }
        if (e > peak) peak = e;
    }
    const float lift = peak > 0.5f ? 1.f / peak : 2.f;
    g.lift = lift;
    g.gain_l = lift * 1.4142f * Cos01(0.25f * pan);
    g.gain_r = lift * 1.4142f * Sin01(0.25f * pan);

    g.active   = true;
    g.start_at = at;
    newest_    = static_cast<int8_t>(v);
}

void Grains::Process(const float* in_l, const float* in_r,
                     const float* tape_l, const float* tape_r,
                     float* out_l, float* out_r,
                     float* feast_l, float* feast_r,
                     size_t n, const GrainControls& c)
{
    const float period_smp = Clamp(c.period_s, 0.002f, 30.f) * fs_;
    const float trig_inc   = static_cast<float>(c.per_clock) / period_smp;
    const float tick_inc   = 1.f / period_smp;

    const float speed    = Clamp(c.speed, 1.f / 16.f, 8.f);
    const float size_smp = Clamp(c.size_s, kMinGrainS, 4.f) * fs_;
    const float dur_smp  = size_smp / speed;               /* grain length heard */

    /* Base delay from the grain clock (frames at 24 kHz). */
    const float base_off = Clamp(c.period_s * kBufRate, kMarginFrames,
                                 static_cast<float>(kBufFrames) - kMarginFrames);

    /* Scan playhead: grain starts drift through the buffer at `speed`. */
    float range = base_off;
    if (range < size_smp * 0.5f) range = size_smp * 0.5f;
    if (range < 4800.f) range = 4800.f;
    const float scan_rate = 0.5f * (1.f - speed);

    /* Output normalisation from the expected overlap. */
    const float interval = period_smp / static_cast<float>(c.per_clock);
    float overlap = dur_smp / interval;
    if (overlap > static_cast<float>(c.count)) overlap = static_cast<float>(c.count);
    const float norm = 1.f / sqrtf(overlap < 1.f ? 1.f : overlap);

    /* LPG blend: weight of the LPG station in the morph. */
    const float s3 = Clamp(c.shape, 0.f, 1.f) * 3.f;
    const float lpg_amt = Clamp(1.f - fabsf(s3 - 1.f), 0.f, 1.f);

    /* Envelope: knob morph + automatic curve. */
    env_gamma_ = EnvGamma(c.period_s, c.size_s, c.count_knob);
    if (build_pos_ > 0 || fabsf(c.shape - env_shape_) > 0.002f
        || fabsf(env_gamma_ - built_gamma_) > 0.02f)
        BuildEnvTable(c.shape, env_gamma_);

    if (!c.render)
        for (auto& g : voices_) g.active = false;
    newest_env_ = c.render ? newest_env_ : 0.f;

    const bool oct    = c.feast_active && c.octaves > 0u;
    const bool oct_hi = oct && c.octaves >= 2u;

    for (size_t i = 0; i < n; i++)
    {
        /* ── Record (AA filter every sample, write every other) ────────── */
        const float src_l = Lerp(in_l[i], tape_l[i], c.source_mix);
        const float src_r = Lerp(in_r[i], tape_r[i], c.source_mix);
        /* 2-pole anti-alias (S950-loose: a little aliasing is the point). */
        const float al = aa_[0][0].Lp(src_l);
        const float ar = aa_[1][0].Lp(src_r);
        odd_ = !odd_;
        if (odd_ && !c.freeze)
        {
            buf_[2u * write_]      = To12(al);
            buf_[2u * write_ + 1u] = To12(ar);
            if (++write_ >= kBufFrames) write_ = 0;
        }

        /* ── Scan playhead ──────────────────────────────────────────── */
        scan_off_ += scan_rate;
        if (scan_off_ > 0.5f * range)  scan_off_ -= range;
        if (scan_off_ < -0.5f * range) scan_off_ += range;

        /* ── Clocks ─────────────────────────────────────────────────── */
        tick_phase_ += tick_inc;
        if (tick_phase_ >= 1.f) { tick_phase_ -= 1.f; ticks_++; }
        trig_phase_ += trig_inc;
        if (trig_phase_ >= 1.f)
        {
            trig_phase_ -= 1.f;
            StartGrain(c, base_off, static_cast<uint32_t>(i));
        }
        if (gate_samples_) gate_samples_--;

    }

    /* ── Voices at 24 kHz ─────────────────────────────────────────────
     * The buffer holds nothing above ~10 kHz (it is recorded at 24 kHz),
     * so grains are rendered at 24 kHz — one step per two output
     * samples, half the work — then interpolated back up to 48 kHz.
     * Each grain renders its whole block at once (state in registers). */
    if (n > kMaxBlock) n = kMaxBlock;
    const size_t m = n / 2u;                         /* 24 kHz steps this block */
    float acc_l[kMaxBlock / 2] = {}, acc_r[kMaxBlock / 2] = {};
    float oct_l[kMaxBlock / 2] = {}, oct_r[kMaxBlock / 2] = {};

    for (uint32_t k = 0; k < kMaxGrains; k++)
    {
        Voice& g = voices_[k];
        if (!g.active) continue;

        const bool  do_oct = oct && k < kOctVoices;
        uint32_t    t      = g.t;                    /* 48 kHz samples elapsed */
        const uint32_t len = g.len;
        const float inv    = g.inv_len;
        const float gl0    = g.gain_l, gr0 = g.gain_r;
        const float step   = 2.f * g.c_inc;          /* cursor per 24 kHz step */
        float       lp_l   = g.lp_l, lp_r = g.lp_r;
        float       env    = 0.f, x = 0.f;

        for (size_t j = g.start_at / 2u; j < m; j++)
        {
            x   = static_cast<float>(t) * inv;
            env = TableLookup(env_tab_, x);
            /* Soft edges on every grain (raised-cosine-like): 8–40 ms. */
            const uint32_t edge = t < len - t ? t : len - t;
            if (edge < g.edge)
            {
                const float u = static_cast<float>(edge) * g.inv_edge;
                env *= u * u * (3.f - 2.f * u);
            }

            float l, r;
            ReadTap(g, g.main, l, r);
            if (lpg_amt > 0.f)
            {
                /* The LPG opens with the lifted level, so short grains
                 * are not darkened as well as quieted. */
                float el = env * g.lift;
                if (el > 1.f) el = 1.f;
                const float kc = Lerp(1.f, 0.05f + 0.95f * el * el, lpg_amt);
                lp_l += kc * (l - lp_l);
                lp_r += kc * (r - lp_r);
                l = lp_l;
                r = lp_r;
            }
            const float gl = env * gl0, gr = env * gr0;
            acc_l[j] += l * gl;
            acc_r[j] += r * gr;

            if (do_oct)
            {
                float a, b;
                ReadTap(g, g.lo, a, b);
                float sl = a, sr = b;
                if (oct_hi)
                {
                    ReadTap(g, g.hi, a, b);
                    sl += a;  sr += b;
                }
                oct_l[j] += sl * gl;
                oct_r[j] += sr * gr;
            }

            Advance(g.c_idx, g.c_frac, step);
            t += 2u;
            if (t >= len) { g.active = false; break; }
        }
        g.t = t;  g.lp_l = lp_l;  g.lp_r = lp_r;  g.start_at = 0;

        if (static_cast<int8_t>(k) == newest_)
        {
            newest_progress_ = x;
            newest_env_      = g.active ? env : 0.f;
        }
    }

    /* ── Back to 48 kHz: linear interpolation, then the S950-style
     *    smoothing filter, overlap normalisation and the Feast bus. ───── */
    const float on = oct_hi ? 0.5f : 0.7f;
    for (size_t j = 0; j < m; j++)
    {
        for (int h = 0; h < 2; h++)
        {
            const size_t i = 2u * j + static_cast<size_t>(h);
            const float  al = h == 0 ? 0.5f * (prev_l_ + acc_l[j]) : acc_l[j];
            const float  ar = h == 0 ? 0.5f * (prev_r_ + acc_r[j]) : acc_r[j];
            const float  ol  = recon_[0].Lp(al);
            const float  or_ = recon_[1].Lp(ar);
            out_l[i] = ol * norm;
            out_r[i] = or_ * norm;
            if (oct)
            {
                const float xl = recon_[2].Lp(h == 0 ? 0.5f * (prev_xl_ + oct_l[j]) : oct_l[j]);
                const float xr = recon_[3].Lp(h == 0 ? 0.5f * (prev_xr_ + oct_r[j]) : oct_r[j]);
                feast_l[i] = (ol + on * xl) * norm * 0.8f;
                feast_r[i] = (or_ + on * xr) * norm * 0.8f;
            }
            else
            {
                feast_l[i] = out_l[i];
                feast_r[i] = out_r[i];
            }
        }
        prev_l_ = acc_l[j];  prev_r_ = acc_r[j];
        prev_xl_ = oct_l[j]; prev_xr_ = oct_r[j];
    }

    scan_phase01_ = (scan_off_ + 0.5f * range) / range;
}

} // namespace sim
