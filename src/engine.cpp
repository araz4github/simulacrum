/**
 * engine.cpp — wiring of the three layers and the output mix.
 */

#include "engine.h"

#include "daisy_seed.h"
#include "util/CpuLoadMeter.h"

#include "dsp_util.h"
#include "grain.h"
#include "sim_shared.h"
#include "stack.h"
#include "tape.h"

namespace sim {

Params    g_params;
Telemetry g_tel;

namespace {

/* Large buffers live in the 64 MB SDRAM (NOLOAD; cleared in Init). */
float   DSY_SDRAM_BSS tape_buf_l[Tape::kBufLen];
float   DSY_SDRAM_BSS tape_buf_r[Tape::kBufLen];
alignas(4) int16_t DSY_SDRAM_BSS grain_buf[Grains::kBufFrames * 2u];

Tape   tape;
Grains grains;
Stack  stack;

daisy::CpuLoadMeter cpu;

constexpr size_t kMaxBlock = 64u;
float in_l[kMaxBlock], in_r[kMaxBlock];
float sub[kMaxBlock];
float deep_sub[kMaxBlock];   /* 4 octaves down: tape microspeed */
float tape_l[kMaxBlock], tape_r[kMaxBlock];
float tmix_l[kMaxBlock], tmix_r[kMaxBlock];
float gr_l[kMaxBlock], gr_r[kMaxBlock];
float fe_l[kMaxBlock], fe_r[kMaxBlock];
float st_l[kMaxBlock], st_r[kMaxBlock];

/* Block-rate smoothing of the mix gains (zipper-free knob moves). */
struct Smooth
{
    float y = 0.f;
    float Step(float target) { y += 0.08f * (target - y); return y; }
};
Smooth s_dry, s_wet_t, s_wet_g, s_src, s_feast, s_level, s_in, s_out;

/* Slower glide for filter knobs: the panel updates every ~16 ms, so the
 * cutoff and spacing are smoothed over ~20 ms to remove zipper steps. */
struct Glide
{
    float y = 0.5f;
    bool  primed = false;
    float Step(float target)
    {
        if (!primed) { y = target; primed = true; }
        y += 0.025f * (target - y);
        return y;
    }
};
Glide s_cutoff, s_spacing;

EnvFollow out_env;
float     sub_last = 0.f;
bool      gate_last = false;

/* CPU governor: if the audio callback averages above 85 % of its budget,
 * lower the number of simultaneous grains (never below 4) so audio never
 * drops out; give voices back once the load falls under 65 %.  The grain
 * rhythm (triggers per clock) is unchanged — only overlap is limited. */
uint32_t  voice_cap   = Grains::kMaxGrains;

/* J2 normalling: J2 has no switch contact, so "unpatched" is detected
 * as silence.  When J2 stays below −60 dBFS for 10 s while J1 carries
 * signal, J1 is copied to J2 (mono → stereo); real signal on J2 above
 * −50 dBFS hands it back.  10 ms crossfade either way. */
float     r_env      = 0.f, l_env = 0.f;
uint32_t  r_quiet    = 0;
bool      mono_in    = false;
float     mono_mix   = 0.f;
constexpr float    kMonoOn   = 0.001f;    /* −60 dBFS */
constexpr float    kMonoOff  = 0.0032f;   /* −50 dBFS */
constexpr uint32_t kMonoHold = 20000u;    /* blocks: 10 s of silence on J2 */

/* Per-stage profile: summed ticks over the 1 s window. */
float     prof_tape = 0.f, prof_grain = 0.f, prof_stack = 0.f, prof_total = 0.f;

/* Rolling-window CPU peak. */
float     ticks_to_load = 0.f;
float     win_peak      = 0.f;
uint32_t  win_blocks    = 0;
uint32_t  gov_holdoff = 0;
float     fs = 48000.f;

constexpr float kFeastGain = 0.9f;

} // namespace

void EngineInit(float sample_rate, size_t block_size)
{
    fs = sample_rate;
    tape.Init(tape_buf_l, tape_buf_r, fs);
    grains.Init(grain_buf, fs);
    stack.Init(fs);
    out_env.Init(0.002f, 0.15f, fs);
    cpu.Init(fs, block_size);
    ticks_to_load = sample_rate
                  / (static_cast<float>(daisy::System::GetTickFreq())
                     * static_cast<float>(block_size));
    s_in.y = 1.f;
    s_out.y = 1.f;
    s_dry.y = 1.f;
}

float EngineSubValue() { return sub_last; }
bool  EngineTrigGate() { return gate_last; }

void EngineProcess(const float* const* in, float** out, size_t n)
{
    cpu.OnBlockStart();
    const uint32_t t_start = daisy::System::GetTick();
    if (n > kMaxBlock) n = kMaxBlock;

    const Params& p = g_params;

    /* ── Mix law (spec "Mixing") ───────────────────────────────────── */
    /* Each Mix knob: first 20 % = layer level (fully dry), then the
     * original dry → wet law over the remaining 80 %. */
    const MixZone z_t = SplitMixKnob(p.tape_mix);
    const MixZone z_g = SplitMixKnob(p.grain_mix);

    const float a_t   = z_t.mix;
    const float dry_t = Cos01(0.25f * a_t);
    const float wet_t = Sin01(0.25f * a_t);

    const float a_g = z_g.mix;
    float dry_g, wet_g, src;
    if (a_g < 0.5f)
    {
        const float y = 2.f * a_g;
        dry_g = 1.f - 0.5f * y;
        wet_g = 0.5f * y;
        src   = 0.f;
    }
    else
    {
        const float y = 2.f * a_g - 1.f;
        dry_g = 0.5f - 0.25f * y;
        wet_g = 0.5f + 0.25f * y;
        src   = y;
    }

    /* Dry = the lower of the two layers' dry amounts (as before), times
     * the louder layer's level: one layer turned off no longer pulls the
     * dry down, and with both off the dry is gone too.  Above 20 % on
     * both knobs this is exactly the original law. */
    const float lvl_max = z_t.level > z_g.level ? z_t.level : z_g.level;
    const float dry   = s_dry.Step((dry_t < dry_g ? dry_t : dry_g) * lvl_max);
    const float wt    = s_wet_t.Step(wet_t);
    const float wg    = s_wet_g.Step(wet_g);
    const float srcm  = s_src.Step(src);
    const float feast = s_feast.Step(Clamp(p.stack_feast, 0.f, 1.f));
    const float level = s_level.Step(p.stack_mute ? 0.f : Clamp(p.stack_level, 0.f, 1.f));
    const float ing   = s_in.Step(p.in_gain);
    const float outg  = s_out.Step(p.out_level);

    /* ── Pitch ─────────────────────────────────────────────────────── */
    /* Stack notes first: the newest note sets the sub-sine pitch. */
    StackControls sc;
    sc.tune_oct   = p.stack_tune_oct;
    sc.detune     = p.stack_detune;
    sc.sine_saw   = p.sine_saw;
    sc.octaves    = p.octaves;
    sc.level      = level;
    sc.cutoff     = s_cutoff.Step(p.stack_cutoff);
    sc.spacing    = s_spacing.Step(p.stack_spacing);
    sc.sub_depth  = p.sub_depth;
    sc.attack     = Clamp(p.stack_attack + p.env_cv, 0.f, 1.f);
    sc.release    = Clamp(p.stack_release + p.env_cv, 0.f, 1.f);
    sc.voices     = p.stack_voices;
    sc.gate       = p.gate != 0u;
    sc.gated_mode = p.gated_mode != 0u;
    sc.note_seq   = p.note_seq;
    sc.note_volts = p.note_volts;
    sc.voct_live  = p.voct_volts;
    stack.BeginBlock(sc, n);
    const float pitch_oct = stack.LeadOct();
    const float f0 = stack.LeadHz();

    /* ── Input (with J1 → J2 normalling) ───────────────────────────── */
    {
        float pl = 0.f, pr = 0.f;
        for (size_t i = 0; i < n; i++)
        {
            pl = fmaxf(pl, fabsf(in[0][i]));
            pr = fmaxf(pr, fabsf(in[1][i]));
        }
        l_env = fmaxf(pl, l_env * 0.98f);   /* ~25 ms release */
        r_env = fmaxf(pr, r_env * 0.98f);
        if (r_env < kMonoOn) { if (r_quiet < kMonoHold) r_quiet++; }
        else                   r_quiet = 0;
        if (!mono_in && r_quiet >= kMonoHold && l_env > kMonoOn) mono_in = true;
        if (mono_in && r_env > kMonoOff)                         mono_in = false;
    }
    const float mono_target = mono_in ? 1.f : 0.f;
    const float mono_step   = static_cast<float>(n) / (0.01f * fs);
    float peak = 0.f;
    for (size_t i = 0; i < n; i++)
    {
        if (mono_mix < mono_target)      mono_mix = fminf(mono_target, mono_mix + mono_step / static_cast<float>(n));
        else if (mono_mix > mono_target) mono_mix = fmaxf(mono_target, mono_mix - mono_step / static_cast<float>(n));
        in_l[i] = in[0][i] * ing;
        in_r[i] = Lerp(in[1][i], in[0][i], mono_mix) * ing;
        const float a = fabsf(in[0][i]) > fabsf(in[1][i]) ? fabsf(in[0][i]) : fabsf(in[1][i]);
        if (a > peak) peak = a;
    }

    /* ── 1. Sub sine ───────────────────────────────────────────────── */
    stack.ComputeSub(sub, n);
    stack.ComputeDeepSub(deep_sub, n);

    /* ── 2. Tape ───────────────────────────────────────────────────── */
    TapeControls tc;
    tc.delay_s  = p.clock_period_s * p.tape_ratio;
    tc.feedback = p.tape_feedback;
    tc.tone     = p.tape_tone;
    tc.wow      = p.tape_wow;
    {
        /* Microspeed: CCW half = V/Oct depth, CW half = sub depth. */
        const float m = Clamp(p.tape_micro, 0.f, 1.f) - 0.5f;
        const float dv = m < 0.f ? (-2.f * m) : 0.f;
        const float ds = m > 0.f ? (2.f * m) : 0.f;
        /* CCW: up to 1/3 V/oct (4 st per volt), cubic taper for fine
         * control near the centre. */
        tc.voct_shift_st = Clamp(dv * dv * dv * 4.f * p.voct_volts, -12.f, 12.f);
        tc.sub_dev_st    = ds * ds * 1.0f;
    }
    tc.sub_hz   = 0.0625f * f0;     /* microspeed sub: 4 octaves below the note */
    tc.freeze   = p.tape_freeze != 0u;
    tc.pingpong = p.tape_pingpong != 0u;
    { const uint32_t t0 = daisy::System::GetTick();
      tape.Process(in_l, in_r, tape_l, tape_r, deep_sub, n, tc);
      prof_tape += static_cast<float>(daisy::System::GetTick() - t0); }

    for (size_t i = 0; i < n; i++)
    {
        tmix_l[i] = dry_t * in_l[i] + wet_t * tape_l[i];
        tmix_r[i] = dry_t * in_r[i] + wet_t * tape_r[i];
    }

    /* ── 3. Grains ─────────────────────────────────────────────────── */
    GrainControls gc;
    gc.period_s   = p.clock_period_s * p.grain_ratio;
    gc.size_s     = p.grain_size_s;
    gc.count      = p.grain_count < 1u ? 1u
                  : (p.grain_count > Grains::kMaxGrains ? Grains::kMaxGrains : p.grain_count);
    gc.count_knob = gc.count;
    gc.per_clock  = TriggersPerClock(gc.count);
    if (gc.count > voice_cap) gc.count = voice_cap;   /* CPU governor */
    gc.shape      = p.grain_shape;
    gc.speed      = p.grain_speed;
    gc.spray      = p.grain_spray;
    gc.pitch_st   = p.grain_pitch_st;
    gc.pitch_rand = p.grain_pitch_rand;
    gc.dir        = p.grain_dir;
    gc.spread     = p.spread;
    gc.freeze     = p.grain_freeze != 0u;
    gc.source_mix = srcm;
    gc.octaves    = p.octaves;
    gc.feast_active = feast > 0.002f;
    gc.render       = wg > 0.001f || feast > 0.002f;
    { const uint32_t t0 = daisy::System::GetTick();
      grains.Process(in_l, in_r, tmix_l, tmix_r, gr_l, gr_r, fe_l, fe_r, n, gc);
      prof_grain += static_cast<float>(daisy::System::GetTick() - t0); }

    for (size_t i = 0; i < n; i++)
    {
        /* Grains → Filter: independent of the grain mix, so it is heard
         * even with the grain layer turned down. */
        fe_l[i] *= kFeastGain * feast;
        fe_r[i] *= kFeastGain * feast;
    }

    /* ── 4. Stack + notch ──────────────────────────────────────────── */
    { const uint32_t t0 = daisy::System::GetTick();
      stack.Process(fe_l, fe_r, sub, st_l, st_r, n, sc);
      prof_stack += static_cast<float>(daisy::System::GetTick() - t0); }

    /* ── 5. Mix ────────────────────────────────────────────────────── */
    const float g_main = (1.f - feast) * wg;
    for (size_t i = 0; i < n; i++)
    {
        float l = dry * in_l[i] + wt * tape_l[i] + g_main * gr_l[i] + st_l[i];
        float r = dry * in_r[i] + wt * tape_r[i] + g_main * gr_r[i] + st_r[i];
        l = SoftClip(l * outg);
        r = SoftClip(r * outg);
        out[0][i] = l;
        out[1][i] = r;
        out_env.Process(l > r ? l : r);
    }

    sub_last  = sub[n - 1];
    gate_last = grains.TrigGate();

    /* ── Telemetry ─────────────────────────────────────────────────── */
    g_tel.tape_env       = tape.Env();
    g_tel.tape_phase     = tape.PeriodPhase();
    g_tel.tape_taps      = tape.Taps();
    g_tel.tape_wow       = tape.WowNow();
    g_tel.tape_speed_st  = tape.SpeedSt();
    if (feast > 0.002f && grains.Triggers() != g_tel.grain_triggers)
        g_tel.feast_triggers += grains.Triggers() - g_tel.grain_triggers;
    g_tel.grain_triggers = grains.Triggers();
    g_tel.grain_ticks    = grains.Ticks();
    g_tel.grain_progress = grains.Progress();
    g_tel.grain_env      = grains.EnvNow();
    g_tel.grain_scan     = grains.ScanPhase();
    g_tel.stack_env      = stack.Env();
    g_tel.stack_pitch_oct = pitch_oct;
    g_tel.notch_lo01     = stack.NotchLo01();
    g_tel.notch_hi01     = stack.NotchHi01();
    g_tel.sub_phase      = stack.SubPhase();
    g_tel.voices_sounding = stack.Sounding();
    for (uint32_t v = 0; v < kMaxStackVoices; v++) g_tel.voice_env[v] = stack.VoiceEnv(v);
    g_tel.sub_value      = sub_last;
    g_tel.out_env        = out_env.y;
    g_tel.in_peak        = peak > g_tel.in_peak ? peak : g_tel.in_peak * 0.995f;

    cpu.OnBlockEnd();
    g_tel.mono_in  = mono_in;
    g_tel.cpu_load = cpu.GetAvgCpuLoad();
    g_tel.cpu_peak = cpu.GetMaxCpuLoad();

    /* This block's own load: rolling 1 s peak + overrun count.  An
     * overrun (> 100 %) is a block that missed its deadline — a glitch. */
    float block_load = 0.f;
    {
        const float load = static_cast<float>(daisy::System::GetTick() - t_start)
                         * ticks_to_load;
        block_load = load;
        prof_total += load;
        if (load > win_peak) win_peak = load;
        if (load > 1.f) g_tel.overruns++;
        if (++win_blocks >= 2000u)          /* 1 s at 24-sample blocks */
        {
            g_tel.cpu_peak_1s = win_peak;
            /* Average share of the block budget per stage over the second. */
            const float k = ticks_to_load / static_cast<float>(win_blocks);
            g_tel.prof_tape  = prof_tape * k;
            g_tel.prof_grain = prof_grain * k;
            g_tel.prof_stack = prof_stack * k;
            g_tel.prof_other = prof_total / static_cast<float>(win_blocks)
                             - g_tel.prof_tape - g_tel.prof_grain - g_tel.prof_stack;
            prof_tape = prof_grain = prof_stack = prof_total = 0.f;
            win_peak   = 0.f;
            win_blocks = 0;
        }
    }

    /* Emergency: one block close to its deadline cuts a quarter of the
     * grains at once; a high average trims gently; recovery is slow. */
    if (block_load > 0.88f && voice_cap > 2u)
    {
        voice_cap = voice_cap * 3u / 4u;
        if (voice_cap < 2u) voice_cap = 2u;
        gov_holdoff = 400u;
    }
    else if (gov_holdoff) gov_holdoff--;
    else if (g_tel.cpu_load > 0.68f && voice_cap > 2u)
    {
        voice_cap = voice_cap > 4u ? voice_cap - 2u : 2u;
        gov_holdoff = 100u;     /* 50 ms for the average to respond */
    }
    else if (g_tel.cpu_load < 0.52f && voice_cap < Grains::kMaxGrains)
    {
        voice_cap++;
        gov_holdoff = 400u;     /* recover slowly */
    }
    g_tel.voice_cap = voice_cap;
}

} // namespace sim
