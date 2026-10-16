#pragma once

// Chord palette generation: the grid of chord pads shown for a key. Pure C++17, no JUCE.

#include "Theory.h"

namespace cb
{

struct PaletteChord
{
    Chord chord;
    std::string roman;          // romanNumeral (chord, key)
    HarmonicFunction function;  // harmonicFunction (chord, key)
    std::string hint;           // one short sentence for the tooltip, e.g. "Borrowed from C Natural Minor: a bittersweet lift."
};

enum class PaletteSectionId
{
    Triads = 0,   // the 7 diatonic triads, degree order
    Sevenths,     // the 7 diatonic seventh chords, degree order
    Color,        // up to 8 diatonic colour chords (add9, 6, 6/9, maj9, m9, m11, maj7#11, 13, sus ...) in the order
                  // producers use them (out/midi_stats.md); every tone in the key (C major: Cadd9 C6/9 Cmaj9 Dm9 ...)
    Borrowed,     // up to 8 chords from key.parallel() - and for Harmonic / Melodic Minor, Dorian, Phrygian, Locrian also
                  // from the natural minor on the same tonic - that are NOT diatonic in key
                  // (C major: bVI, bVII, iv, bIII, iv7, bVImaj7, v, i7)
    Secondary,    // up to 6 secondary dominants V7/x for diatonic non-diminished, non-tonic targets x, in target-degree order
    NumSections
};

constexpr int numPaletteSections = (int) PaletteSectionId::NumSections;
constexpr int kMaxPadsPerSection = 8;

struct PaletteSection
{
    PaletteSectionId id;
    std::string title;                  // "Triads", "Sevenths", "Color", "Borrowed", "Secondary"
    std::vector<PaletteChord> chords;   // 0..kMaxPadsPerSection entries, no duplicate chords within a section
};

// Always returns numPaletteSections sections in PaletteSectionId order (a section may be empty).
// Deterministic: same key -> identical output.
std::vector<PaletteSection> buildPalette (const Key&);

// Convenience: the diatonic chord on a degree (0..6). sevenths=false -> triad, true -> seventh chord.
// Used by the one-finger chord trigger mode (white keys C..B -> degrees 0..6).
Chord diatonicChord (const Key&, int degree, bool sevenths);

} // namespace cb
