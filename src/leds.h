/**
 * leds.h — Simulacrum ring animations, button LEDs, and the start-up sweep.
 *
 * Every knob keeps a declarative base ring (so the SDK's value fill and
 * pot-catch indicator stay correct); the functions here are `.Overdraw()`
 * callbacks layered on top, driven by live engine telemetry.
 */

#pragma once

#include <cstdint>
#include "alchemy/hardware_types.h"
#include "alchemy/led/panel.h"

namespace sim {

/* Page indices (Pager order). */
enum : uint8_t
{
    kPgTape   = 0,
    kPgGrain  = 1,
    kPgStack  = 2,
    kPgGlobal = 3,   /* B2 held */
    kPgGrainX = 4,   /* B3 held on the Grain page: Grain Parameters */
    kPgStackX = 5,   /* B3 held on the Stack page: Stack Parameters */
    kNumPages = 6,
};

/** Control-side state the LEDs need (written by the control loop). */
struct UiState
{
    uint8_t  active_page     = kPgTape;
    uint8_t  main_page       = kPgTape;   ///< last non-hidden page
    bool     settings_active = false;
    bool     ext_clock       = false;
    uint32_t last_beat_ms    = 0;         ///< last clock pulse (ext or internal)
    float    clock_period_ms = 500.f;
    uint32_t tape_freeze     = 0;
    uint32_t grain_freeze    = 0;
    uint32_t stack_mute      = 0;
    uint32_t tape_pingpong   = 0;
    uint32_t grain_dir       = 0;
    uint32_t voct_grains     = 0;
    float    grain_pitch_st  = 0.f;
    uint32_t j8_fn           = 1;
};

extern UiState g_ui;

using RingFnSig = void(alchemy::LedPanel&, uint8_t, const alchemy::ArcGeometry&,
                       float, uint32_t, void*);

/* Any bipolar ring: wider centre indicator inside the detent. */
RingFnSig OdCenter;
/* Tape */
RingFnSig OdTapeMix, OdTapeClock, OdTapeFeedback, OdTapeWow, OdTapeMicro;
/* Grain */
RingFnSig OdGrainClock, OdGrainSize, OdGrainCount, OdGrainEnv, OdGrainSpeed;
/* Stack */
RingFnSig OdStackTune, OdStackDetune, OdStackLevel, OdStackSpacing, OdStackFeast,
          OdStackEnvelope, OdVoices;
/* B2 hidden */
RingFnSig OdGain, OdTempo, OdSpread, OdSpray, OdSubDepth, OdOutput;
/* B3 hidden */
RingFnSig OdGrainPitch, OdPitchRand, OdJackFn;

/** Paint all three buttons (top + bottom LEDs).  Called from OnRender. */
void RenderButtons(alchemy::LedPanel& panel, uint32_t t_ms);

/** Blocking ~2 s start-up animation. */
void RunSplash(alchemy::LedPanel& panel, const alchemy::ArcGeometry& geo);

} // namespace sim
