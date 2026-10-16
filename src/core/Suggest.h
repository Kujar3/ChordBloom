#pragma once

// Next-chord suggestions ("what sounds good after this chord?"). Pure C++17, no JUCE.

#include "Palette.h"

namespace cb
{

// 0..1: how naturally `to` follows `from` in `key`. Deterministic; returns 0 when to == from.
// Combines (weights are the implementer's choice, documented in Suggest.cpp):
//  - functional motion between scale degrees (major-ish: I->IV/V/vi/ii, ii->V/vii°, iii->vi/IV, IV->V/I/ii,
//    V->I/vi, vi->ii/IV/V, vii°->I; the analogous minor-key moves i->iv/VI/VII/v, iv->V/i/VII, VI->VII/iv/III, VII->III/i ...)
//  - root motion (down a fifth strongest, then down a third / up a step)
//  - shared tones between the two chords
//  - a secondary dominant resolving to its target (V7/x -> x scores ~1.0)
//  - common borrowed moves (bVI->bVII, bVII->I, iv->I, bVI->V)
// Scores should spread across the range: a typical palette has a few chords >= 0.75, several around 0.4-0.6, the rest lower.
float transitionScore (const Chord& from, const Chord& to, const Key& key);

// Score for every pad in the palette, same shape as palette[section].chords.
std::vector<std::vector<float>> suggestionsFor (const Chord& from, const std::vector<PaletteSection>& palette, const Key& key);

} // namespace cb
