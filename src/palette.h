/**
 * palette.h — Simulacrum LED colours (spec "LEDs").  Brightness is
 * applied globally by the panel (Settings default 30 %).
 */

#pragma once

#include "alchemy/led/panel.h"

namespace sim::pal {

using Rgb = alchemy::LedPanel::Rgb;

/* Page identities */
constexpr Rgb kAmber     = {0xFF, 0x6A, 0x00};   /* Tape   */
constexpr Rgb kCyan      = {0x00, 0xD8, 0xFF};   /* Grain  */
constexpr Rgb kViolet    = {0x8A, 0x2B, 0xFF};   /* Stack  */
constexpr Rgb kWarmWhite = {0xFF, 0xC8, 0x8C};   /* B2 hidden page */
constexpr Rgb kLime      = {0x9C, 0xFF, 0x00};   /* accents */
constexpr Rgb kGrainBlue = {0x10, 0x60, 0xFF};   /* Grain Parameters (B3) */

/* Accents */
constexpr Rgb kWhite     = {0xFF, 0xFF, 0xFF};
constexpr Rgb kRed       = {0xFF, 0x00, 0x00};
constexpr Rgb kDarkRed   = {0x90, 0x0C, 0x00};
constexpr Rgb kPaleGold  = {0xFF, 0xD8, 0x80};
constexpr Rgb kDeepBlue  = {0x10, 0x30, 0xFF};
constexpr Rgb kMagenta   = {0xFF, 0x10, 0xA0};
constexpr Rgb kPink      = {0xFF, 0x40, 0xA0};
constexpr Rgb kDeepViolet= {0x50, 0x00, 0xC0};
constexpr Rgb kPaleBlue  = {0x80, 0xB0, 0xFF};
constexpr Rgb kOrange    = {0xFF, 0x80, 0x10};
constexpr Rgb kGreen     = {0x00, 0xFF, 0x40};
constexpr Rgb kYellow    = {0xFF, 0xD0, 0x00};

/* J8 option colours: the colour of what each option drives. */
constexpr Rgb kJackFnColors[6] = {
    kAmber,       /* CV → feedback      */
    kViolet,      /* CV → cutoff        */
    kPink,        /* CV → notch spacing */
    kWarmWhite,   /* CV → envelopes     */
    kDeepViolet,  /* sub-oscillator out */
    kCyan,        /* grain trigger out  */
};

inline Rgb Dim(Rgb c, float k) { return alchemy::LedPanel::Scale(c, k); }

} // namespace sim::pal
