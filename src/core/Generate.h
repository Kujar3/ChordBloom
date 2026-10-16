#pragma once

// Progression starting points: mood presets and the "Spark" generator. Pure C++17, no JUCE, deterministic.

#include <cstdint>

#include "Palette.h"
#include "Progression.h"

namespace cb
{

enum class Mood
{
    Uplifting = 0,   // bright, anthemic: mostly Tonic/Subdominant motion, few colour chords
    Emotional,       // bittersweet: vi/i-centred, sus and add9 colour, borrowed iv / bVI
    Dark,            // minor, aeolian/phrygian motion (i, bVI, bVII, iv, v), sparse colour
    Dreamy,          // maj7 / add9 / maj9 colour, lydian-ish IV, slow functional pull
    Epic,            // cinematic: i-VI-III-VII family, borrowed bVI/bVII in major
    LoFi,            // jazzy 7ths and 9ths, ii-V motion, occasional secondary dominants
    NumMoods
};

constexpr int numMoods = (int) Mood::NumMoods;
const char* moodName (Mood);   // "Uplifting", "Emotional", "Dark", "Dreamy", "Epic", "Lo-fi"

struct Preset
{
    std::string name;          // e.g. "Pop anthem", "Andalusian cadence", "Neo-soul ii-V"
    std::string description;   // one short line, e.g. "The four chords behind countless pop hits."
    std::string romanSummary;  // numerals joined by " - " in this key, e.g. "I - V - vi - IV"
    Mood mood;
    Progression progression;   // resolved in the key; lengthBeats per slot set by the preset (usually 4)
};

// 12-20 well-known, generic progressions suited to the key's family (major-family or minor-family scales; for modes
// include their signature moves, e.g. Mixolydian I-bVII-IV, Dorian i-IV, Phrygian i-bII), resolved into the key.
// Weighted towards what modern producers actually use (see out/midi_stats.md: minor keys dominate; i-VI-III-VII,
// i-VII-VI-VII, VI-VII-i, i-iv-VI-v; major keys borrow bVI/bVII/iv). Mix triads with 7th/add9 colour variants
// (e.g. a "lo-fi" i7-iv7-VImaj7-v7). Deterministic, no duplicates (by progression contents). Every chord is either in
// the key's palette or a borrowed/secondary chord that buildPalette (key) would also show.
std::vector<Preset> presetsFor (const Key&);

struct SparkSettings
{
    Mood mood = Mood::Emotional;
    int numChords = 4;         // 2..8 (clamped)
    int beatsPerChord = 4;     // 1..16 (clamped)
};

// Generates a progression by chaining transitionScore over the palette rows the mood allows, weighted by the mood
// (colour/borrowed/secondary probability), avoiding immediate repeats and same-root neighbours, starting on a tonic-
// family chord (or a mood-typical opener such as bVI/VI for Epic), and favouring a last chord that leads back to the
// first. Seeded PRNG (xorshift32 or similar): same (key, palette, seed, settings) -> identical result; different seeds
// give clearly different results. Never more than kMaxSlots chords. Never empty for a non-empty palette.
Progression sparkProgression (const Key&, const std::vector<PaletteSection>& palette, std::uint32_t seed,
                              const SparkSettings&);

} // namespace cb
