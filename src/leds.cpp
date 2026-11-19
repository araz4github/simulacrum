/**
 * leds.cpp — ring overdraws, button LEDs, start-up sweep (spec "LEDs").
 *
 * Rule kept throughout: the SDK's declarative base ring shows the real
 * value (incl. CV); these overlays only add light driven by engine
 * telemetry or re-tint the fill.  All colour reaches the LEDs through
 * RingFrame::Emit / ScaleGlobal, so the Settings brightness applies.
 */

#include "leds.h"

#include <cmath>

#include "daisy_seed.h"
#include "alchemy/led/anims/color_morph_arc.h"
#include "alchemy/led/ring_frame.h"

#include "dsp_util.h"
#include "palette.h"
#include "sim_shared.h"

namespace sim {

UiState g_ui;

using alchemy::ArcGeometry;
using alchemy::FillDesc;
using alchemy::LedPanel;
using alchemy::PipCompose;
using alchemy::PipDesc;
using alchemy::PipMotion;
using alchemy::PipTrailSide;
using alchemy::Region;
using alchemy::RingFrame;
using alchemy::SelectorDesc;
using Rgb = LedPanel::Rgb;

namespace {

/* ── Small helpers ───────────────────────────────────────────────────── */

float Decay(uint32_t t_ms, uint32_t t0, float tau_ms)
{
    if (t0 == 0u || t_ms < t0) return 0.f;
    return expf(-static_cast<float>(t_ms - t0) / tau_ms);
}

/** A flash triggered whenever a telemetry counter moves. */
struct Ping
{
    uint32_t last  = 0;
    uint32_t t0    = 0;
    float Gain(uint32_t counter, uint32_t t_ms, float tau_ms)
    {
        if (counter != last) { last = counter; t0 = t_ms; }
        return Decay(t_ms, t0, tau_ms);
    }
};

/** Up to 8 decaying sparks, one spawned per telemetry event. */
struct EventSparks
{
    static constexpr int kN = 8;
    float    pos[kN] = {};
    float    lvl[kN] = {};
    uint32_t last    = 0;
    uint32_t last_t  = 0;
    Rng      rng;

    void Update(uint32_t counter, uint32_t t_ms)
    {
        const float dt = last_t ? static_cast<float>(t_ms - last_t) : 0.f;
        last_t = t_ms;
        const float k = expf(-dt / 110.f);
        for (float& l : lvl) l *= k;

        uint32_t fresh = counter - last;
        last = counter;
        if (fresh > static_cast<uint32_t>(kN)) fresh = kN;
        while (fresh--)
        {
            int slot = 0;
            for (int i = 1; i < kN; i++) if (lvl[i] < lvl[slot]) slot = i;
            pos[slot] = rng.Uni();
            lvl[slot] = 0.6f + 0.4f * rng.Uni();
        }
    }

    void Draw(RingFrame& f, Region r, Rgb c) const
    {
        PipDesc d;
        d.color   = c;
        d.compose = PipCompose::Add;
        for (int i = 0; i < kN; i++)
            if (lvl[i] > 0.02f) f.Pip(r, d, pos[i], lvl[i]);
    }
};

/** Re-draw an Edge fill in @p c, scaled by @p k (overlay frame). */
void Fill(RingFrame& f, Rgb c, float norm, float k)
{
    FillDesc fd;
    fd.color = pal::Dim(c, Clamp(k, 0.f, 1.f));
    f.Base(fd, norm);
}

PipDesc AddPip(Rgb c, bool smooth = true)
{
    PipDesc d;
    d.color   = c;
    d.compose = PipCompose::Add;
    d.smooth  = smooth;
    return d;
}

/** Linear colour interpolation across morph snaps (for re-tinting). */
Rgb MorphColor(const alchemy::MorphSnapPoint* s, int n, float x)
{
    if (x <= s[0].position) return s[0].color;
    for (int i = 1; i < n; i++)
        if (x <= s[i].position)
        {
            const float t = (x - s[i - 1].position) / (s[i].position - s[i - 1].position);
            return LedPanel::Mix(s[i - 1].color, s[i].color, t);
        }
    return s[n - 1].color;
}

float Tri(float phase) { return 1.f - fabsf(2.f * Wrap01(phase) - 1.f); }

/* Octave landmarks for pitch rings (both span four octaves). */
void OctaveMarks(RingFrame& f)
{
    PipDesc mark = AddPip(pal::kWhite, false);
    for (int k = 0; k <= 4; k++) f.Pip(Region::Full, mark, 0.25f * static_cast<float>(k), 0.18f);
}

/** Inside the centre detent: a 3-LED white bar at 12 o'clock. */
void CenterBar(RingFrame& f, float norm)
{
    if (!InDetent(norm)) return;
    PipDesc d;
    d.color = pal::kWhite;
    d.width = 3;
    f.Pip(Region::Full, d, 0.5f, 1.f);
}

} // namespace

void OdCenter(LedPanel& p, uint8_t pot, const ArcGeometry& geo, float norm, uint32_t, void*)
{
    RingFrame f;
    f.BeginOverlay(geo);
    CenterBar(f, norm);
    f.Emit(p, pot);
}

/* Shared by the grain-envelope ring (base declared in simulacrum.cpp). */
extern const alchemy::MorphSnapPoint kEnvSnaps[4];

/* ════════════════════════════ Tape ════════════════════════════════════ */

void OdTapeMix(LedPanel& p, uint8_t pot, const ArcGeometry& geo, float norm, uint32_t, void*)
{
    /* Level zone (first 20 %): a warm-white fill growing in brightness
     * with the layer level (dry only).  Above it: amber, breathing with
     * the level of the repeats. */
    RingFrame f;
    f.BeginOverlay(geo);
    const MixZone z = SplitMixKnob(norm);
    if (z.mix <= 0.f)
    {
        Fill(f, pal::kWarmWhite, norm, 0.15f + 0.6f * z.level);
        f.Emit(p, pot);
        return;
    }
    const float env = Clamp(g_tel.tape_env * 4.f, 0.f, 1.f);
    Fill(f, pal::kAmber, norm, 0.35f + 0.65f * env);
    f.Emit(p, pot);
}

void OdTapeClock(LedPanel& p, uint8_t pot, const ArcGeometry& geo, float, uint32_t t, void*)
{
    static Ping tap;
    RingFrame f;
    f.BeginOverlay(geo);
    /* Ping on every delay tap, on the selected zone. */
    const float g = tap.Gain(g_tel.tape_taps, t, 140.f);
    if (g > 0.02f) f.Pip(Region::Active, AddPip(pal::kWhite), 0.5f, 0.8f * g);
    /* Bottom pip: incoming clock (dimmer on the internal tempo). */
    const float b = Decay(t, g_ui.last_beat_ms, 90.f) * (g_ui.ext_clock ? 1.f : 0.35f);
    if (b > 0.02f) f.Pip(Region::BottomPip, AddPip(pal::kWhite, false), 0.f, b);
    f.Emit(p, pot);
}

void OdTapeFeedback(LedPanel& p, uint8_t pot, const ArcGeometry& geo, float norm, uint32_t, void*)
{
    RingFrame f;
    f.BeginOverlay(geo);
    /* Amber turning red past 100 % (100/105 of travel). */
    constexpr float kUnity = 100.f / 105.f;
    if (norm > kUnity)
        Fill(f, LedPanel::Mix(pal::kAmber, pal::kRed, (norm - kUnity) / (1.f - kUnity)), norm, 1.f);
    else
        Fill(f, pal::kAmber, norm, 1.f);
    /* A fading light travels the fill once per repeat. */
    PipDesc d = AddPip(pal::kWhite);
    d.trail_steps   = 3;
    d.trail_side    = PipTrailSide::Ccw;
    d.trail_falloff = 0.5f;
    const float env = Clamp(g_tel.tape_env * 4.f, 0.f, 1.f);
    if (env > 0.02f) f.Pip(Region::Active, d, g_tel.tape_phase, 0.7f * env);
    f.Emit(p, pot);
}

void OdTapeWow(LedPanel& p, uint8_t pot, const ArcGeometry& geo, float norm, uint32_t t, void*)
{
    /* Drifting ripple whose phase is the real wow/flutter movement. */
    RingFrame f;
    f.BeginOverlay(geo);
    Fill(f, pal::kAmber, norm, 1.f);
    f.Field(Region::Active, alchemy::FieldRipple(2000u, 6.f), 0.6f * norm, t,
            Wrap01(0.5f + 0.5f * g_tel.tape_wow));
    f.Emit(p, pot);
}

void OdTapeMicro(LedPanel& p, uint8_t pot, const ArcGeometry& geo, float norm, uint32_t, void*)
{
    /* Dot at the live tape speed offset (±1 octave across the ring). */
    RingFrame f;
    f.BeginOverlay(geo);
    CenterBar(f, norm);
    const float pos = Clamp(0.5f + g_tel.tape_speed_st / 24.f, 0.f, 1.f);
    if (fabsf(g_tel.tape_speed_st) > 0.02f)
        f.Pip(Region::Full, AddPip(pal::kWhite), pos, 0.9f);
    f.Emit(p, pot);
}

/* ════════════════════════════ Grain ═══════════════════════════════════ */

void OdGrainClock(LedPanel& p, uint8_t pot, const ArcGeometry& geo, float, uint32_t t, void*)
{
    static Ping tick;
    RingFrame f;
    f.BeginOverlay(geo);
    const float g = tick.Gain(g_tel.grain_ticks, t, 140.f);
    if (g > 0.02f) f.Pip(Region::Active, AddPip(pal::kWhite), 0.5f, 0.8f * g);
    f.Emit(p, pot);
}

void OdGrainSize(LedPanel& p, uint8_t pot, const ArcGeometry& geo, float, uint32_t, void*)
{
    /* Dot sweeping through the newest grain's length. */
    RingFrame f;
    f.BeginOverlay(geo);
    if (g_tel.grain_env > 0.02f)
        f.Pip(Region::Active, AddPip(pal::kWhite), g_tel.grain_progress,
              0.3f + 0.7f * g_tel.grain_env);
    f.Emit(p, pot);
}

void OdGrainCount(LedPanel& p, uint8_t pot, const ArcGeometry& geo, float, uint32_t t, void*)
{
    static EventSparks sparks;
    sparks.Update(g_tel.grain_triggers, t);
    RingFrame f;
    f.BeginOverlay(geo);
    sparks.Draw(f, Region::Full, pal::kWhite);
    f.Emit(p, pot);
}

void OdGrainEnv(LedPanel& p, uint8_t pot, const ArcGeometry& geo, float norm, uint32_t, void*)
{
    /* Gradient fill whose brightness follows the newest grain envelope. */
    RingFrame f;
    f.BeginOverlay(geo);
    Fill(f, MorphColor(kEnvSnaps, 4, norm), norm, 0.3f + 0.7f * g_tel.grain_env);
    f.Emit(p, pot);
}

void OdGrainSpeed(LedPanel& p, uint8_t pot, const ArcGeometry& geo, float norm, uint32_t, void*)
{
    RingFrame f;
    f.BeginOverlay(geo);
    /* Dot lapping the ring at the real scan speed. */
    PipDesc d = AddPip(pal::kWhite);
    d.motion = PipMotion::Wrap;
    f.Pip(Region::Full, d, g_tel.grain_scan, 0.45f);
    CenterBar(f, norm);
    /* Bottom pip white at 1×. */
    if (InDetent(norm))
        f.Pip(Region::BottomPip, AddPip(pal::kWhite, false), 0.f, 1.f);
    f.Emit(p, pot);
}

/* ════════════════════════════ Stack ═══════════════════════════════════ */

void OdStackTune(LedPanel& p, uint8_t pot, const ArcGeometry& geo, float, uint32_t, void*)
{
    RingFrame f;
    f.BeginOverlay(geo);
    OctaveMarks(f);
    /* Real pitch incl. V/Oct: C2..C6 across the ring. */
    const float pos = Clamp((g_tel.stack_pitch_oct + 2.f) / 4.f, 0.f, 1.f);
    f.Pip(Region::Full, AddPip(pal::kWhite), pos, 0.9f);
    f.Emit(p, pot);
}

void OdStackDetune(LedPanel& p, uint8_t pot, const ArcGeometry& geo, float norm, uint32_t t, void*)
{
    /* Shimmer speeds up with detune (the beating). */
    RingFrame f;
    f.BeginOverlay(geo);
    Fill(f, pal::kViolet, norm, 1.f);
    const uint16_t bucket = static_cast<uint16_t>(220.f - 170.f * norm);
    f.Field(Region::Active, alchemy::FieldShimmer(bucket), 0.5f * norm, t);
    f.Emit(p, pot);
}

void OdStackLevel(LedPanel& p, uint8_t pot, const ArcGeometry& geo, float norm, uint32_t, void*)
{
    RingFrame f;
    f.BeginOverlay(geo);
    const float env = Clamp(g_tel.stack_env * 3.f, 0.f, 1.f);
    Fill(f, pal::kViolet, norm, 0.35f + 0.65f * env);
    f.Emit(p, pot);
}

void OdStackSpacing(LedPanel& p, uint8_t pot, const ArcGeometry& geo, float, uint32_t, void*)
{
    /* Two dots at the live notch frequencies (move with the sub-sine). */
    RingFrame f;
    f.BeginOverlay(geo);
    if (g_tel.notch_lo01 < 0.999f)
    {
        f.Pip(Region::Full, AddPip(pal::kWhite), g_tel.notch_lo01, 0.9f);
        f.Pip(Region::Full, AddPip(pal::kPink), g_tel.notch_hi01, 0.9f);
    }
    f.Emit(p, pot);
}

void OdStackFeast(LedPanel& p, uint8_t pot, const ArcGeometry& geo, float, uint32_t t, void*)
{
    static EventSparks sparks;
    sparks.Update(g_tel.feast_triggers, t);
    RingFrame f;
    f.BeginOverlay(geo);
    sparks.Draw(f, Region::Active, pal::kWhite);
    f.Emit(p, pot);
}

void OdStackEnvelope(LedPanel& p, uint8_t pot, const ArcGeometry& geo, float norm, uint32_t, void*)
{
    /* Attack / Release fills glow with the loudest sounding voice. */
    float e = 0.f;
    for (uint32_t v = 0; v < kMaxStackVoices; v++) e = fmaxf(e, g_tel.voice_env[v]);
    RingFrame f;
    f.BeginOverlay(geo);
    Fill(f, pal::kViolet, norm, 0.35f + 0.65f * e);
    f.Emit(p, pot);
}

void OdVoices(LedPanel& p, uint8_t pot, const ArcGeometry& geo, float, uint32_t, void*)
{
    /* One white dot per sounding voice, brightness = its envelope. */
    RingFrame f;
    f.BeginOverlay(geo);
    PipDesc d = AddPip(pal::kWhite, false);
    for (uint32_t v = 0; v < kMaxStackVoices; v++)
        if (g_tel.voice_env[v] > 0.02f)
            f.Pip(Region::Full, d, (static_cast<float>(v) + 0.5f) / kMaxStackVoices,
                  0.3f + 0.7f * g_tel.voice_env[v]);
    f.Emit(p, pot);
}

/* ═════════════════════════ B2 hidden (global) ═════════════════════════ */

void OdGain(LedPanel& p, uint8_t pot, const ArcGeometry& geo, float norm, uint32_t t, void*)
{
    static uint32_t clip_t = 0;
    if (g_tel.in_peak > 0.98f) clip_t = t;
    RingFrame f;
    f.BeginOverlay(geo);
    const float c = Decay(t, clip_t, 250.f);
    if (c > 0.02f) Fill(f, LedPanel::Mix(pal::kWarmWhite, pal::kRed, c), norm, 1.f);
    f.Emit(p, pot);
}

void OdTempo(LedPanel& p, uint8_t pot, const ArcGeometry& geo, float norm, uint32_t t, void*)
{
    RingFrame f;
    f.BeginOverlay(geo);
    /* The fill shows the tempo in use (the knob follows an external
     * clock 1:1) and blinks on every beat: bright white when the beat
     * comes from J3, warm white on the internal tempo. */
    const float b = Decay(t, g_ui.last_beat_ms, 120.f);
    const Rgb   c = g_ui.ext_clock ? LedPanel::Mix(pal::kWarmWhite, pal::kWhite, b)
                                   : pal::kWarmWhite;
    Fill(f, c, norm, 0.3f + 0.7f * b);
    if (b > 0.02f) f.Pip(Region::BottomPip, AddPip(pal::kWhite, false), 0.f, b);
    f.Emit(p, pot);
}

void OdSpread(LedPanel& p, uint8_t pot, const ArcGeometry& geo, float, uint32_t, void*)
{
    /* Dot bouncing end to end once per delay period. */
    RingFrame f;
    f.BeginOverlay(geo);
    PipDesc d = AddPip(pal::kWhite);
    d.motion = PipMotion::Bounce;
    f.Pip(Region::Active, d, 2.f * g_tel.tape_phase, 0.7f);
    f.Emit(p, pot);
}

void OdSpray(LedPanel& p, uint8_t pot, const ArcGeometry& geo, float norm, uint32_t t, void*)
{
    /* Sparkle density = spray amount. */
    static EventSparks sparks;
    static float acc = 0.f;
    static uint32_t count = 0, last_t = 0;
    const float dt = last_t ? static_cast<float>(t - last_t) : 0.f;
    last_t = t;
    acc += dt * 0.001f * 14.f * norm;
    while (acc >= 1.f) { acc -= 1.f; count++; }
    sparks.Update(count, t);
    RingFrame f;
    f.BeginOverlay(geo);
    sparks.Draw(f, Region::Active, pal::kWhite);
    f.Emit(p, pot);
}

void OdSubDepth(LedPanel& p, uint8_t pot, const ArcGeometry& geo, float norm, uint32_t, void*)
{
    /* Dot riding the sub-sine, scaled by the depth. */
    RingFrame f;
    f.BeginOverlay(geo);
    const float pos = Clamp(0.5f + 0.5f * norm * g_tel.sub_value, 0.f, 1.f);
    f.Pip(Region::Full, AddPip(pal::kViolet), pos, 0.9f);
    f.Emit(p, pot);
}

void OdOutput(LedPanel& p, uint8_t pot, const ArcGeometry& geo, float, uint32_t, void*)
{
    /* Green / yellow / red output meter riding on the level fill. */
    RingFrame f;
    f.BeginOverlay(geo);
    const float lvl = Clamp(g_tel.out_env, 0.f, 1.f);
    const Rgb c = lvl < 0.6f ? LedPanel::Mix(pal::kGreen, pal::kYellow, lvl / 0.6f)
                             : LedPanel::Mix(pal::kYellow, pal::kRed, (lvl - 0.6f) / 0.4f);
    PipDesc d = AddPip(c, false);
    d.trail_steps   = 12;
    d.trail_side    = PipTrailSide::Ccw;
    d.trail_falloff = 0.85f;
    if (lvl > 0.01f) f.Pip(Region::Full, d, lvl, 0.8f);
    f.Emit(p, pot);
}

/* ═════════════════════════ B3 hidden (lime) ═══════════════════════════ */

void OdGrainPitch(LedPanel& p, uint8_t pot, const ArcGeometry& geo, float norm, uint32_t, void*)
{
    RingFrame f;
    f.BeginOverlay(geo);
    OctaveMarks(f);
    CenterBar(f, norm);
    if (fabsf(g_ui.grain_pitch_st) < 0.5f)
        f.Pip(Region::BottomPip, AddPip(pal::kWhite, false), 0.f, 1.f);
    f.Emit(p, pot);
}

void OdPitchRand(LedPanel& p, uint8_t pot, const ArcGeometry& geo, float norm, uint32_t t, void* ctx)
{
    OdSpray(p, pot, geo, norm, t, ctx);   /* same sparkle-density idea */
}

void OdJackFn(LedPanel& p, uint8_t pot, const ArcGeometry& geo, float, uint32_t, void* ctx)
{
    /* Re-colour the selected option in its target's colour. */
    const uint32_t fn = *static_cast<const uint32_t*>(ctx);
    RingFrame f;
    f.BeginOverlay(geo);
    SelectorDesc d;
    d.num_zones      = kNumJackFns;
    d.active_color   = pal::kJackFnColors[fn < kNumJackFns ? fn : 0];
    d.inactive_color = pal::kWhite;
    d.inactive_dim   = 0.08f;
    f.BaseZone(d, static_cast<uint8_t>(fn));
    f.Emit(p, pot);
}

/* ═════════════════════════════ Buttons ════════════════════════════════ */

namespace {

Rgb PageColor(uint8_t page)
{
    switch (page)
    {
        case kPgGrain:  return pal::kCyan;
        case kPgStack:  return pal::kViolet;
        case kPgGlobal: return pal::kWarmWhite;
        case kPgGrainX: return pal::kGrainBlue;
        case kPgStackX: return pal::kGreen;
        default:        return pal::kAmber;
    }
}

void SetBtn(LedPanel& p, uint8_t b, Rgb top, Rgb bottom)
{
    p.SetButton(b, LedPanel::ButtonLed::Top,    p.ScaleGlobal(top));
    p.SetButton(b, LedPanel::ButtonLed::Bottom, p.ScaleGlobal(bottom));
}

} // namespace

void RenderButtons(LedPanel& p, uint32_t t)
{
    if (g_ui.settings_active) return;   /* Settings paints its own UI */

    const uint8_t main = g_ui.main_page;
    const Rgb     pc   = PageColor(main);

    /* B1: page colour; bottom pulses on each clock beat. */
    const float beat = Decay(t, g_ui.last_beat_ms, 120.f);
    SetBtn(p, 0, pc, pal::Dim(pc, 0.3f + 0.7f * beat));

    const Rgb   off_c = pal::Dim(pc, 0.25f);
    const Rgb   on_c  = pal::Dim(pal::kWhite, 0.45f + 0.55f * Tri(static_cast<float>(t) / 1500.f));

    /* B2: Tape / Grain freeze, Stack V/Oct → grains.  Bottom LED = the
     * *other* layer is frozen.  Warm white while B2's page is held. */
    if (g_ui.active_page == kPgGlobal)
    {
        SetBtn(p, 1, pal::kWarmWhite, pal::kWarmWhite);
    }
    else
    {
        uint32_t on = 0, other = 0;
        switch (main)
        {
            case kPgTape:  on = g_ui.tape_freeze;  other = g_ui.grain_freeze; break;
            case kPgGrain: on = g_ui.grain_freeze; other = g_ui.tape_freeze;  break;
            default:       on = g_ui.voct_grains;
                           other = g_ui.tape_freeze | g_ui.grain_freeze;       break;
        }
        SetBtn(p, 1, on ? on_c : off_c, other ? pal::Dim(pal::kWhite, 0.5f) : Rgb{0, 0, 0});
    }

    /* B3: mode per page; the hidden page's colour while it is held. */
    if (g_ui.active_page == kPgGrainX) { SetBtn(p, 2, pal::kGrainBlue, pal::kGrainBlue); return; }
    if (g_ui.active_page == kPgStackX) { SetBtn(p, 2, pal::kGreen, pal::kGreen); return; }
    switch (main)
    {
        case kPgTape:
            if (g_ui.tape_pingpong)
            {
                const bool swap = ((t / 250u) & 1u) != 0u;
                SetBtn(p, 2, swap ? pal::kWhite : pal::kAmber, swap ? pal::kAmber : pal::kWhite);
            }
            else
                SetBtn(p, 2, pal::kAmber, pal::kAmber);
            break;
        case kPgGrain:
            if (g_ui.grain_dir == 0u)      SetBtn(p, 2, pal::kCyan, pal::kCyan);
            else if (g_ui.grain_dir == 1u) SetBtn(p, 2, pal::kMagenta, pal::kMagenta);
            else
            {
                const uint32_t h = (t / 90u) * 2654435761u;
                SetBtn(p, 2, (h >> 28) & 1u ? pal::kCyan : pal::kMagenta,
                             (h >> 29) & 1u ? pal::kCyan : pal::kMagenta);
            }
            break;
        default:
            /* Stack mute: violet while playing, breathing white when muted. */
            SetBtn(p, 2, g_ui.stack_mute ? on_c : pc, g_ui.stack_mute ? on_c : pc);
            break;
    }
}

/* ═══════════════════════════ Start-up sweep ═══════════════════════════ */

void RunSplash(LedPanel& panel, const ArcGeometry& geo)
{
    /* Serpentine through the panel: P1 → P2 → P4 → P3 → P5 → P6. */
    constexpr uint8_t kSnake[6] = {0, 1, 3, 2, 4, 5};
    constexpr uint32_t kTravelMs = 1500u;   /* comet across all six rings */
    constexpr uint32_t kFadeMs   = 600u;
    constexpr float    kEcho1    = 0.45f;   /* ring-lengths behind the head */
    constexpr float    kEcho2    = 0.90f;

    /* Row colours for the closing flash: tape / grain / stack. */
    const Rgb row[6] = {pal::kAmber, pal::kAmber, pal::kCyan,
                        pal::kCyan,  pal::kViolet, pal::kViolet};

    const uint32_t start = daisy::System::GetNow();
    for (;;)
    {
        const uint32_t t = daisy::System::GetNow() - start;
        if (t > kTravelMs + kFadeMs) break;

        panel.Clear();
        const float u = 6.f * static_cast<float>(t) / static_cast<float>(kTravelMs);

        RingFrame frames[6];
        for (auto& f : frames) f.Begin(geo);

        auto head = [&](float pos, Rgb c, float gain) {
            if (pos < 0.f || pos >= 6.f) return;
            const int   ring = static_cast<int>(pos);
            const float frac = pos - static_cast<float>(ring);
            PipDesc d;
            d.color         = c;
            d.compose       = PipCompose::Add;
            d.smooth        = true;
            d.trail_steps   = 4;
            d.trail_side    = PipTrailSide::Ccw;
            d.trail_falloff = 0.55f;
            frames[ring].Pip(Region::Full, d, frac, gain);
        };

        if (t <= kTravelMs)
        {
            head(u,          pal::kViolet, 1.f);
            head(u - kEcho1, pal::kAmber,  0.6f);
            head(u - kEcho2, pal::kCyan,   0.35f);
        }
        else
        {
            /* Every ring flashes its page colour, then fades to the UI. */
            const float k = 1.f - static_cast<float>(t - kTravelMs) / static_cast<float>(kFadeMs);
            FillDesc fd;
            for (int r = 0; r < 6; r++)
            {
                fd.color = pal::Dim(row[r], k * k);
                frames[r].Base(fd, 1.f);
            }
        }
        for (int i = 0; i < 6; i++) frames[i].Emit(panel, kSnake[i]);

        /* Buttons light as the comet passes their row. */
        const Rgb bc[3] = {pal::kAmber, pal::kCyan, pal::kViolet};
        for (uint8_t b = 0; b < 3; b++)
        {
            const float pass = 1.5f + 2.f * static_cast<float>(b);
            float g = u >= pass ? expf(-(u - pass) * 1.2f) : 0.f;
            if (t > kTravelMs) g = 0.f;
            panel.SetButtonPair(b, panel.ScaleGlobal(pal::Dim(bc[b], g)));
        }

        panel.Show();
        daisy::System::Delay(16);
    }
    panel.Clear();
    panel.Show();
}

} // namespace sim
