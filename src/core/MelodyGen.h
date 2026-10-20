#pragma once

// Phase 2 melody generator (the roll's Generate): motif + variation phrasing, chord-tone targeting on strong beats,
// passing / neighbour tones between, open / closed cadences and contour, tuned to aggregate corpus statistics
// (GP section 1; final-spec section 5.4). Pure C++17, no JUCE, deterministic (in-house xorshift32 behind a murmur3
// finaliser, per-stage streams). May allocate; budget < 5 ms for a 64-bar loop in Release (runs on a click).

#include "Generate.h"
#include "Melody.h"

#include <cstdint>
#include <string>
#include <utility>

namespace cb
{

enum class MelodyStyle { Uplifting = 0, Lyrical, Driving, Sparse, NumStyles };   // append-only (cb.gen.style)
constexpr int numMelodyStyles = (int) MelodyStyle::NumStyles;
const char* melodyStyleName (MelodyStyle);          // "Uplifting lead", "Lyrical hook", "Driving riff", "Sparse"
const char* melodyStyleShortName (MelodyStyle);     // "Uplifting", "Lyrical", "Driving", "Sparse"
const char* melodyStyleDescription (MelodyStyle);   // one short line for the menu / tooltip

// Default style for the lane's Spark mood: Uplifting -> Uplifting, Emotional -> Lyrical, Dark -> Driving,
// Dreamy -> Sparse, Epic -> Uplifting, LoFi -> Lyrical.
MelodyStyle melodyStyleForMood (Mood);

enum class MelodyContour { Auto = 0, Rising, Falling, Arch, Wave, NumContours };   // append-only (cb.gen.contour)
constexpr int numMelodyContours = (int) MelodyContour::NumContours;
const char* melodyContourName (MelodyContour);   // "Auto", "Rising", "Falling", "Arch", "Wave"

struct MelodyGenSettings
{
    MelodyStyle style = MelodyStyle::Lyrical;
    float density = 0.5f;        // 0..1 (clamped): the style's low .. default (0.5) .. high notes per active bar
    float repetition = 0.5f;     // 0..1 (clamped): 0 through-composed, 0.5 corpus default, 1 loop-like
    int registerShift = 0;       // -12..+12 semitones (clamped) on the register window
    MelodyContour contour = MelodyContour::Auto;
    bool resolveEnding = true;   // the loop's last phrase ends on a tonic-family tone

    bool operator== (const MelodyGenSettings&) const;
    bool operator!= (const MelodyGenSettings& o) const { return ! (*this == o); }
};

struct MelodyGenContext
{
    std::vector<std::vector<int>> voicings;   // voiceProgression output per slot (register window); may be empty (top 68)
    std::vector<NoteEvent> accompaniment;     // one loop of what else plays (chords ch 1, bass ch 2, arp ch 4); may be empty
};

// A new melody over [0, progression.totalBeats()). Same (progression, key, context, seed, settings) -> identical
// melody; seed + 1 -> a clearly different one. Streams: the plan depends only on (seed, key, slot chords, inversions
// and lengths, style, repetition); rhythm adds density; pitches add registerShift, contour, resolveEnding and the
// context. So the same seed with another contour or registerShift keeps every onset time, and with another density
// keeps the plan (melodyPlanSignature).
// Result: sanitised; every note starts in [0, totalBeats) on the 1/16 grid and ends at totalBeats at the latest;
// pitches inside melodyRegisterWindow and [36, 96]; at most kMaxMelodyNotes; velocity kDefaultMelodyVelocity.
// Empty for an empty progression.
Melody generateMelody (const Progression&, const Key&, const MelodyGenContext&, std::uint32_t seed,
                       const MelodyGenSettings&);

// The {low, high} window generateMelody uses: the style window over the median voicing top, + registerShift, + the
// capped shift above the arp (rule R8) when the accompaniment has channel-4 notes; clamped to [36, 96].
std::pair<int, int> melodyRegisterWindow (const Progression&, const MelodyGenContext&, const MelodyGenSettings&);

// Test hook: the plan as text, e.g. "U2 ABAB gap:-,-,2,- cad:o,c,o,c pedal:0" (unit bars, form letters, gap beats per
// unit, open / closed cadence tags, Driving pedal mode). Depends only on what the plan stream depends on.
std::string melodyPlanSignature (const Progression&, const Key&, std::uint32_t seed, const MelodyGenSettings&);

// Measurements behind the CoreTests bands and the demos report. Bars = absolute 4-beat bars of the loop; strong =
// steps 0 and 8 of a bar; a phrase-final note is followed by >= 1 beat without an onset, or is the last note.
struct MelodyStats
{
    int notes = 0, lowest = 0, highest = 0;
    double notesPerActiveBar = 0.0;            // notes / bars with at least one onset
    double chordToneShare = 0.0;               // pitch class is a tone of chordAtBeat (start), extensions included
    double downbeatChordToneShare = 0.0;       // the same over notes starting on a bar downbeat
    double phraseFinalChordToneShare = 0.0;
    double repeatShare = 0.0;                  // consecutive intervals of 0
    double stepShareMoving = 0.0;              // |interval| 1..2 among non-zero intervals
    double leapShareMoving = 0.0;              // |interval| >= 5 among non-zero intervals
    double reverseAfterLeap = 0.0;             // after a >= 5 leap, the next non-zero interval goes the other way
    double odd16Share = 0.0;                   // onsets on odd 16th steps
    double rhythmRepeatLag4 = 0.0;             // bars b >= 4 whose onset mask equals bar b - 4 (0 for loops < 8 bars)
    double strongBeatClashShare = 0.0;         // strong-beat non-chord notes a semitone from a sounding accompaniment note
    double outOfChordScaleShare = 0.0;         // notes on a scale tone that the chord's same-letter out-of-key tone replaces
    int seamInterval = 0;                      // |last note - first note| (the loop plays them in a row)
};
MelodyStats analyseMelody (const Melody&, const Progression&, const Key&,
                           const std::vector<NoteEvent>& accompaniment = {});

} // namespace cb
