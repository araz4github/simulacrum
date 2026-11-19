/**
 * dsp_util.h — small, header-only DSP building blocks for Simulacrum.
 *
 * Everything here is allocation-free and cheap enough for the 48 kHz
 * per-sample loop on the Cortex-M7 (hardware single-precision FPU).
 */

#pragma once

#include <cmath>
#include <cstdint>

namespace sim {

constexpr float kPi    = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530717959f;

inline float Clamp(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

/** sin(2π·p) for any phase p (wrapped), ~1e-4 max error. */
inline float Sin01(float p)
{
    p -= static_cast<float>(static_cast<int32_t>(p));
    if (p < 0.f) p += 1.f;
    float x = p - 0.5f;                 /* sin(2πp) = −sin(2πx) */
    if (x > 0.25f)  x = 0.5f - x;
    if (x < -0.25f) x = -0.5f - x;
    const float y  = kTwoPi * x;
    const float y2 = y * y;
    const float s  = y * (1.f - y2 * (1.f / 6.f - y2 * (1.f / 120.f - y2 * (1.f / 5040.f))));
    return -s;
}

inline float Cos01(float p) { return Sin01(p + 0.25f); }

/** 2^x, ~2e-5 relative error, for per-sample use in hot loops. */
inline float FastExp2(float x)
{
    x = Clamp(x, -126.f, 126.f);
    const float fl = floorf(x);
    const float f  = x - fl;
    /* 5th-order minimax-ish polynomial for 2^f on [0, 1). */
    const float p = 1.f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f
                  + f * (0.0096181f + f * 0.0013334f))));
    union { float f; int32_t i; } u{p};
    u.i += static_cast<int32_t>(fl) << 23;
    return u.f;
}

/** log2(x) for x > 0, ~2e-5 absolute error (one division). */
inline float FastLog2(float x)
{
    union { float f; int32_t i; } u{x};
    const float e = static_cast<float>(((u.i >> 23) & 0xFF) - 127);
    u.i = (u.i & 0x007FFFFF) | 0x3F800000;          /* mantissa m in [1, 2) */
    const float m  = u.f;
    const float t  = (m - 1.f) / (m + 1.f);          /* |t| <= 1/3 */
    const float t2 = t * t;
    const float ln = 2.f * t * (1.f + t2 * (1.f / 3.f + t2 * (0.2f + t2 * (1.f / 7.f))));
    return e + ln * 1.44269504f;
}

/** x^g for x in [0, 1], cheap (table building, not per-sample audio). */
inline float FastPow01(float x, float g)
{
    return x <= 0.f ? 0.f : FastExp2(g * FastLog2(x));
}

/** Wrap a phase into [0, 1). */
inline float Wrap01(float p)
{
    p -= static_cast<float>(static_cast<int32_t>(p));
    return p < 0.f ? p + 1.f : p;
}

/** Smooth saturator (tanh Padé): ~linear below 0.5, gentle knee, ±1.
 *  Smoother than a cubic clip — no hard corner, so no added harshness. */
inline float SoftClip(float x)
{
    if (x > 3.f)  return 1.f;
    if (x < -3.f) return -1.f;
    const float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}

/** tan(x) Padé approximant for the SVF prewarp, x in [0, ~1.4]. */
inline float FastTan(float x)
{
    const float x2 = x * x;
    return x * (15.f - x2) / (15.f - 6.f * x2);
}

/** xorshift32 — deterministic, ISR-safe noise. */
struct Rng
{
    uint32_t s = 0x9E3779B9u;
    uint32_t Next()
    {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return s;
    }
    float Uni() { return static_cast<float>(Next() >> 8) * (1.f / 16777216.f); }  /* 0..1 */
    float Bi()  { return 2.f * Uni() - 1.f; }                                     /* −1..1 */
};

/** One-pole smoother / low-pass.  coef = 1 − e^(−2πfc/fs). */
struct OnePole
{
    float y = 0.f;
    float a = 1.f;
    void  SetHz(float hz, float fs) { a = 1.f - expf(-kTwoPi * hz / fs); }
    void  SetTime(float sec, float fs) { a = sec <= 0.f ? 1.f : 1.f - expf(-1.f / (sec * fs)); }
    float Process(float x) { y += a * (x - y); return y; }
};

/** Peak follower with separate attack / release. */
struct EnvFollow
{
    float y = 0.f, att = 0.5f, rel = 0.001f;
    void  Init(float att_s, float rel_s, float fs)
    {
        att = 1.f - expf(-1.f / (att_s * fs));
        rel = 1.f - expf(-1.f / (rel_s * fs));
    }
    float Process(float x)
    {
        x = fabsf(x);
        y += (x > y ? att : rel) * (x - y);
        return y;
    }
};

/** Topology-preserving-transform state-variable filter (Zavalishin). */
struct Svf
{
    float ic1 = 0.f, ic2 = 0.f;
    float g = 0.1f, k = 1.4f, a1 = 0.f, a2 = 0.f, a3 = 0.f;

    /** fc in Hz, q > 0 (0.5 = very wide, 0.707 = Butterworth). */
    void Set(float fc, float q, float fs)
    {
        fc = Clamp(fc, 10.f, 0.42f * fs);
        g  = FastTan(kPi * fc / fs);
        k  = 1.f / q;
        a1 = 1.f / (1.f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    /** Share another filter's coefficients (keeps this filter's state). */
    void CopyCoeffs(const Svf& o)
    {
        g = o.g; k = o.k; a1 = o.a1; a2 = o.a2; a3 = o.a3;
    }

    struct Out { float lp, bp, hp; };

    Out Tick(float x)
    {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.f * v1 - ic1;
        ic2 = 2.f * v2 - ic2;
        const float hp = x - k * v1 - v2;
        return {v2, v1, hp};
    }

    float Lp(float x)    { return Tick(x).lp; }
    float Hp(float x)    { return Tick(x).hp; }
    /** Notch = input minus the (k-scaled) band-pass. */
    float Notch(float x) { const Out o = Tick(x); return x - k * o.bp; }

    /** Notch with soft saturation on the integrator states — character. */
    float NotchDriven(float x, float drive)
    {
        const Out o = Tick(x);
        ic1 = SoftClip(ic1 * drive) / drive;
        return x - k * o.bp;
    }

    void Reset() { ic1 = ic2 = 0.f; }
};

/** PolyBLEP residual for band-limited saws. */
inline float PolyBlep(float t, float dt)
{
    if (t < dt)
    {
        t /= dt;
        return t + t - t * t - 1.f;
    }
    if (t > 1.f - dt)
    {
        t = (t - 1.f) / dt;
        return t * t + t + t + 1.f;
    }
    return 0.f;
}

struct BlepSaw
{
    float phase = 0.f;
    float Tick(float dt)
    {
        float y = 2.f * phase - 1.f;
        y -= PolyBlep(phase, dt);
        phase += dt;
        if (phase >= 1.f) phase -= 1.f;
        return y;
    }
};

/** DC blocker (one-zero / one-pole high-pass at ~10 Hz). */
struct DcBlock
{
    float x1 = 0.f, y1 = 0.f;
    float Process(float x)
    {
        const float y = x - x1 + 0.9987f * y1;
        x1 = x;
        y1 = y;
        return y;
    }
};

/** 4-point, 3rd-order Hermite interpolation. */
inline float Hermite(float xm1, float x0, float x1, float x2, float t)
{
    const float c0 = x0;
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + c0;
}

} // namespace sim
