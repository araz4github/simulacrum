/**
 * host_test.cpp — functional tests of the Simulacrum DSP on the host.
 *
 * Builds engine.cpp / tape.cpp / grain.cpp / stack.cpp natively against
 * two tiny libDaisy stubs (tests/stubs) and checks behaviour, stability
 * and a relative cost profile.  Run:  make -C tests  (or see Makefile).
 *
 * These prove the DSP does what the spec says and never blows up; they
 * do NOT prove the Cortex-M7 is fast enough — that is the CPU estimate
 * plus the on-device "Audio CPU" gauge.
 */

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "engine.h"
#include "sim_shared.h"
#include "grain.h"
#include "stack.h"

using namespace sim;

static constexpr float  kFs    = 48000.f;
static constexpr size_t kBlock = 24;

static int g_fail = 0, g_pass = 0;

#define CHECK(cond, ...)                                                    \
    do {                                                                   \
        if (cond) { g_pass++; std::printf("  PASS  "); }                   \
        else      { g_fail++; std::printf("  FAIL  "); }                   \
        std::printf(__VA_ARGS__);                                          \
        std::printf("\n");                                                 \
    } while (0)

/* ── Helpers ─────────────────────────────────────────────────────────── */

struct Stereo { std::vector<float> l, r; };

using Gen = float (*)(size_t i, int ch);

static float Silence(size_t, int) { return 0.f; }

/* Index-addressed white noise, so a test can recompute the input. */
static float Noise(size_t i, int ch)
{
    uint32_t h = static_cast<uint32_t>(i) * 2654435761u ^ (ch ? 0x9E3779B9u : 0x85EBCA6Bu);
    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
    return 0.5f * (static_cast<float>(h >> 8) / 8388608.f - 1.f);
}

static float g_sine_hz = 440.f;
static float Sine(size_t i, int)
{
    return 0.5f * sinf(6.2831853f * g_sine_hz * static_cast<float>(i) / kFs);
}

static size_t g_clock = 0;   /* global sample clock across runs */

/** Run the engine for @p seconds, input from @p gen, return the output. */
static Stereo Run(float seconds, Gen gen, double* cpu_ns = nullptr)
{
    const size_t n = static_cast<size_t>(seconds * kFs);
    Stereo o;
    o.l.resize(n);
    o.r.resize(n);
    float il[kBlock], ir[kBlock], ol[kBlock], orr[kBlock];
    const float* in[2] = {il, ir};
    float*       out[2] = {ol, orr};
    double ns = 0.0;
    for (size_t b = 0; b + kBlock <= n; b += kBlock)
    {
        for (size_t i = 0; i < kBlock; i++)
        {
            il[i] = gen(g_clock + b + i, 0);
            ir[i] = gen(g_clock + b + i, 1);
        }
        const auto t0 = std::chrono::steady_clock::now();
        EngineProcess(in, out, kBlock);
        ns += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
        std::memcpy(&o.l[b], ol, sizeof ol);
        std::memcpy(&o.r[b], orr, sizeof orr);
    }
    g_clock += n;
    if (cpu_ns) *cpu_ns = ns / static_cast<double>(n / kBlock);
    return o;
}

static bool Finite(const std::vector<float>& v)
{
    for (float x : v) if (!std::isfinite(x)) return false;
    return true;
}

static float Peak(const std::vector<float>& v, size_t a = 0, size_t b = 0)
{
    if (b == 0 || b > v.size()) b = v.size();
    float p = 0.f;
    for (size_t i = a; i < b; i++) p = std::fmax(p, std::fabs(v[i]));
    return p;
}

static float Rms(const std::vector<float>& v, size_t a = 0, size_t b = 0)
{
    if (b == 0 || b > v.size()) b = v.size();
    double s = 0.0;
    for (size_t i = a; i < b; i++) s += static_cast<double>(v[i]) * v[i];
    return static_cast<float>(std::sqrt(s / static_cast<double>(b - a)));
}

/** Goertzel power at @p hz over [a, b). */
static float Tone(const std::vector<float>& v, float hz, size_t a = 0, size_t b = 0)
{
    if (b == 0 || b > v.size()) b = v.size();
    const double w = 2.0 * M_PI * hz / kFs, c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (size_t i = a; i < b; i++)
    {
        const double s0 = v[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return static_cast<float>(p / static_cast<double>((b - a) * (b - a)));
}

/** Hann-windowed Goertzel power (low leakage, for narrow notches). */
static float ToneW(const std::vector<float>& v, float hz, size_t a, size_t b)
{
    const double w = 2.0 * M_PI * hz / kFs, c = 2.0 * std::cos(w);
    const double L = static_cast<double>(b - a);
    double s1 = 0.0, s2 = 0.0;
    for (size_t i = a; i < b; i++)
    {
        const double win = 0.5 - 0.5 * std::cos(2.0 * M_PI * static_cast<double>(i - a) / L);
        const double s0 = v[i] * win + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return static_cast<float>(s1 * s1 + s2 * s2 - c * s1 * s2);
}

/** Band ratio out/in around @p hz (±1 %), Hann-windowed, in dB. */
static float BandDb(const std::vector<float>& out, const std::vector<float>& in, float hz)
{
    float po = 0.f, pi = 0.f;
    for (float f = 0.99f * hz; f <= 1.01f * hz; f += 0.002f * hz)
    {
        po += ToneW(out, f, 4800, out.size());
        pi += ToneW(in,  f, 4800, in.size());
    }
    return 10.f * log10f(po / pi);
}

static float Db(float ratio) { return 10.f * log10f(ratio + 1e-30f); }

/** Strongest frequency on a 1 Hz grid between lo and hi. */
static float PeakFreq(const std::vector<float>& v, float lo, float hi, size_t a, size_t b)
{
    float best = lo, bp = 0.f;
    for (float f = lo; f <= hi; f += 1.f)
    {
        const float p = Tone(v, f, a, b);
        if (p > bp) { bp = p; best = f; }
    }
    return best;
}

/** Default "everything neutral" parameter set. */
static void Neutral()
{
    g_params = Params{};
    g_params.tape_mix      = 0.f;
    g_params.tape_feedback = 0.f;
    g_params.tape_wow      = 0.f;
    g_params.tape_micro    = 0.5f;
    g_params.tape_tone     = 0.5f;
    g_params.grain_mix     = 0.f;
    g_params.stack_level   = 0.f;
    g_params.stack_cutoff  = 0.5f;
    g_params.stack_feast   = 0.f;
    g_params.in_gain       = 1.f;
    g_params.out_level     = 1.f;
    g_params.sub_depth     = 0.f;
}

/* ── Tests ───────────────────────────────────────────────────────────── */

static void TestPassthrough()
{
    std::printf("\n[mix] both mixes dry → output follows input\n");
    Neutral();
    EngineInit(kFs, kBlock);
    Run(0.5f, Silence);
    g_sine_hz = 440.f;
    Stereo o = Run(1.f, Sine);
    const float in_rms = 0.5f / std::sqrt(2.f);
    CHECK(std::fabs(Rms(o.l, 24000) - in_rms) < 0.02f,
          "dry RMS %.3f (input %.3f)", Rms(o.l, 24000), in_rms);
}

static size_t g_impulse_at = 0;
static float Impulse(size_t i, int) { return i == g_impulse_at ? 1.f : 0.f; }

static void TestTapeDelayTime()
{
    std::printf("\n[tape] clock-synced delay time and repeats\n");
    Neutral();
    g_params.tape_mix      = 1.f;
    g_params.tape_feedback = 0.5f;
    g_params.clock_period_s = 0.25f;
    g_params.tape_ratio    = 1.f;   /* 250 ms */
    EngineInit(kFs, kBlock);
    Run(1.5f, Silence);             /* let the glide settle at 250 ms */
    g_impulse_at = g_clock + 1000;
    Stereo o = Run(1.2f, Impulse);

    /* First repeat ≈ 1000 + 12000 samples into this run. */
    size_t at = 0;
    float  pk = 0.f;
    for (size_t i = 0; i < 20000; i++)
        if (std::fabs(o.l[i]) > pk) { pk = std::fabs(o.l[i]); at = i; }
    const float ms = static_cast<float>(at - 1000) / 48.f;
    CHECK(std::fabs(ms - 250.f) < 2.f, "first repeat at %.1f ms (target 250)", ms);

    const float p1 = Peak(o.l, 12500, 14000), p2 = Peak(o.l, 24500, 26000),
                p3 = Peak(o.l, 36500, 38000);
    CHECK(p2 < p1 && p3 < p2, "repeats decay %.3f → %.3f → %.3f", p1, p2, p3);
    CHECK(p2 / p1 > 0.25f && p2 / p1 < 0.7f, "repeat ratio %.2f at 50%% feedback", p2 / p1);
}

static void TestTapeStability()
{
    std::printf("\n[tape] 105%% feedback stays bounded (tape saturation)\n");
    Neutral();
    g_params.tape_mix      = 1.f;
    g_params.tape_feedback = 1.05f;
    g_params.tape_wow      = 1.f;
    g_params.clock_period_s = 0.3f;
    EngineInit(kFs, kBlock);
    Run(0.5f, Noise);
    Stereo o = Run(20.f, Silence);
    CHECK(Finite(o.l) && Finite(o.r), "no NaN / inf over 20 s");
    CHECK(Peak(o.l) <= 1.0f && Peak(o.r) <= 1.0f, "peak %.2f / %.2f (≤ 1.0 full scale)",
          Peak(o.l), Peak(o.r));
    CHECK(Rms(o.l, o.l.size() - 48000) > 0.02f, "self-oscillation sustains (RMS %.3f)",
          Rms(o.l, o.l.size() - 48000));
}

static void TestTapeFreeze()
{
    std::printf("\n[tape] freeze holds the loop\n");
    Neutral();
    g_params.tape_mix      = 1.f;
    g_params.tape_feedback = 0.3f;
    g_params.clock_period_s = 0.5f;
    EngineInit(kFs, kBlock);
    Run(1.f, Noise);
    g_params.tape_freeze = 1;
    Stereo o = Run(10.f, Noise);   /* input ignored while frozen */
    const float early = Rms(o.l, 24000, 72000), late = Rms(o.l, o.l.size() - 48000);
    CHECK(late > 0.7f * early && late < 1.3f * early,
          "level after 10 s: %.0f %% of start", 100.f * late / early);
}

static void TestTapeTone()
{
    std::printf("\n[tape] tone darkens more on every repeat (CCW)\n");
    Neutral();
    g_params.tape_mix      = 1.f;
    g_params.tape_feedback = 0.85f;
    g_params.tape_tone     = 0.15f;
    g_params.clock_period_s = 0.25f;
    EngineInit(kFs, kBlock);
    Run(1.5f, Silence);
    Run(0.05f, Noise);
    Stereo o = Run(1.5f, Silence);
    /* Ratio of 4 kHz to 300 Hz energy in repeat 1 vs repeat 4. */
    auto bright = [&](size_t a) {
        return Tone(o.l, 4000.f, a, a + 2400) / (Tone(o.l, 300.f, a, a + 2400) + 1e-20f);
    };
    const float r1 = bright(12000 - 2400), r4 = bright(4 * 12000 - 2400);
    CHECK(r4 < 0.5f * r1, "HF/LF ratio repeat 1 %.2e → repeat 4 %.2e", r1, r4);
}

static void TestMicrospeed()
{
    std::printf("\n[tape] microspeed: full CCW, +3 V → +12 st (1/3 V/oct scaling)\n");
    Neutral();
    g_params.tape_mix      = 1.f;
    g_params.tape_feedback = 0.f;
    g_params.tape_micro    = 0.f;      /* full CCW = V/Oct depth 1 */
    g_params.voct_volts    = 3.f;      /* 3 V × 4 st/V = +12 st */
    g_params.clock_period_s = 0.3f;
    g_sine_hz = 300.f;
    EngineInit(kFs, kBlock);
    Stereo o = Run(2.f, Sine);
    /* A two-head shifter puts ±(crossfade rate) sidebands around the new
     * pitch, so compare band energy rather than the single strongest bin. */
    auto band = [&](float c) {
        float e = 0.f;
        for (float f = c - 30.f; f <= c + 30.f; f += 2.f) e += Tone(o.l, f, 48000, 96000);
        return e;
    };
    const float up = band(600.f), orig = band(300.f);
    CHECK(Db(up / orig) > 12.f, "energy at 600 Hz vs 300 Hz: %+.1f dB (input 300 Hz)",
          Db(up / orig));
}

static float LeftOnly(size_t i, int ch) { return ch == 0 ? Sine(i, 0) : 0.f; }

static void TestMonoNormal()
{
    std::printf("\n[input] J2 silent → J1 copied to J2; real J2 signal wins\n");
    Neutral();
    EngineInit(kFs, kBlock);
    g_sine_hz = 440.f;
    Stereo o = Run(9.f, LeftOnly);
    CHECK(Rms(o.r, 400000) < 0.01f, "J2 empty for 9 s: not copied yet (R %.3f)", Rms(o.r, 400000));
    o = Run(2.f, LeftOnly);
    const float rl = Rms(o.r, 60000), ll = Rms(o.l, 60000);
    CHECK(std::fabs(rl - ll) < 0.01f, "after 10 s: R %.3f follows L %.3f", rl, ll);
    o = Run(1.f, Noise);
    float d = 0.f;
    for (size_t i = 24000; i < o.l.size(); i++) d += std::fabs(o.l[i] - o.r[i]);
    CHECK(d / static_cast<float>(o.l.size() - 24000) > 0.05f, "J2 patched: L and R independent");
}

static void TestGrains()
{
    std::printf("\n[grain] grains play, pitch in semitones, speed keeps pitch\n");
    Neutral();
    g_params.grain_mix    = 1.f;
    g_params.grain_count  = 6;
    g_params.grain_size_s = 0.12f;
    g_params.grain_shape  = 2.f / 3.f;   /* triangle */
    g_params.clock_period_s = 0.4f;
    g_sine_hz = 330.f;
    EngineInit(kFs, kBlock);
    Stereo o = Run(3.f, Sine);
    CHECK(Finite(o.l) && Rms(o.l, 96000) > 0.05f, "grain output RMS %.3f", Rms(o.l, 96000));

    {
        /* Grains alone (no dry): +12 st must land on the octave. */
        static int16_t* gbuf = new int16_t[Grains::kBufFrames * 2u];
        Grains gr;
        gr.Init(gbuf, kFs);
        GrainControls c{};
        c.period_s = 0.4f; c.size_s = 0.12f; c.count = 6; c.per_clock = 3; c.count_knob = 6;
        c.shape = 2.f / 3.f; c.speed = 1.f; c.pitch_st = 12.f; c.render = true;
        const size_t n = static_cast<size_t>(3.f * kFs);
        std::vector<float> in(n), ol(n), orr(n), fl(n), fr(n);
        for (size_t i = 0; i < n; i++) in[i] = Sine(i, 0);
        for (size_t b = 0; b + kBlock <= n; b += kBlock)
            gr.Process(&in[b], &in[b], &in[b], &in[b], &ol[b], &orr[b], &fl[b], &fr[b], kBlock, c);
        const float up = Tone(ol, 660.f, 96000), base = Tone(ol, 330.f, 96000);
        CHECK(up > 100.f * base, "+12 st: grains at 660 Hz, %.0f dB over 330 Hz", Db(up / base));
    }
    float f = 0.f;

    g_params.grain_pitch_st = 0.f;
    g_params.grain_speed    = 8.f;
    o = Run(3.f, Sine);
    f = PeakFreq(o.l, 200.f, 800.f, 96000, 144000);
    CHECK(std::fabs(f - 330.f) < 5.f, "speed ×8: pitch unchanged at %.0f Hz", f);
}

/** Grains alone: one grain per 4 s clock, size 2 s, at a given speed. */
static void GrainSpeedCase(float speed, float expect_s)
{
    static int16_t* buf = new int16_t[Grains::kBufFrames * 2u];
    Grains gr;
    gr.Init(buf, kFs);
    GrainControls c{};
    c.period_s = 4.f; c.size_s = 2.f; c.count = 1; c.per_clock = 1; c.count_knob = 1;
    c.shape = 0.f; c.speed = speed; c.spray = 0.f; c.pitch_st = 0.f; c.pitch_rand = 0.f;
    c.dir = 0; c.spread = 0.f; c.freeze = false; c.source_mix = 0.f; c.octaves = 0;
    c.feast_active = false; c.render = true;
    const size_t n = static_cast<size_t>(16.f * kFs);
    std::vector<float> in(n), out_l(n), out_r(n), fl(n), fr(n);
    g_sine_hz = 330.f;
    for (size_t i = 0; i < n; i++) in[i] = Sine(i, 0);
    for (size_t b = 0; b + kBlock <= n; b += kBlock)
        gr.Process(&in[b], &in[b], &in[b], &in[b], &out_l[b], &out_r[b], &fl[b], &fr[b], kBlock, c);
    /* Duration of the grain that starts at the 12 s trigger. */
    const size_t a = static_cast<size_t>(12.f * kFs);
    size_t last = a;
    for (size_t i = a; i < n; i++) if (std::fabs(out_l[i]) > 0.02f) last = i;
    size_t first = n;
    for (size_t i = a - 2400; i < n; i++) if (std::fabs(out_l[i]) > 0.02f) { first = i; break; }
    const float dur = static_cast<float>(last - first) / kFs;
    const size_t m0 = first + 2400, m1 = std::min(last, m0 + 24000);
    const float f = PeakFreq(out_l, 250.f, 450.f, m0, m1);
    CHECK(std::fabs(dur - expect_s) < 0.08f * expect_s + 0.02f && std::fabs(f - 330.f) < 4.f,
          "2 s grain at %.4g×: plays %.2f s (expect %.2f), pitch %.0f Hz", speed, dur, expect_s, f);
}

static void TestGrainSpeed()
{
    std::printf("\n[grain] speed changes grain length, never pitch\n");
    GrainSpeedCase(1.f, 2.f);
    GrainSpeedCase(2.f, 1.f);
    GrainSpeedCase(0.5f, 4.f);
}

static void TestGrainCrunch()
{
    std::printf("\n[grain] 12-bit / 24 kHz buffer: no content above ~11 kHz\n");
    Neutral();
    g_params.grain_mix    = 0.5f;      /* 50/50 dry/wet; dry kept for ref */
    g_params.grain_count  = 8;
    g_params.grain_size_s = 0.3f;
    g_params.grain_shape  = 0.f;       /* square */
    EngineInit(kFs, kBlock);
    Stereo o = Run(3.f, Noise);
    /* Full mix = 25 % dry + 75 % wet: subtract the known dry part to
     * measure the grains alone. */
    g_params.grain_mix = 1.f;
    Run(0.5f, Noise);
    const size_t start = g_clock;
    o = Run(3.f, Noise);
    std::vector<float> wet(o.l.size());
    for (size_t i = 0; i < wet.size(); i++) wet[i] = o.l[i] - 0.25f * Noise(start + i, 0);
    const float hf = Tone(wet, 16000.f, 48000), mf = Tone(wet, 3000.f, 48000);
    CHECK(Db(hf / mf) < -20.f, "grains alone, 16 kHz vs 3 kHz: %.1f dB", Db(hf / mf));
}

/** Hold the J6 gate and start a note at @p volts (control-loop path). */
static void Note(float volts)
{
    g_params.gated_mode = 1u;
    g_params.gate       = 1u;
    g_params.note_volts = volts;
    g_params.note_seq++;
}

static void TestStackPitch()
{
    std::printf("\n[stack] notes track tuning + V/Oct at 1 V/oct\n");
    Neutral();
    g_params.stack_level = 1.f;
    g_params.sine_saw    = 0.f;
    g_params.stack_voices = 1;
    EngineInit(kFs, kBlock);
    Note(0.f);
    Stereo o = Run(1.f, Silence);
    float f = PeakFreq(o.l, 200.f, 330.f, 12000, 48000);
    CHECK(std::fabs(f - 261.6f) < 1.5f, "C4 at noon: %.1f Hz", f);

    Note(1.f);
    o = Run(1.f, Silence);
    f = PeakFreq(o.l, 450.f, 600.f, 12000, 48000);
    CHECK(std::fabs(f - 523.3f) < 2.f, "+1 V: %.1f Hz (expect 523.3)", f);

    g_params.stack_tune_oct = -1.f;
    Note(0.f);
    o = Run(1.f, Silence);
    f = PeakFreq(o.l, 100.f, 160.f, 12000, 48000);
    CHECK(std::fabs(f - 130.8f) < 1.5f, "tune −1 oct: %.1f Hz (expect 130.8)", f);

    g_params.stack_tune_oct = 0.f;
    g_params.sine_saw = 1.f;
    Note(0.f);
    o = Run(1.f, Silence);
    CHECK(Tone(o.l, 261.6f, 12000) > 4.f * Tone(o.l, 523.3f, 12000),
          "50/50 sine/saw: fundamental %.1f dB over 2nd harmonic",
          Db(Tone(o.l, 261.6f, 12000) / Tone(o.l, 523.3f, 12000)));
}

static void TestStackTone()
{
    std::printf("\n[stack] gentle ladder low-pass trims only the highest highs\n");
    Neutral();
    g_params.stack_level  = 1.f;
    g_params.sine_saw     = 1.f;      /* saws on: plenty of harmonics */
    g_params.stack_detune = 0.f;      /* keep harmonics on exact bins */
    g_params.stack_voices = 1;
    EngineInit(kFs, kBlock);
    Note(0.f);
    Stereo o = Run(1.f, Silence);
    const float h2k  = Tone(o.l, 2093.f, 12000);    /* 8th harmonic of C4 */
    const float h16k = Tone(o.l, 16744.f, 12000);   /* 64th */
    CHECK(Db(h16k / h2k) < -30.f, "64th harmonic vs 8th: %.1f dB (saws alone ≈ −18)", Db(h16k / h2k));
    const float h1k = Tone(o.l, 1046.5f, 12000);    /* 4th */
    CHECK(Db(h2k / h1k) > -9.f, "mids untouched: 8th vs 4th harmonic %.1f dB (saw ≈ −6)", Db(h2k / h1k));
}

/** Peak of one grain (LPG shape) of the given size. */
static float OneGrainPeak(float size_s)
{
    static int16_t* buf = new int16_t[Grains::kBufFrames * 2u];
    Grains gr;
    gr.Init(buf, kFs);
    GrainControls c{};
    c.period_s = 2.f; c.size_s = size_s; c.count = 1; c.per_clock = 1; c.count_knob = 1;
    c.shape = 1.f / 3.f; c.speed = 1.f; c.render = true;
    const size_t n = static_cast<size_t>(4.5f * kFs);
    std::vector<float> in(n), ol(n), orr(n), fl(n), fr(n);
    for (size_t i = 0; i < n; i++) in[i] = Sine(i, 0);
    for (size_t b = 0; b + kBlock <= n; b += kBlock)
        gr.Process(&in[b], &in[b], &in[b], &in[b], &ol[b], &orr[b], &fl[b], &fr[b], kBlock, c);
    return Peak(ol, static_cast<size_t>(3.9f * kFs));
}

static void TestStackLpg()
{
    std::printf("\n[stack] LPG-style: notes darken as they fade\n");
    Neutral();
    g_params.stack_level   = 1.f;
    g_params.sine_saw      = 1.f;
    g_params.stack_detune  = 0.f;
    g_params.stack_voices  = 1;
    g_params.stack_release = 0.6f;      /* ≈ 2.3 s ring-out */
    EngineInit(kFs, kBlock);
    Note(0.f);
    Stereo o = Run(0.5f, Silence);
    const float loud = Tone(o.l, 4186.f, 12000) / Tone(o.l, 261.6f, 12000);   /* 16th */
    g_params.gate = 0u;
    Run(1.2f, Silence);
    o = Run(0.3f, Silence);
    const float quiet = Tone(o.l, 4186.f) / Tone(o.l, 261.6f);
    CHECK(Db(quiet / loud) < -0.8f, "16th harmonic vs fundamental while fading: %.1f dB darker",
          -Db(quiet / loud));
}

static void TestShortGrainLift()
{
    std::printf("\n[grain] short grains are lifted to normal level\n");
    g_sine_hz = 440.f;
    const float long_pk = OneGrainPeak(0.5f), short_pk = OneGrainPeak(0.02f);
    CHECK(short_pk > 0.35f * long_pk, "20 ms grain peak %.2f vs 500 ms grain %.2f (lift ≤ +6 dB)",
          short_pk, long_pk);
}

static void TestNotesAndEnvelopes()
{
    std::printf("\n[stack] gate, sustain, release, ringing voices\n");
    Neutral();
    g_params.stack_level   = 1.f;
    g_params.sine_saw      = 0.f;
    g_params.stack_voices  = 4;
    g_params.stack_release = 0.15f;     /* ≈ 85 ms */
    EngineInit(kFs, kBlock);

    Stereo o = Run(0.3f, Silence);
    CHECK(Rms(o.l) < 1e-4f, "no gate, no notes: silent (RMS %.1e)", Rms(o.l));

    /* Gate held, three quick notes: earlier ones ring out on their own
     * release (≈155 ms) while the newest plays. */
    Note(0.f);  Run(0.06f, Silence);
    Note(0.25f); Run(0.04f, Silence);
    Note(7.f / 12.f);
    Run(0.02f, Silence);
    CHECK(g_tel.voices_sounding == 3, "3 quick notes → %u voices ringing together",
          g_tel.voices_sounding);
    o = Run(1.f, Silence);
    CHECK(g_tel.voices_sounding == 1, "a second later: %u (older notes rang out)",
          g_tel.voices_sounding);
    float mx = 0.f;
    for (float e : g_tel.voice_env) mx = std::fmax(mx, e);
    CHECK(std::fabs(mx - 0.5f) < 0.02f, "newest note sustains at %.2f (expect 0.50)", mx);
    const float f = PeakFreq(o.l, 350.f, 420.f, 24000, 48000);
    CHECK(std::fabs(f - 392.f) < 2.f, "it is the newest pitch: %.1f Hz (G4 392)", f);

    /* Gate released: all ring out within the release time. */
    g_params.gate = 0u;
    o = Run(0.25f, Silence);
    CHECK(g_tel.voices_sounding == 0, "after release: %u voices sounding", g_tel.voices_sounding);

    /* Voices = 1: a new note takes the only voice. */
    g_params.stack_voices = 1;
    Note(0.f); Run(0.05f, Silence);
    Note(0.5f);
    Run(0.2f, Silence);
    CHECK(g_tel.voices_sounding == 1, "Voices = 1: %u sounding", g_tel.voices_sounding);
    g_params.gate = 0u;
    Run(0.3f, Silence);
}

static void TestNotch()
{
    std::printf("\n[stack] dual notch (original): noon passes, CCW cuts at the set frequencies\n");
    Stack st;
    st.Init(kFs);
    const size_t n = 96000;
    std::vector<float> nl(n), nr(n), ol(n), orr(n), sub(n, 0.f);
    for (size_t i = 0; i < n; i++) { nl[i] = Noise(i, 0); nr[i] = Noise(i, 1); }

    auto run = [&](float cutoff, float spacing) {
        StackControls c{};
        c.cutoff = cutoff; c.spacing = spacing; c.voices = 1; c.gated_mode = true;
        st.BeginBlock(c, kBlock);
        for (size_t b = 0; b < n; b += kBlock)
            st.Process(&nl[b], &nr[b], &sub[b], &ol[b], &orr[b], kBlock, c);
    };

    run(0.5f, 0.f);
    float a = Tone(ol, 1000.f, 4800) / Tone(nl, 1000.f, 4800);
    CHECK(std::fabs(Db(a)) < 1.f, "noon: 1 kHz change %.1f dB", Db(a));

    /* Cutoff curve anchors: 0.55 of the way out → 400 Hz;
     * spacing 1/3 → upper notch one octave up ≈ 1550 Hz. */
    run(0.225f, 1.f / 3.f);
    /* Cutoff 0.225 → 0.55 of the way out → lower notch 400 Hz. */
    const float lo  = BandDb(ol, nl, 400.f);
    const float hi  = BandDb(ol, nl, 800.f);
    const float ref = BandDb(ol, nl, 6000.f);
    CHECK(lo < -12.f && hi < -12.f, "notches: 400 Hz %.1f dB, 800 Hz %.1f dB", lo, hi);
    CHECK(std::fabs(ref) < 4.f, "away from notches (6 kHz): %+.1f dB", ref);

    /* Original gentle notch: no resonant peak around it, and Spacing
     * moves the upper notch (3 oct → ~6.2 kHz). */
    run(0.225f, 1.f);
    float peak = -100.f;
    for (float r = 1.2f; r < 3.f; r += 0.1f) peak = std::fmax(peak, BandDb(ol, nl, 400.f * r));
    CHECK(peak < 1.f, "no resonant peak around the notch (max %+.1f dB)", peak);
    const float moved = BandDb(ol, nl, 3200.f);
    CHECK(moved < -12.f, "Spacing 3 oct: upper notch at 3.2 kHz (%.1f dB)", moved);
}

static void TestStereoCutoff()
{
    std::printf("\n[stack] CW cutoff: lows summed to centre, highs stay wide\n");
    Stack st;
    st.Init(kFs);
    const size_t n = 96000;
    std::vector<float> nl(n), nr(n), ol(n), orr(n), sub(n, 0.f), side(n);
    for (size_t i = 0; i < n; i++) { nl[i] = Noise(i, 0); nr[i] = Noise(i, 1); }
    StackControls c{};
    c.cutoff = 0.8f; c.spacing = 0.3f; c.voices = 1; c.gated_mode = true;
    st.BeginBlock(c, kBlock);
    for (size_t b = 0; b < n; b += kBlock)
        st.Process(&nl[b], &nr[b], &sub[b], &ol[b], &orr[b], kBlock, c);
    for (size_t i = 0; i < n; i++) side[i] = ol[i] - orr[i];
    std::vector<float> in_side(n);
    for (size_t i = 0; i < n; i++) in_side[i] = nl[i] - nr[i];
    const float low  = Tone(side, 60.f, 4800) / Tone(in_side, 60.f, 4800);
    const float high = Tone(side, 5000.f, 4800) / Tone(in_side, 5000.f, 4800);
    CHECK(Db(low) < -10.f, "side signal at 60 Hz: %.1f dB (lows centred)", Db(low));
    CHECK(Db(high) > -6.f, "side signal at 5 kHz: %.1f dB (highs wide)", Db(high));
}

static void WorstParams()
{
    Neutral();
    Params& p = g_params;
    p.tape_mix = 1.f; p.tape_feedback = 1.05f; p.tape_wow = 1.f; p.tape_micro = 1.f;
    p.tape_tone = 0.1f; p.tape_pingpong = 1;
    p.grain_mix = 1.f; p.grain_count = 36; p.grain_size_s = 4.f; p.grain_shape = 1.f / 3.f;
    p.grain_ratio = 0.125f; p.grain_speed = 1.5f; p.grain_spray = 1.f;
    p.grain_pitch_st = 12.f; p.grain_pitch_rand = 1.f; p.grain_dir = 2;
    p.stack_level = 1.f; p.stack_detune = 1.f; p.sine_saw = 1.f; p.octaves = 2;
    p.stack_cutoff = 0.9f; p.stack_spacing = 1.f; p.sub_depth = 1.f; p.stack_feast = 0.7f;
    p.spread = 1.f; p.voct_volts = 1.f; p.voct_grains = 1;
    p.stack_voices = 8; p.stack_release = 1.f; p.gated_mode = 1u; p.gate = 1u;
}

static void TestWorstCase()
{
    std::printf("\n[stress] everything on, 30 s: stability + host cost profile\n");
    WorstParams();
    EngineInit(kFs, kBlock);
    double ns = 0.0;
    Stereo o = Run(30.f, Noise, &ns);
    CHECK(Finite(o.l) && Finite(o.r), "no NaN / inf");
    CHECK(Peak(o.l) <= 1.f && Peak(o.r) <= 1.f, "output within full scale (%.2f)",
          std::fmax(Peak(o.l), Peak(o.r)));
    std::printf("  INFO  host: %.0f ns per 24-sample block (worst case)\n", ns);

    Neutral();
    EngineInit(kFs, kBlock);
    double idle = 0.0;
    Run(5.f, Noise, &idle);
    std::printf("  INFO  host: %.0f ns per block with every layer at rest\n", idle);
}

/* Bench mode for instruction counting:  host_test bench worst|idle|none <s>
 * Run under /usr/bin/time -l and subtract the "none" (setup) count. */
static int Bench(const char* scenario, float seconds)
{
    const bool worst = std::strncmp(scenario, "worst", 5) == 0;
    if (worst) WorstParams(); else Neutral();
    /* Ablations: switch one feature off from the worst case. */
    if (std::strstr(scenario, "-nofeast"))  g_params.stack_feast = 0.f;
    if (std::strstr(scenario, "-nooct"))    g_params.octaves = 0;
    if (std::strstr(scenario, "-nostack"))  { g_params.stack_level = 0.f; g_params.stack_cutoff = 0.5f; g_params.stack_feast = 0.f; }
    if (std::strstr(scenario, "-onegrain")) g_params.grain_count = 1;
    if (std::strstr(scenario, "-nopitch"))  g_params.tape_micro = 0.5f;
    if (std::strstr(scenario, "-mono"))     g_params.stack_cutoff = 0.1f;
    /* Grain-heavy cases on top of a quiet patch. */
    if (std::strncmp(scenario, "grains", 6) == 0)
    {
        Neutral();
        Params& p = g_params;
        p.grain_mix = 0.5f; p.grain_size_s = 4.f; p.grain_ratio = 1.f; p.clock_period_s = 0.5f;
        p.grain_shape = 0.f;
        p.grain_count = static_cast<uint32_t>(std::atoi(scenario + 6));   /* e.g. grains24 */
        if (std::strstr(scenario, "-stretch")) p.grain_speed = 2.f;
    }
    EngineInit(kFs, kBlock);
    if (std::strncmp(scenario, "stack", 5) == 0)
    {
        /* stackN: N held notes, saws on, octaves on. */
        g_params.stack_level = 1.f; g_params.sine_saw = 0.6f; g_params.octaves = 2;
        g_params.stack_cutoff = 0.8f; g_params.sub_depth = 0.5f;
        const int nv = std::atoi(scenario + 5);
        g_params.stack_voices = static_cast<uint32_t>(nv);
        for (int i = 0; i < nv; i++) { Note(0.1f * i); Run(0.01f, Silence); }
    }
    if (worst)
        for (int i = 0; i < 8; i++) { Note(0.1f * i); Run(0.01f, Silence); }
    if (std::strncmp(scenario, "heavy", 5) == 0)
    {
        /* Typical heavy patch: tape on, 12 slow grains, 4 ringing notes. */
        Params& p = g_params;
        p.tape_mix = 0.5f; p.tape_feedback = 0.5f; p.tape_wow = 0.3f;
        p.grain_mix = 0.5f; p.grain_count = 12; p.grain_size_s = 1.f; p.grain_speed = 0.5f;
        p.grain_ratio = 0.5f; p.stack_level = 0.7f; p.stack_voices = 4; p.stack_release = 0.7f;
        p.stack_cutoff = 0.3f; p.sine_saw = 0.5f;
        for (int i = 0; i < 4; i++) { Note(0.1f * i); Run(0.01f, Silence); }
    }
    if (std::strcmp(scenario, "none") != 0) Run(seconds, Noise);
    if (std::getenv("SIM_PROF"))
        std::printf("share of total: tape %.0f%%  grains %.0f%%  stack %.0f%%  rest %.0f%%\n",
                    100.f * g_tel.prof_tape / (g_tel.prof_tape + g_tel.prof_grain + g_tel.prof_stack + g_tel.prof_other),
                    100.f * g_tel.prof_grain / (g_tel.prof_tape + g_tel.prof_grain + g_tel.prof_stack + g_tel.prof_other),
                    100.f * g_tel.prof_stack / (g_tel.prof_tape + g_tel.prof_grain + g_tel.prof_stack + g_tel.prof_other),
                    100.f * g_tel.prof_other / (g_tel.prof_tape + g_tel.prof_grain + g_tel.prof_stack + g_tel.prof_other));
    return 0;
}

int main(int argc, char** argv)
{
    if (argc >= 4 && std::strcmp(argv[1], "bench") == 0)
        return Bench(argv[2], static_cast<float>(std::atof(argv[3])));

    std::printf("Simulacrum host DSP tests\n");
    TestPassthrough();
    TestMonoNormal();
    TestTapeDelayTime();
    TestTapeStability();
    TestTapeFreeze();
    TestTapeTone();
    TestMicrospeed();
    TestGrains();
    TestGrainSpeed();
    TestGrainCrunch();
    TestStackPitch();
    TestNotesAndEnvelopes();
    TestStackTone();
    TestStackLpg();
    TestShortGrainLift();
    TestNotch();
    TestStereoCutoff();
    TestWorstCase();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
