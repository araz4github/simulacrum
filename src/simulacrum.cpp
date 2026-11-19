/**
 * simulacrum.cpp — Simulacrum for Hermetic Modular Alchemy Lab (V2).
 *
 * Dark clocked tape delay + 12-bit granular delay + quadrature-sine /
 * supersaw stack through a dual notch filter, all running at once.
 * The full design lives in SPEC.md; this file is the control surface:
 *
 *   Pages (B1 cycles):  Tape (amber) → Grain (cyan) → Stack (violet)
 *   Hidden pages:       B2 held → Global (warm white)
 *                       B3 held on Grain → Grain Parameters (blue)
 *                       B3 held on Stack → Stack Parameters (green)
 *   Buttons (tap):      Tape  B2 freeze · B3 repeat mode
 *                       Grain B2 freeze · B3 direction
 *                       Stack B2 V/Oct → grains · B3 mute
 *   Jacks:              J3 clock · J4 V/Oct · J5 grain size · J6 gate
 *                       J7 grain spray · J8 assignable (Global P6)
 *   Settings:           hold B2 + B3 for 2 s (brightness, presets; page 2
 *                       = internal Tempo)
 */

#include <cmath>

#include "daisy_seed.h"
#include "alchemy/control/cv_edge.h"
#include "alchemy/hw/alchemy_lab.h"
#include "alchemy/host_link/diagnostics.h"
#include "alchemy/host_link/host.h"
#include "alchemy/led/anims/color_morph_arc.h"
#include "alchemy/surface/button_bank.h"
#include "alchemy/surface/control_loop.h"
#include "alchemy/surface/cv_matrix.h"
#include "alchemy/surface/jack.h"
#include "alchemy/surface/page.h"
#include "alchemy/surface/pager.h"
#include "alchemy/surface/param_lock.h"
#include "alchemy/surface/presets.h"
#include "alchemy/surface/settings.h"
#include "alchemy/surface/virtual_button.h"
#include "alchemy/surface/virtual_knob.h"

#include "dsp_util.h"
#include "engine.h"
#include "leds.h"
#include "palette.h"
#include "sim_shared.h"

using namespace alchemy;
using namespace sim;

/* ═══════════════════════════ Tables ═══════════════════════════════════ */

static const char* kTapeRatioLabels[11] = {
    "1/4", "1/3", "1/2", "2/3", "3/4", "1", "3/2", "2", "3", "4", "8"};
static constexpr float kTapeRatios[11] = {
    0.25f, 1.f / 3.f, 0.5f, 2.f / 3.f, 0.75f, 1.f, 1.5f, 2.f, 3.f, 4.f, 8.f};

static const char* kGrainRatioLabels[20] = {
    "1/24", "1/16", "1/12", "1/8", "1/6", "1/4", "1/3", "1/2", "2/3", "3/4",
    "1", "3/2", "2", "3", "4", "6", "8", "12", "16", "24"};
static constexpr float kGrainRatios[20] = {
    1.f / 24.f, 1.f / 16.f, 1.f / 12.f, 0.125f, 1.f / 6.f, 0.25f, 1.f / 3.f,
    0.5f, 2.f / 3.f, 0.75f, 1.f, 1.5f, 2.f, 3.f, 4.f, 6.f, 8.f, 12.f, 16.f, 24.f};

static const char* kCountLabels[36] = {
    "1",  "2",  "3",  "4",  "5",  "6",  "7",  "8",  "9",  "10", "11", "12",
    "13", "14", "15", "16", "17", "18", "19", "20", "21", "22", "23", "24",
    "25", "26", "27", "28", "29", "30", "31", "32", "33", "34", "35", "36"};

/* Grain speed steps: noon = 1×, six steps each way (spec 2-F). */
static constexpr float kSpeedUp[6]   = {1.5f, 2.f, 3.f, 4.f, 6.f, 8.f};
static constexpr float kSpeedDown[6] = {2.f / 3.f, 0.5f, 1.f / 3.f, 0.25f, 0.125f, 1.f / 16.f};
static const char* kOctaveLabels[3] = {"Unison", "-12/0", "-12/0/+12"};
static const char* kVoiceLabels[8] = {"1", "2", "3", "4", "5", "6", "7", "8"};
static const char* kJackFnLabels[kNumJackFns] = {
    "CV > Feedback", "CV > Cutoff", "CV > Spacing", "CV > Grain Spray", "CV > Envelopes",
    "Sub-Osc Out", "Grain Trigger Out"};

namespace sim {
/* Envelope morph colours: square, LPG, triangle, slow attack. */
extern const MorphSnapPoint kEnvSnaps[4] = {
    {0.f, pal::kWhite}, {1.f / 3.f, pal::kOrange},
    {2.f / 3.f, pal::kCyan}, {1.f, pal::kViolet}};
} // namespace sim

/* Dark → warm white over the level zone (0–20 %), then the original
 * cyan → white → amber over the mix (20–100 %). */
static const MorphSnapPoint kGrainMixSnaps[5] = {
    {0.f, {0x14, 0x14, 0x14}}, {0.2f, pal::kWarmWhite}, {0.2001f, pal::kCyan},
    {0.6f, pal::kWhite}, {1.f, pal::kAmber}};
static const MorphSnapPoint kFeastSnaps[2] = {
    {0.f, pal::kViolet}, {1.f, pal::kCyan}};
static const MorphSnapPoint kSineSawSnaps[2] = {
    {0.f, pal::kPaleBlue}, {1.f, pal::kOrange}};

static constexpr pal::Rgb kSelOff = {0x14, 0x14, 0x14};

/* ═══════════════════════════ Page 1 — Tape ════════════════════════════ */

static VirtualKnob t_mix = VirtualKnob(kPotTopLeft, "Mix")
    .Linear(0.f, 100.f).Unit("%").Ident("tape.mix")
    .Ring(Level(pal::kAmber)).Overdraw(OdTapeMix);

static VirtualKnob t_clock = VirtualKnob(kPotTopRight, "Clock")
    .Selector(11).Labels(kTapeRatioLabels).Ident("tape.ratio")
    .Ring(SelectorRing(pal::kAmber, pal::Dim(pal::kAmber, 0.6f), 11))
    .Overdraw(OdTapeClock);

static VirtualKnob t_fb = VirtualKnob(kPotMiddleLeft, "Feedback")
    .Linear(0.f, 105.f).Unit("%").Ident("tape.fb")
    .Ring(Level(pal::kAmber)).Overdraw(OdTapeFeedback);

static VirtualKnob t_tone = VirtualKnob(kPotMiddleRight, "Tone")
    .Linear(-100.f, 100.f).Ident("tape.tone")
    .Ring(Bipolar(pal::kPaleGold, pal::kDarkRed, pal::kWhite)).Overdraw(OdCenter);

static VirtualKnob t_wow = VirtualKnob(kPotBottomLeft, "Wow/Flutter")
    .Linear(0.f, 100.f).Unit("%").Ident("tape.wow")
    .Ring(Level(pal::kAmber)).Overdraw(OdTapeWow);

static VirtualKnob t_micro = VirtualKnob(kPotBottomRight, "Microspeed")
    .Linear(-100.f, 100.f).Ident("tape.micro")
    .Ring(Bipolar(pal::kViolet, pal::kLime, pal::kWhite)).Overdraw(OdTapeMicro);

/* ═══════════════════════════ Page 2 — Grain ═══════════════════════════ */

static VirtualKnob g_mix = VirtualKnob(kPotTopLeft, "Mix")
    .Linear(0.f, 100.f).Unit("%").Ident("grain.mix")
    .Ring(Gradient(kGrainMixSnaps, 5));

static VirtualKnob g_clock = VirtualKnob(kPotTopRight, "Clock")
    .Selector(20).Labels(kGrainRatioLabels).Ident("grain.ratio")
    .Ring(SelectorRing(pal::kCyan, pal::Dim(pal::kCyan, 0.6f), 20))
    .Overdraw(OdGrainClock);

static VirtualKnob g_size = VirtualKnob(kPotMiddleLeft, "Size")
    .Exp(0.005f, 4.f).Unit("s").Ident("grain.size")
    .Ring(Level(pal::kCyan)).Overdraw(OdGrainSize);

static VirtualKnob g_count = VirtualKnob(kPotMiddleRight, "Count")
    .Selector(36).Labels(kCountLabels).Ident("grain.count")
    .Ring(Level(pal::kCyan))
    .Overdraw(OdGrainCount);

static VirtualKnob g_shape = VirtualKnob(kPotBottomLeft, "Envelope")
    .Linear(0.f, 100.f).Ident("grain.shape")
    .Ring(Gradient(kEnvSnaps, 4)).Overdraw(OdGrainEnv);

static VirtualKnob g_speed = VirtualKnob(kPotBottomRight, "Speed")
    .Linear(-6.f, 6.f).Unit("steps").Ident("grain.speed")
    .Ring(Bipolar(pal::kCyan, pal::kDeepBlue, pal::kWhite)).Overdraw(OdGrainSpeed);


/* ═══════════════════════════ Page 3 — Stack ═══════════════════════════ */

static VirtualKnob s_level = VirtualKnob(kPotTopLeft, "Level")
    .Linear(0.f, 100.f).Unit("%").Ident("stack.level")
    .Ring(Level(pal::kViolet)).Overdraw(OdStackLevel);

static VirtualKnob s_tune = VirtualKnob(kPotTopRight, "Tuning")
    .Linear(-2.f, 2.f).Unit("oct").Ident("stack.tune")
    .Ring(Bipolar(pal::kViolet, pal::kViolet, pal::kWhite)).Overdraw(OdStackTune);

static VirtualKnob s_detune = VirtualKnob(kPotMiddleLeft, "Detune")
    .Linear(0.f, 100.f).Unit("%").Ident("stack.detune")
    .Ring(Level(pal::kViolet)).Overdraw(OdStackDetune);

static VirtualKnob s_cutoff = VirtualKnob(kPotMiddleRight, "Cutoff")
    .Linear(-100.f, 100.f).Ident("stack.cutoff")
    .Ring(Bipolar(pal::kPink, pal::kViolet, pal::kWhite)).Overdraw(OdCenter);

static VirtualKnob s_attack = VirtualKnob(kPotBottomLeft, "Attack")
    .Linear(0.f, 100.f).Unit("%").Ident("stack.attack")
    .Ring(Level(pal::kViolet)).Overdraw(OdStackEnvelope);

static VirtualKnob s_release = VirtualKnob(kPotBottomRight, "Release")
    .Linear(0.f, 100.f).Unit("%").Ident("stack.release")
    .Ring(Level(pal::kViolet)).Overdraw(OdStackEnvelope);

/* ═══════════════════ Hidden page — B2 held (global) ═══════════════════ */

static VirtualKnob x_gain = VirtualKnob(kPotTopLeft, "Input Gain")
    .Linear(0.f, 2.f).Unit("x").Ident("glob.in")
    .Ring(Level(pal::kWarmWhite)).Overdraw(OdGain);

static VirtualKnob x_out = VirtualKnob(kPotTopRight, "Output Level")
    .Linear(0.f, 2.f).Unit("x").Ident("glob.out")
    .Ring(Level(pal::Dim(pal::kWarmWhite, 0.5f))).Overdraw(OdOutput);

static VirtualKnob x_spread = VirtualKnob(kPotMiddleLeft, "Spread")
    .Linear(0.f, 100.f).Unit("%").Ident("glob.spread")
    .Ring(Level(pal::kWarmWhite)).Overdraw(OdSpread);

static VirtualKnob x_feast = VirtualKnob(kPotMiddleRight, "Grains -> Filter")
    .Linear(0.f, 100.f).Unit("%").Ident("stack.feast")
    .Ring(Gradient(kFeastSnaps, 2)).Overdraw(OdStackFeast);

static VirtualKnob x_subdepth = VirtualKnob(kPotBottomLeft, "Sub -> Spacing")
    .Linear(0.f, 100.f).Unit("%").Ident("glob.subdepth")
    .Ring(Level(pal::kWarmWhite)).Overdraw(OdSubDepth);

static VirtualKnob x_j8 = VirtualKnob(kPotBottomRight, "CV6 Function")
    .Selector(kNumJackFns).Labels(kJackFnLabels).Ident("jack.j8")
    .Ring(SelectorRing(pal::kWhite, kSelOff, kNumJackFns))
    .Overdraw(OdJackFn, &g_ui.j8_fn);

/* ════════════ Hidden page — B3 held on Grain (Grain Parameters) ═══════ */

static VirtualKnob gx_pitch = VirtualKnob(kPotTopLeft, "Grain Pitch")
    .Linear(-24.f, 24.f).Unit("st").Ident("grain.pitch")
    .Ring(Bipolar(pal::kGrainBlue, pal::kGrainBlue, pal::kWhite)).Overdraw(OdGrainPitch);

static VirtualKnob gx_prand = VirtualKnob(kPotTopRight, "Pitch Random")
    .Linear(0.f, 100.f).Unit("%").Ident("grain.prand")
    .Ring(Level(pal::kGrainBlue)).Overdraw(OdPitchRand);

static VirtualKnob gx_spray = VirtualKnob(kPotMiddleLeft, "Grain Spray")
    .Linear(0.f, 100.f).Unit("%").Ident("glob.spray")
    .Ring(Level(pal::kGrainBlue)).Overdraw(OdSpray);

/* ════════════ Hidden page — B3 held on Stack (Stack Parameters) ═══════ */

static VirtualKnob sx_sinesaw = VirtualKnob(kPotTopLeft, "Sine/Saw")
    .Linear(0.f, 50.f).Unit("% saw").Ident("stack.sinesaw")
    .Ring(Gradient(kSineSawSnaps, 2));

static VirtualKnob sx_oct = VirtualKnob(kPotTopRight, "Octaves")
    .Selector(3).Labels(kOctaveLabels).Ident("stack.octaves")
    .Ring(SelectorRing(pal::kGreen, pal::Dim(pal::kGreen, 0.6f), 3, ZoneGeometry::Region));

static VirtualKnob sx_voices = VirtualKnob(kPotMiddleLeft, "Voices")
    .Selector(8).Labels(kVoiceLabels).Ident("stack.voices")
    .Ring(SelectorRing(pal::kGreen, pal::Dim(pal::kGreen, 0.5f), 8))
    .Overdraw(OdVoices);

static VirtualKnob sx_spacing = VirtualKnob(kPotMiddleRight, "Spacing")
    .Linear(0.f, 3.f).Unit("oct").Ident("stack.spacing")
    .Ring(Level(pal::Dim(pal::kGreen, 0.35f))).Overdraw(OdStackSpacing);

/* ═══════════════════════════ Hardware + B3 routing ════════════════════ */

static AlchemyLab hw;

/**
 * A view of B3 that only counts as pressed when the press began on one
 * particular page.  The SDK allows one hold-page per physical button, so
 * two of these give B3 a different hidden page on Grain and on Stack.
 */
class RoutedButton : public IButton
{
  public:
    RoutedButton(IButton& real, uint8_t page) : real_(real), page_(page) {}
    void Watch(const KnobStorage* s) { storage_ = s; }

    bool Pressed() const override
    {
        const bool p = real_.Pressed();
        if (p && !prev_) latched_ = storage_ && storage_->ActivePage() == page_;
        if (!p) latched_ = false;
        prev_ = p;
        return p && latched_;
    }
    bool  RisingEdge() override  { return false; }
    bool  FallingEdge() override { return false; }
    float TimeHeldMs() const override { return real_.TimeHeldMs(); }

  private:
    IButton&           real_;
    uint8_t            page_;
    const KnobStorage* storage_ = nullptr;
    mutable bool       prev_    = false;
    mutable bool       latched_ = false;
};

static RoutedButton b3_on_grain(hw.buttons[kButtonB3], kPgGrain);
static RoutedButton b3_on_stack(hw.buttons[kButtonB3], kPgStack);

static ControlLoop loop(hw);

static Pager pager = Pager(kNumPages, kNumPots)
    .Cycle(hw.buttons[kButtonB1], kPgTape, kPgGrain, kPgStack)
    .Shift(hw.buttons[kButtonB2], kPgGlobal)
    .Shift(b3_on_grain, kPgGrainX).From(kPgGrain)
    .Shift(b3_on_stack, kPgStackX).From(kPgStack);

/* ═══════════════════════════ Buttons ══════════════════════════════════ */

static const char* kFreezeLabels[2] = {"Off", "Frozen"};
static const char* kMuteLabels[2]   = {"Playing", "Muted"};
static const char* kTapeModes[2]    = {"Normal", "Ping-Pong"};
static const char* kGrainDirs[3]    = {"Forward", "Reverse", "Random"};
static const char* kOnOff[2]        = {"Off", "On"};

static void OnGrainDirTap(void*);
static void OnStackMuteTap(void*);

static VirtualButton b_tape_freeze = VirtualButton(kButtonB2, "Tape Freeze")
    .Ident("tape.freeze").Selector(kFreezeLabels).Bind(&g_params.tape_freeze);
static VirtualButton b_tape_mode = VirtualButton(kButtonB3, "Repeat Mode")
    .Ident("tape.mode").Selector(kTapeModes).Bind(&g_params.tape_pingpong);

static VirtualButton b_grain_freeze = VirtualButton(kButtonB2, "Grain Freeze")
    .Ident("grain.freeze").Selector(kFreezeLabels).Bind(&g_params.grain_freeze);
/* B3 on Grain both taps (direction) and holds (Grain Parameters).  The
 * state lives in a host-only button; the tap is momentary and skips
 * itself when the hold turned a knob. */
static VirtualButton b_grain_dir = VirtualButton("grain.dir", "Grain Direction")
    .Selector(kGrainDirs).Bind(&g_params.grain_dir);
static VirtualButton b_grain_dir_tap = VirtualButton(kButtonB3, "Grain Direction (tap)")
    .Ident("grain.dir.tap").Tap(OnGrainDirTap, "Cycle direction");

static VirtualButton b_voct_grains = VirtualButton(kButtonB2, "V/Oct > Grains")
    .Ident("grain.voct").Selector(kOnOff).Bind(&g_params.voct_grains);
static VirtualButton b_stack_mute = VirtualButton("stack.mute", "Stack Mute")
    .Selector(kMuteLabels).Bind(&g_params.stack_mute);
static VirtualButton b_stack_mute_tap = VirtualButton(kButtonB3, "Stack Mute (tap)")
    .Ident("stack.mute.tap").Tap(OnStackMuteTap, "Mute / unmute");

static ButtonBank buttons;

static void OnGrainDirTap(void*)
{
    if (pager.HoldUsed(b3_on_grain)) return;
    buttons.SetZone(b_grain_dir, static_cast<uint8_t>((buttons.ZoneOf(b_grain_dir) + 1u) % 3u));
}

static void OnStackMuteTap(void*)
{
    if (pager.HoldUsed(b3_on_stack)) return;
    buttons.SetZone(b_stack_mute, static_cast<uint8_t>(buttons.ZoneOf(b_stack_mute) ^ 1u));
}

/* ═══════════════════════════ Pages ════════════════════════════════════ */

static Page tape_page = Page(kPgTape).Name("Tape").Color(pal::kAmber)
    .Knobs(t_mix, t_clock, t_fb, t_tone, t_wow, t_micro)
    .Buttons(b_tape_freeze, b_tape_mode);
static Page grain_page = Page(kPgGrain).Name("Grain").Color(pal::kCyan)
    .Knobs(g_mix, g_clock, g_size, g_count, g_shape, g_speed)
    .Buttons(b_grain_freeze, b_grain_dir, b_grain_dir_tap);
static Page stack_page = Page(kPgStack).Name("Stack").Color(pal::kViolet)
    .Knobs(s_level, s_tune, s_detune, s_cutoff, s_attack, s_release)
    .Buttons(b_voct_grains, b_stack_mute, b_stack_mute_tap);
static Page global_page = Page(kPgGlobal).Name("Global (B2)").Color(pal::kWarmWhite)
    .Knobs(x_gain, x_out, x_spread, x_feast, x_subdepth, x_j8);
static Page grainx_page = Page(kPgGrainX).Name("Grain Parameters (B3)").Color(pal::kGrainBlue)
    .Knobs(gx_pitch, gx_prand, gx_spray);
static Page stackx_page = Page(kPgStackX).Name("Stack Parameters (B3)").Color(pal::kGreen)
    .Knobs(sx_sinesaw, sx_oct, sx_voices, sx_spacing);

/* ═══════════════════════════ Surfaces ═════════════════════════════════ */

/* Knob-motion recording (B1 hold + turn).  The paged form needs a slot
 * per (page, pot) — 36 — so loops are 8 s at 20 Hz to stay well inside
 * the preset budget.  Rarely used by design. */
using Locks = ParamLock<kNumPages * kNumPots, LockLength<8, 20>>;
static Locks locks(hw.buttons[kButtonB1], pager);

static Presets    presets(hw.seed.qspi);
static Settings   settings(hw, &pager);
static CvMatrix   cv_matrix(kNumCvInputs);
static KnobHandle s_tempo;

static hostlink::Diagnostics debug;
static hostlink::Host        host(presets, "simulacrum", "Simulacrum",
                                  "1.0.0", "v1.0.0");
static hostlink::Gauge<float>    g_cpu("audio.cpu", "Audio CPU avg");
static hostlink::Gauge<float>    g_cpu_peak("audio.cpu_peak", "Audio CPU peak (since boot)");
static hostlink::Gauge<float>    g_cpu_1s("audio.cpu_peak_1s", "Audio CPU peak (last 1 s)");
static hostlink::Gauge<uint32_t> g_overruns("audio.overruns", "Audio overruns");
static hostlink::Gauge<float>    g_bpm("clock.bpm", "Clock BPM");
static hostlink::Gauge<bool>     g_ext("clock.ext", "External clock");
static hostlink::Gauge<uint32_t> g_edges("clock.edges", "Clock edges seen (J3)");
static hostlink::Gauge<float>    g_j3v("clock.j3_volts", "J3 volts");
static hostlink::Gauge<uint32_t> g_cap("grain.cap", "Grain voice cap (CPU)");
static hostlink::Gauge<bool>     g_mono("in.mono", "J1 copied to J2");
static hostlink::Gauge<bool>     g_gate("notes.gate", "Gate (J6)");
static hostlink::Gauge<uint32_t> g_notes("notes.count", "Notes started");
static hostlink::Gauge<uint32_t> g_sounding("notes.sounding", "Stack voices sounding");
static hostlink::Gauge<float>    g_p_tape("cpu.tape", "CPU: tape %");
static hostlink::Gauge<float>    g_p_grain("cpu.grain", "CPU: grains %");
static hostlink::Gauge<float>    g_p_stack("cpu.stack", "CPU: stack + filter %");
static hostlink::Gauge<float>    g_p_other("cpu.other", "CPU: input, mix, rest %");

/* ═══════════════════════════ CV indices ═══════════════════════════════ */

static constexpr uint8_t kCvClock = 0;   /* J3 */
static constexpr uint8_t kCvVoct  = 1;   /* J4 */
static constexpr uint8_t kCvSize  = 2;   /* J5 */
static constexpr uint8_t kCvGate  = 3;   /* J6 */
static constexpr uint8_t kCvSpray = 4;   /* J7 */
static constexpr uint8_t kCvAssign = 5;  /* J8 */
static constexpr float   kCvFullRange = 2.f;   /* ±5 V reads as 0.25..0.75 */

/* ═══════════════════════════ Clock (J3) ═══════════════════════════════ */

static CvEdge   edges;                 /* J3 clock + J6 gate */

/*
 * Tempo filter.  Raw pulse intervals jitter by a millisecond or two, so:
 *   1. the median of the last 5 intervals rejects stray pulses;
 *   2. that estimate lands on a whole BPM, and only moves on when it is
 *      more than 0.6 BPM away (hysteresis — no flicker between values);
 *   3. the tempo in use glides to the new whole BPM (τ ≈ 0.4 s, 0.12 s
 *      for a jump of more than 10 %), so delay times slew rather than step.
 * The internal Tempo setting lands on whole BPM and glides the same way.
 */
static constexpr int   kMedianN   = 5;
static constexpr float kBpmHyst   = 0.6f;
static constexpr float kGlideSlow = 0.4f;    /* s */
static constexpr float kGlideFast = 0.12f;   /* s */

static uint32_t clk_last_us   = 0;
static float    intervals[kMedianN] = {};
static int      n_intervals   = 0, iv_head = 0;
static bool     clk_external  = false;
static float    bpm_target    = 120.f;   /* whole BPM the clock settled on */
static float    bpm_now       = 120.f;   /* glided tempo actually in use */
static float    glide_s       = kGlideSlow;
static uint32_t internal_beat_us = 0;
static float    held_bpm      = 0.f;     /* last external tempo, until Tempo moves */
static float    held_knob     = -1.f;

static float TempoBpm() { return roundf(30.f + 210.f * s_tempo.Value()); }

static float MedianInterval()
{
    float v[kMedianN];
    const int n = n_intervals < kMedianN ? n_intervals : kMedianN;
    for (int i = 0; i < n; i++) v[i] = intervals[i];
    for (int i = 1; i < n; i++)                       /* tiny insertion sort */
        for (int j = i; j > 0 && v[j] < v[j - 1]; j--) { const float t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
    return v[n / 2];
}

static void SetTarget(float bpm)
{
    bpm = Clamp(roundf(bpm), 20.f, 300.f);
    if (bpm == bpm_target) return;
    glide_s    = fabsf(bpm - bpm_target) > 0.1f * bpm_target ? kGlideFast : kGlideSlow;
    bpm_target = bpm;
}

static void PollClock(uint32_t now_us)
{
    if (edges.JustRose(kCvClock))
    {
        const uint32_t t = edges.LastRiseUs(kCvClock);
        if (clk_last_us != 0u)
        {
            const float dt = static_cast<float>(t - clk_last_us) * 1e-6f;
            if (dt > 0.02f && dt < 4.f)
            {
                /* A tempo jump flushes the history so it is followed fast. */
                if (n_intervals > 0 && fabsf(dt - MedianInterval()) > 0.25f * MedianInterval())
                    n_intervals = 0;
                intervals[iv_head] = dt;
                iv_head = (iv_head + 1) % kMedianN;
                if (n_intervals < kMedianN) n_intervals++;

                const float est = 60.f / MedianInterval();
                if (!clk_external)
                {
                    SetTarget(est);
                    if (n_intervals >= 2) { clk_external = true; bpm_now = bpm_target; }
                }
                else if (fabsf(est - bpm_target) > kBpmHyst)
                    SetTarget(est);
            }
        }
        clk_last_us = t;
        g_ui.last_beat_ms = t / 1000u;
        g_params.clock_epoch++;
    }

    /* Lost clock: carry on at the last external tempo until the Tempo
     * setting is moved, then follow the setting. */
    if (clk_external)
    {
        const float since = static_cast<float>(now_us - clk_last_us) * 1e-6f;
        if (since > 2.2f * (60.f / bpm_target) + 0.1f)
        {
            clk_external = false;
            n_intervals  = 0;
            held_bpm     = bpm_target;
            held_knob    = s_tempo.Value();
        }
    }

    if (!clk_external)
    {
        if (held_bpm > 0.f && fabsf(s_tempo.Value() - held_knob) > 0.01f) held_bpm = 0.f;
        SetTarget(held_bpm > 0.f ? held_bpm : TempoBpm());
    }

    /* Glide toward the target (1 ms poll). */
    bpm_now += (1.f - expf(-0.001f / glide_s)) * (bpm_target - bpm_now);
    if (fabsf(bpm_target - bpm_now) < 0.005f) bpm_now = bpm_target;
    const float period = 60.f / bpm_now;
    g_params.clock_period_s = period;

    if (!clk_external)
    {
        const uint32_t period_us = static_cast<uint32_t>(period * 1e6f);
        if (now_us - internal_beat_us >= period_us)
        {
            internal_beat_us = now_us;
            g_ui.last_beat_ms = now_us / 1000u;
        }
    }
    g_ui.ext_clock       = clk_external;
    g_ui.clock_period_ms = 1000.f * period;
}

/* ═══════════════════════════ Notes (J6 gate + J4 V/Oct) ═══════════════ */
/*
 * A note's pitch is fixed when it starts, so it must be read from a
 * V/Oct that has finished moving:
 *   - "settled" = the raw J4 voltage has stayed within ±4 mV for 3 ms
 *     (a sequencer step, or the end of a glide); the note takes the
 *     average over that window.
 *   - Rising gate → note as soon as V/Oct is settled (at most 10 ms).
 *   - Gate held → a new note when the settled V/Oct is more than 25 mV
 *     (~30 cents) from the last note.  Glides only make a note where
 *     they land, never on the way.
 *   - Gate released → all notes release.
 * J6 has no switch contact, so an empty J6 simply never plays notes.
 */
static constexpr float    kSettleBandV  = 0.004f;
static constexpr uint32_t kSettleMs     = 3u;
static constexpr uint32_t kGateWaitMs   = 10u;
static constexpr float    kNoteThreshV  = 0.025f;

static float    voct_smooth   = 0.f;    /* live V/Oct (grain pitch, sub) */
static bool     gate_on       = false;
static bool     note_pending  = false;  /* gate rose, waiting for V/Oct */
static uint32_t pending_ms    = 0;
static float    last_note_v   = 0.f;
static float    settle_ref    = 0.f;    /* start of the current still window */
static float    settle_sum    = 0.f;
static uint32_t settle_ms     = 0;

static void StartNote(float volts)
{
    g_params.note_volts = volts;
    g_params.note_seq++;
    last_note_v = volts;
}

static void PollNotes(float raw_v)
{
    /* Track how long V/Oct has been still. */
    if (fabsf(raw_v - settle_ref) > kSettleBandV)
    {
        settle_ref = raw_v;
        settle_sum = 0.f;
        settle_ms  = 0;
    }
    settle_sum += raw_v;
    settle_ms++;
    const bool  settled = settle_ms >= kSettleMs;
    const float settled_v = settle_sum / static_cast<float>(settle_ms);

    if (edges.JustRose(kCvGate))
    {
        gate_on      = true;
        note_pending = true;
        pending_ms   = 0;
        g_params.gated_mode = 1u;
    }
    if (edges.JustFell(kCvGate))
    {
        gate_on      = false;
        note_pending = false;
    }
    g_params.gate = gate_on ? 1u : 0u;

    if (note_pending)
    {
        if (settled || ++pending_ms >= kGateWaitMs)
        {
            StartNote(settled ? settled_v : raw_v);
            note_pending = false;
        }
        return;
    }
    if (gate_on && settled && fabsf(settled_v - last_note_v) > kNoteThreshV)
        StartNote(settled_v);
}

static void OnPoll(uint32_t /*t_ms*/)
{
    const uint32_t now_us = loop.PollTimeUs();
    edges.Tick(loop.Cv(), loop.NumCv(), now_us);
    PollClock(now_us);

    /* Light smoothing removes ADC hash without audible portamento. */
    const float raw_v = hw.j4.Volts();
    voct_smooth += 0.35f * (raw_v - voct_smooth);
    g_params.voct_volts = voct_smooth;

    PollNotes(raw_v);
}

/* ═══════════════════════════ J8 (assignable) ══════════════════════════ */

/* Jack names for the Hermetic Modular editor.  J8 keeps a stable id and
 * swaps its description when CV6 Function changes; the editor picks up
 * the new name via a descriptor refresh. */
static constexpr Jack kJ1("J1", "In L", JackSig::AudioIn);
static constexpr Jack kJ2 = Jack("J2", "In R", JackSig::AudioIn).Normalled(kJ1);
static constexpr Jack kJ3("J3", "CV1: Clock In", JackSig::Trig);
static constexpr Jack kJ4("J4", "CV2: V/Oct In", JackSig::Voct);
static constexpr Jack kJ5("J5", "CV3: Grain Size", JackSig::CvBi);
static constexpr Jack kJ6("J6", "CV4: Gate / Trigger", JackSig::Gate);
static constexpr Jack kJ7("J7", "CV5: Grain Spray", JackSig::CvBi);
static constexpr Jack kJ8[kNumJackFns] = {
    Jack("J8", "CV6: Feedback CV",      JackSig::CvBi),
    Jack("J8", "CV6: Cutoff CV",        JackSig::CvBi),
    Jack("J8", "CV6: Spacing CV",       JackSig::CvBi),
    Jack("J8", "CV6: Grain Spray CV",   JackSig::CvBi),
    Jack("J8", "CV6: Envelopes CV",     JackSig::CvBi),
    Jack("J8", "CV6: Sub-Osc Out",      JackSig::CvBi),
    Jack("J8", "CV6: Grain Trigger Out", JackSig::Trig),
};
static constexpr Jack kJ9("J9", "Out L", JackSig::AudioOut);
static constexpr Jack kJ10("J10", "Out R", JackSig::AudioOut);

static bool     s_host_started = false;
static uint32_t j8_applied     = 0xFFu;

static void PublishJacks(uint32_t fn8)
{
    host.Jacks(kJ1, kJ2, kJ3, kJ4, kJ5, kJ6, kJ7,
               kJ8[fn8 < kNumJackFns ? fn8 : 0], kJ9, kJ10);
    if (s_host_started) host.RequestDescriptorRefresh();
}

/* CV → Envelopes: J8 offsets attack and release together (±5 V = ±1). */
static void OnEnvelopeCv(float cv, uint32_t, void*)
{
    g_params.env_cv = Clamp(kCvFullRange * (cv - 0.5f), -1.f, 1.f);
}

/* CV → Grain Spray: J8 adds to the spray knob (and J7), ±5 V = ±1. */
static void OnSprayCv(float cv, uint32_t, void*)
{
    g_params.spray_cv = Clamp(kCvFullRange * (cv - 0.5f), -1.f, 1.f);
}

static void ApplyJ8(uint32_t fn)
{
    if (fn == j8_applied) return;
    j8_applied = fn;
    g_params.env_cv   = 0.f;
    g_params.spray_cv = 0.f;
    CvJack& jack = hw.j8;
    switch (fn)
    {
        case kJackCvFeedback:  jack.DisableCvOutput(); cv_matrix.Jack(kCvAssign).To(t_fb).Atten(kCvFullRange);       break;
        case kJackCvCutoff:    jack.DisableCvOutput(); cv_matrix.Jack(kCvAssign).To(s_cutoff).Atten(kCvFullRange);   break;
        case kJackCvSpacing:   jack.DisableCvOutput(); cv_matrix.Jack(kCvAssign).To(sx_spacing).Atten(kCvFullRange); break;
        case kJackCvSpray:     jack.DisableCvOutput(); cv_matrix.Jack(kCvAssign).Custom(OnSprayCv);                 break;
        case kJackCvEnvelopes: jack.DisableCvOutput(); cv_matrix.Jack(kCvAssign).Custom(OnEnvelopeCv);              break;
        default:               cv_matrix.Jack(kCvAssign).Off(); jack.EnableCvOutput();                              break;
    }
    PublishJacks(fn);
}

/* ═══════════════════════════ Audio ════════════════════════════════════ */

static void AudioCallback(daisy::AudioHandle::InputBuffer  in,
                          daisy::AudioHandle::OutputBuffer out,
                          size_t                           size)
{
    EngineProcess(in, out, size);

    /* J8 as an output: updated once per block (2 kHz). */
    const uint32_t fn = g_params.j8_fn;
    if (fn == kJackSubOut)       hw.j8.SetVolts(5.f * EngineSubValue());
    else if (fn == kJackTrigOut) hw.j8.SetVolts(EngineTrigGate() ? 5.f : 0.f);
}

/* ═══════════════════════════ Startup defaults ═════════════════════════ */
/* Factory values (0..1 knob positions) used when no preset is saved; the
 * editor's "factory" values are these too.  Pots pick a value up once
 * you turn through it (pot catch), so nothing jumps at power-on.
 * Unused pots on the B3 pages are listed as 0 and stay dark. */

static constexpr float Zone(int z, int n) { return (static_cast<float>(z) + 0.5f) / n; }

static const float kDefaults[kNumPages][kNumPots] = {
    /* Tape:  mix 60 % (= 50 % of the dry/wet range), clock ×1, feedback 25 %,
     *        tone flat, wow 10 %, micro off */
    {0.60f, Zone(5, 11), 25.f / 105.f, 0.5f, 0.10f, 0.5f},
    /* Grain: mix 40 % (= 25 % of the dry/wet range), clock ×1, size 1 s,
     *        count 6, envelope 0 (square), 1× */
    {0.40f, Zone(10, 20), 0.7926f, Zone(5, 36), 0.f, 0.5f},
    /* Stack: level 0, C4, detune 10 %, cutoff open, attack 0, release 0 */
    {0.f, 0.5f, 0.10f, 0.5f, 0.f, 0.f},
    /* Global: gain 1×, out 1×, spread 25 %, grains→filter 25 %, sub 25 %, CV6 cutoff */
    {0.5f, 0.5f, 0.25f, 0.25f, 0.25f, Zone(kJackCvCutoff, kNumJackFns)},
    /* Grain Parameters: pitch 0 st, random 0 %, spray 0 % */
    {0.5f, 0.f, 0.f, 0.f, 0.f, 0.f},
    /* Stack Parameters: sine/saw noon, unison, 1 voice, spacing 1 oct */
    {0.5f, Zone(0, 3), Zone(0, 8), 1.f / 3.f, 0.f, 0.f},
};

static void SeedDefaults()
{
    for (uint8_t pg = 0; pg < kNumPages; pg++)
        for (uint8_t pot = 0; pot < kNumPots; pot++)
            pager.SetStored(pg, pot, kDefaults[pg][pot], loop.Phys());
}

/* ═══════════════════════════ Frame ════════════════════════════════════ */

static void UpdateParams()
{
    Params& p = g_params;

    /* Tape */
    p.tape_mix      = t_mix.Norm();
    p.tape_ratio    = kTapeRatios[static_cast<int>(t_clock.Value())];
    p.tape_feedback = t_fb.Value() * 0.01f;
    p.tape_tone     = 0.5f + 0.5f * BipolarDetent(t_tone.Norm());
    p.tape_wow      = t_wow.Norm();
    p.tape_micro    = 0.5f + 0.5f * BipolarDetent(t_micro.Norm());

    /* Grain */
    p.grain_mix    = g_mix.Norm();
    p.grain_ratio  = kGrainRatios[static_cast<int>(g_clock.Value())];
    p.grain_size_s = g_size.Value();
    p.grain_count  = static_cast<uint32_t>(g_count.Value()) + 1u;
    p.grain_shape  = g_shape.Norm();
    {
        /* Stepped: 1× in the centre detent, six steps each way. */
        const float e = BipolarDetent(g_speed.Norm());
        int step = static_cast<int>(fabsf(e) * 6.f);
        if (step > 5) step = 5;
        p.grain_speed = e == 0.f ? 1.f : (e > 0.f ? kSpeedUp[step] : kSpeedDown[step]);
    }

    /* Stack */
    p.stack_level    = s_level.Norm();
    p.stack_tune_oct = s_tune.Value();
    p.stack_detune   = s_detune.Norm();
    p.stack_cutoff   = 0.5f + 0.5f * BipolarDetent(s_cutoff.Norm());
    p.stack_attack   = s_attack.Norm();
    p.stack_release  = s_release.Norm();

    /* Global */
    p.in_gain     = 1.f + BipolarDetent(x_gain.Norm());     /* 0..2×, 1× detent */
    p.out_level   = 1.f + BipolarDetent(x_out.Norm());
    p.spread      = x_spread.Norm();
    p.stack_feast = x_feast.Norm();
    p.sub_depth   = x_subdepth.Norm();

    /* Grain Parameters.  Pitch: semitone steps, offset by the stack
     * tuning's fine offset so incoming audio can be pitched to the
     * stack; with V/Oct → Grains on, V/Oct moves the grains too. */
    const float tune_st  = 12.f * p.stack_tune_oct;
    const float fine_st  = tune_st - roundf(tune_st);
    const float pitch_st = roundf(24.f * BipolarDetent(gx_pitch.Norm()));
    float gp = pitch_st + fine_st;
    if (p.voct_grains) gp += 12.f * p.voct_volts;
    p.grain_pitch_st   = gp;
    p.grain_pitch_rand = gx_prand.Norm();
    p.grain_spray      = gx_spray.Norm();

    /* Stack Parameters */
    p.sine_saw      = sx_sinesaw.Norm();
    p.octaves       = static_cast<uint32_t>(sx_oct.Value());
    p.stack_voices  = static_cast<uint32_t>(sx_voices.Value()) + 1u;
    p.stack_spacing = sx_spacing.Norm();

    /* J8 */
    p.j8_fn = static_cast<uint32_t>(x_j8.Value());
    ApplyJ8(p.j8_fn);
    s_host_started = true;   /* the host starts at the first loop poll */

    /* UI mirror for the LEDs */
    const uint8_t active = pager.ActivePage();
    g_ui.active_page = active;
    if (active <= kPgStack) g_ui.main_page = active;
    g_ui.settings_active = settings.IsActive();
    g_ui.tape_freeze     = p.tape_freeze;
    g_ui.grain_freeze    = p.grain_freeze;
    g_ui.stack_mute      = p.stack_mute;
    g_ui.tape_pingpong   = p.tape_pingpong;
    g_ui.grain_dir       = p.grain_dir;
    g_ui.voct_grains     = p.voct_grains;
    g_ui.grain_pitch_st  = pitch_st;
    g_ui.j8_fn           = p.j8_fn;

    /* Diagnostics (USB: hostlink-cli or the web programmer console). */
    g_cpu.Set(100.f * g_tel.cpu_load);
    g_cpu_peak.Set(100.f * g_tel.cpu_peak);
    g_cpu_1s.Set(100.f * g_tel.cpu_peak_1s);
    g_overruns.Set(g_tel.overruns);
    g_bpm.Set(bpm_now);
    g_ext.Set(g_ui.ext_clock);
    g_edges.Set(p.clock_epoch);
    g_j3v.Set(hw.j3.Volts());
    g_cap.Set(g_tel.voice_cap);
    g_mono.Set(g_tel.mono_in);
    g_gate.Set(p.gate != 0u);
    g_notes.Set(p.note_seq);
    g_sounding.Set(g_tel.voices_sounding);
    g_p_tape.Set(100.f * g_tel.prof_tape);
    g_p_grain.Set(100.f * g_tel.prof_grain);
    g_p_stack.Set(100.f * g_tel.prof_stack);
    g_p_other.Set(100.f * g_tel.prof_other);
}

static void OnRender(uint32_t t_ms)
{
    /* OnFrame stands down while Settings is open, so refresh this here. */
    g_ui.settings_active = settings.IsActive();
    RenderButtons(hw.leds, t_ms);
}

/* ═══════════════════════════ main ═════════════════════════════════════ */

int main()
{
    hw.Init();
    EngineInit(hw.SampleRate(), hw.BlockSize());
    b3_on_grain.Watch(&pager);
    b3_on_stack.Watch(&pager);
    g_params.gated_mode = 1u;   /* notes need a gate / trigger on J6 */

    host.Extend(debug);
    PublishJacks(kJackCvCutoff);
    debug.Watch(g_cpu);
    debug.Watch(g_cpu_peak);
    debug.Watch(g_cpu_1s);
    debug.Watch(g_overruns);
    debug.Watch(g_bpm);
    debug.Watch(g_ext);
    debug.Watch(g_edges);
    debug.Watch(g_j3v);
    debug.Watch(g_cap);
    debug.Watch(g_mono);
    debug.Watch(g_gate);
    debug.Watch(g_notes);
    debug.Watch(g_sounding);
    debug.Watch(g_p_tape);
    debug.Watch(g_p_grain);
    debug.Watch(g_p_stack);
    debug.Watch(g_p_other);

    /* J3 clock and J6 gate: Value() is 0.05 per volt around 0.5 = 0 V.
     * Rise above +1.5 V, release below +0.7 V — works for 2.5..10 V. */
    CvEdge::Config cfg;
    cfg.lo_threshold = 0.535f;
    cfg.hi_threshold = 0.575f;
    cfg.debounce_us  = 1000u;
    edges.Init(kNumCvInputs, cfg);

    /* Static CV routing; J8 follows CV6 Function. */
    cv_matrix.Jack(kCvClock).Off();
    cv_matrix.Jack(kCvVoct).Off();
    cv_matrix.Jack(kCvSize).To(g_size).Atten(kCvFullRange);     /* J5 */
    cv_matrix.Jack(kCvGate).Off();                              /* J6: notes */
    cv_matrix.Jack(kCvSpray).To(gx_spray).Atten(kCvFullRange);  /* J7 */

    settings.UseBrightness();
    settings.UsePresets(presets);
    settings.Page(1).Name("Clock");
    s_tempo = settings.Page(1).Pot(kPotTopLeft).Knob()
                  .Default((120.f - 30.f) / 210.f)
                  .Color(pal::kWarmWhite)
                  .Ident("clock.tempo")
                  .Name("Tempo (30-240 BPM, no clock on J3)");

    presets.Manage(pager);
    presets.Manage(locks);
    presets.Manage(buttons);
    presets.Manage(settings);
    presets.UseNames();

    loop.Use(pager)
        .Use(locks)
        .Use(settings)
        .Use(cv_matrix)
        .Use(buttons)
        .Use(host)
        .Use(tape_page)
        .Use(grain_page)
        .Use(stack_page)
        .Use(global_page)
        .Use(grainx_page)
        .Use(stackx_page)
        .OnPoll(OnPoll)
        .OnFrame(UpdateParams)
        .OnRender(OnRender);

    SeedDefaults();          /* before BootLoad: these are the factory values */
    presets.Init();
    presets.BootLoad();

    hw.StartAudio(AudioCallback);

    RunSplash(hw.leds, hw.Arc());

    for (;;) loop.Tick();
}
