#pragma once

// Rhythm patterns: rendering a voiced progression into timed note events (in beats). Pure C++17, no JUCE.

#include "Progression.h"

namespace cb
{

enum class RhythmPattern
{
    Block = 0,   // whole chord held for the slot (gate removes a fixed tail of at most one beat)
    Strum,       // like Block, notes staggered upward by 0.03 beats each (less when the held length is short)
    Pulse,       // whole chord retriggered every rate step
    Offbeat,     // whole chord on the off-beat half of every beat (the "&"), length = gate * half a beat
    ArpUp,       // one note per rate step, low -> high, repeating
    ArpDown,     // high -> low
    ArpUpDown,   // low -> high -> low without repeating the turnaround notes (1 2 3 4 3 2 | 1 2 ...)
    Broken,      // lowest note on each beat, remaining notes together on the following rate step(s) within the beat
    NumPatterns
};

constexpr int numRhythmPatterns = (int) RhythmPattern::NumPatterns;
const char* rhythmPatternName (RhythmPattern);   // "Block", "Strum", "Pulse", "Offbeat", "Arp Up", "Arp Down", "Arp Up/Down", "Broken"

enum class BassMode
{
    Off = 0,     // no bass events
    Root,        // chord root held for the slot
    RootFifth,   // bass and the chord's fifth (b5 / #5 for dim / aug) alternating each beat; slash chords alternate bass and root
    Octaves,     // bass alternating with an upper/lower octave every rate step
    Pulse,       // root retriggered every rate step
    NumModes
};

constexpr int numBassModes = (int) BassMode::NumModes;
const char* bassModeName (BassMode);   // "Off", "Root", "Root + 5th", "Octaves", "Pulse"

enum class RateDivision
{
    Quarter = 0,    // 1 beat
    Eighth,         // 1/2 beat
    Sixteenth,      // 1/4 beat
    EighthTriplet,  // 1/3 beat
    NumRates
};

constexpr int numRateDivisions = (int) RateDivision::NumRates;
const char* rateDivisionName (RateDivision);   // "1/4", "1/8", "1/16", "1/8T"
double rateInBeats (RateDivision);

struct RenderSettings
{
    RhythmPattern pattern = RhythmPattern::Block;
    RateDivision rate = RateDivision::Eighth;
    BassMode bass = BassMode::Root;
    float velocity = 0.75f;   // 0..1, base velocity for chord notes (bass uses the same)
    float gate = 0.9f;        // 0.05..1, step fraction; held notes release at length - (1-gate)*min(length,1)
    float swing = 0.0f;       // 0..1, delays every second rate step by swing * stepLength / 3 (1.0 = triplet shuffle)
    float humanize = 0.0f;    // 0..1, deterministic jitter (hash of slot/step/note, never std::rand):
                              //   timing up to +-0.015 * humanize beats (never before 0, never across the slot end),
                              //   velocity up to +-0.12 * humanize
};

struct NoteEvent
{
    int note = 60;              // MIDI note 0..127
    float velocity = 0.75f;     // 0..1 (never 0)
    double startBeat = 0.0;     // from the start of the progression
    double lengthBeats = 1.0;   // > 0
    int channel = 1;            // 1 = chords, 2 = bass (MIDI channels)

    bool operator== (const NoteEvent& o) const
    {
        return note == o.note && velocity == o.velocity && startBeat == o.startBeat
            && lengthBeats == o.lengthBeats && channel == o.channel;
    }
};

// Renders one loop of the progression. voicings[i] are the notes for progression.slots[i] (as from voiceProgression).
// Bass uses the lowest voicing pitch class for explicit inversions, otherwise the chord root.
// Missing voicings use the requested inversion's chord tone and a chord floor of MIDI 60.
// Bass octaves are chosen in [28,55]: first by separation from the lowest chord note (at least 5 semitones below,
// then below, then a unison, then above), then by |movement from the previous bass note (first: MIDI 40)| plus
// 0.5 * |note - 41|, which keeps the line voice-led around MIDI ~36-50 instead of drifting low (C G Am F -> 36 43 45 41);
// ties go lower, and above the chord the lowest note wins. Only when no note in [28,55] is below the chord may
// C1..D#1 (24..27) be used.
// RootFifth alternates the slot's bass note with the fifth (slash chords: the chord root) chosen by the same rule.
// Octaves keeps its entry note, alternating with an upper/lower octave within range. The last played bass note leads
// into the next slot.
// Block, Strum and Root bass release at slotLength - (1-gate)*min(slotLength,1 beat).
// Strum notes share that release; their 0.03-beat stagger shrinks to held / (2 * (notes - 1)) when needed, so every
// note sounds for at least half the held length. No event extends past its slot; sorted by (startBeat, channel, note).
std::vector<NoteEvent> renderProgression (const Progression&, const std::vector<std::vector<int>>& voicings,
                                          const RenderSettings&);

} // namespace cb
