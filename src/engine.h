/**
 * engine.h — Simulacrum's audio engine: the three layers plus the mixer.
 *
 * Per block:
 *   1. sub sine (stack) — needed by the tape FM and the notch spacing
 *   2. tape  (input → tape wet)
 *   3. grain (records input ⇄ tape layer output, per 2-A)
 *   4. stack (oscillators + Feast grains → dual notch)
 *   5. mix:  dry·min(dry_tape, dry_grain) + tape wet + grain wet + stack
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace sim {

void EngineInit(float sample_rate, size_t block_size);

/** The audio callback body (the board shim wraps it). */
void EngineProcess(const float* const* in, float** out, size_t n);

/** Last sub-sine sample (−1..1) and grain trigger gate, for J7/J8. */
float EngineSubValue();
bool  EngineTrigGate();

} // namespace sim
