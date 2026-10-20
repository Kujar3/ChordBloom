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
    // Phase 2 (appended; values are stored in projects, only ever append before NumModes). One fixed 16-step pattern
    // per 4/4 bar on the absolute grid from beat 0, re-pitched per slot, independent of Rate (see renderProgression).
    Offbeat8,         // "Offbeat 8ths"       --x---x---x---x-
    Rolling16,        // "Rolling 16ths"      xxxxxxxxxxxxxxxx
    RollingKickGap,   // "Rolling, kick gap"  -xox-xox-xox-xox   (o = the octave partner)
    OffbeatPairs,     // "Offbeat pairs"      --xx--xx--xx--xx
    Gallop,           // "Gallop"             x-xx-xx-x-xx-xx-
    OctaveBounce8,    // "Octave bounce"      x-o-x-o-x-o-x-o-
    Driving8,         // "Driving 8ths"       x-x-x-x-x-x-x-x-
    OffKick,          // "Off-kick"           -x-x--x--x-x--x-
    Tresillo,         // "Tresillo 3-3-2"     x--x--x-x--x--x-
    DottedQuarter,    // "Dotted quarters"    x-----x-----x---
    NumModes
};

constexpr int numBassModes = (int) BassMode::NumModes;
// bassModeName: "Off", "Root", "Root + 5th", "Octaves", "Pulse", then the names above.
const char* bassModeName (BassMode);

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

//==============================================================================
// (Phase 2) Bass pattern descriptions (renderProgression renders them; the UI names, groups and orders the modes; the
// tests use them as expectations). Every grid occurs in >= 20 corpus files or is a textbook idiom (GP section 2.2).
enum class BassRole { Root = 0, Octave };   // Octave = the Octaves mode's partner of the slot's root note

struct BassPatternStep
{
    int step = 0;              // 0..15, 16th of the bar
    float lengthSteps = 1.0f;  // in 16ths at gate 0.9 (scaled by gate / 0.9, cut at the next onset and the slot end)
    BassRole role = BassRole::Root;
};

bool isBassPattern (BassMode);                                     // Offbeat8 .. DottedQuarter
const std::vector<BassPatternStep>& bassPatternSteps (BassMode);   // ascending step; empty for Off and classic modes
std::string bassPatternGrid (BassMode);       // 16 chars: 'x' root, 'o' octave, '-' rest; "" for Off and classic modes
// The offbeat family (Offbeat8, RollingKickGap, OffbeatPairs, OffKick): no onset on steps 0, 4, 8, 12, and never a
// strike at a slot start. Every other pattern is "on-beat" (see renderProgression's slot-start strike).
bool bassPatternAvoidsKick (BassMode);
const char* bassModeFamily (BassMode);        // "" Off, "Held", "Follows Rate", "Trance", "Groove" (spec 3.2)
std::vector<BassMode> bassModeBrowseOrder();  // every mode except Off, in enum order (= family order)

// (Phase 2) The slot's bass pitch class exactly as renderProgression uses it: an explicit inversion
// (slot.inversion >= 0) takes the pitch class of the voicing's lowest note (notes outside 0..127 ignored; the
// inversion's chord tone when the voicing is empty); auto (-1) takes the chord root.
int slotBassPitchClass (const ProgressionSlot&, const std::vector<int>& voicing);

// (Phase 2) The swing grid shared by the auto bass patterns and the arp, so every layer swings with the chords:
// rate 1/8 -> 0.5, 1/16 -> 0.25, 1/4 -> 0.5 (the 8ths swing), 1/8T -> 0 (no swing).
double grooveGridBeats (const RenderSettings&);

// (Phase 2) Swung time of a straight time on the absolute grid. Inside each window [k * 2g, (k + 1) * 2g) the
// midpoint moves to k * 2g + g + swing * g / 3 (exactly where the chord renderer puts its swung odd steps, since slots
// start on whole beats) and the two halves map linearly. gridBeats <= 0 or swing <= 0: `beat` unchanged. swing is
// clamped to [0, 1]. Monotonic; window boundaries map to themselves.
double swingWarp (double beat, double gridBeats, double swing);

//==============================================================================
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
// (Phase 2) Pattern modes (isBassPattern) play bassPatternSteps on the absolute 16th grid: step g = round (beat * 4)
// from beat 0, pattern step g % 16, so the groove runs through chord changes; a partial final bar is truncated and the
// grid restarts with the loop. Per slot: R = the same chooser as every mode, bassRootNote (slot bass pc, lowest
// voicing note, previous) with previous = the pitch of the last bass note emitted before the slot (first: 40);
// O = the Octaves mode's partner of R (R + 12, or R - 12 when R + 12 > 55 or when R + 12 is not at least 5 below the
// lowest voicing note and R - 12 >= 28). Onset time = swingWarp (g / 4, grooveGridBeats (settings), swing) + the
// humanize jitter, kept inside the slot. Length = lengthSteps * 0.25 * gate / 0.9 beats, cut at the next bass onset's
// actual (swung, jittered) time and at the slot end. On-beat patterns: when no pattern onset lies in
// [slot start, slot start + 0.5 beat] (tested on the straight grid times g / 4, so swing never adds or removes a
// strike), R is struck at the slot start, lasting (first onset's actual time, or the slot end, - slot start)
// times min (1, gate / 0.9). The offbeat family never strikes at a slot start; it has an onset in every beat, so with
// whole-beat slots every slot gets at least one bass note in every mode. Velocity and humanize as for the other
// modes; Rate does not apply. Every bass note stays in [28, 55] (24..27 only below the chord) with the root or
// slash-bass pitch class. Off, Root, RootFifth, Octaves and Pulse are unchanged.
std::vector<NoteEvent> renderProgression (const Progression&, const std::vector<std::vector<int>>& voicings,
                                          const RenderSettings&);

} // namespace cb
