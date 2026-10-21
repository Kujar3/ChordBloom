#pragma once

// Arp layer (phase 2): chord-following arpeggio shapes on their own MIDI channel. Pure C++17, no JUCE, deterministic
// (hashes only). Every shape and rhythm mask occurs in >= 20 corpus files or is a textbook idiom (GP section 3).
// May allocate: never call from the audio thread.

#include "Pattern.h"

namespace cb
{

constexpr int kArpLowestNote = 28;    // every arp note is octave-folded into [kArpLowestNote, kArpHighestNote]
constexpr int kArpHighestNote = 108;

enum class ArpShape   // append-only (stored through the arpShape parameter); enum order = menu order
{
    Up3 = 0,        // "Up 3"              0 1 2           free-running    group "Ladders"
    Down3,          // "Down 3"            0 2 1           free-running
    Up4,            // "Up 4"              0 1 2 0'
    Down4,          // "Down 4"            0' 2 1 0
    UpDown,         // "Up-down"           0 1 2 1
    Climb7,         // "1-3-5-7"           0 1 2 3         (triads: tone 3 = tone 0 + 12)
    TwoOctaves,     // "Two octaves"       0 1 2 0' 1' 2'
    Broken,         // "Broken 1-5-3-5"    0 2 1 2
    RootPulse,      // "Root pulse"        0                               group "Pedal & bounce"
    OctaveBounce,   // "Octave bounce"     0 0'
    RootHigh3,      // "Root + high 3rd"   0 1'
    RootHigh5,      // "Root + high 5th"   0 2'
    PedalOctave,    // "Pedal 1-3'-1-1'"   0 1' 0 0'
    PedalLow3,      // "Pedal 1-1-3'-1"    0 0 1' 0
    PedalLow5,      // "Pedal 1-1-5'-1"    0 0 2' 0
    Alternate13,    // "Alternate 1-3"     0 1                             group "Two-note"
    Alternate15,    // "Alternate 1-5"     0 2
    NumShapes
};
constexpr int numArpShapes = (int) ArpShape::NumShapes;
const char* arpShapeName (ArpShape);     // the names above
const char* arpShapeFamily (ArpShape);   // "Ladders" (Up3..Broken), "Pedal & bounce" (RootPulse..PedalLow5), "Two-note"

struct ArpTone
{
    int tone = 0;     // index into the slot's tone stack (0..3)
    int octave = 0;   // octaves above the cycle's base
    bool operator== (const ArpTone& o) const { return tone == o.tone && octave == o.octave; }
};
const std::vector<ArpTone>& arpShapeTones (ArpShape);   // one cycle, in order (table above; ' = octave 1)
bool arpShapeFreeRuns (ArpShape);                       // Up3, Down3 run across bar lines; all others restart per bar

enum class ArpRhythm   // append-only (arpRhythm parameter). Masks count arp steps from each bar line.
{
    EveryStep = 0,   // "Every step"
    ThreeOnOneOff,   // "3 on, 1 off"   x x x .   (stepInBar % 4)
    KickGap,         // "Kick gap"      . x x x
    Skip2nd,         // "Skip 2nd"      x . x x
    Skip3rd,         // "Skip 3rd"      x x . x
    Breath,          // "Breath"        the last step of every bar is silent
    LateStart,       // "Late start"    the first step of every bar is silent
    NumRhythms
};
constexpr int numArpRhythms = (int) ArpRhythm::NumRhythms;
const char* arpRhythmName (ArpRhythm);
bool arpStepSounds (ArpRhythm, int stepInBar, int stepsPerBar);   // the mask; every rhythm applies at every rate

struct ArpSettings
{
    ArpShape shape = ArpShape::Up3;
    RateDivision rate = RateDivision::Sixteenth;
    ArpRhythm rhythm = ArpRhythm::EveryStep;
    int centreNote = 65;   // register centre; the processor passes voicing centerNote + 5 + 12 * arpOctave; renderArp
                           // clamps it to 40..96 itself
    float gate = 0.9f;     // 0.05..1 (clamped): note length / arp step
};

// One loop of the arp on MIDI channel `channel`, sorted by (startBeat, note); empty for an empty progression.
//  Grid: steps k at k * rateInBeats (rate) from beat 0 (absolute, through chord changes); stepsPerBar = 4 / step
//   (4, 8, 16 or 12) and stepInBar = k % stepsPerBar. A step sounds when arpStepSounds says so; the cycle advances on
//   sounding steps only (a mask never skips a tone).
//  Cycle position: free-running shapes count sounding steps from beat 0 (the loop start, so the seam is deterministic);
//   the others count sounding steps since the last bar line (a beat that is a multiple of 4; a 5-beat slot restarts
//   at the bar line inside it; a partial final bar is cut and the next loop restarts at beat 0).
//  Tone stack per slot (from the chord, never the pad voicing): core = the first min (4, n) intervals of
//   qualityIntervals (quality) (5- and 6-tone chords keep their first 4). With an explicit inversion
//   (slot.inversion >= 0) core is rotated so tone 0 is the core tone whose pitch class is slotBassPitchClass (slot,
//   voicings[i]), its interval reduced mod 12; every following tone (in core order, wrapping) is raised by octaves
//   until it is above the tone before it (C/E -> E G C' = 4 7 12; Add9 over D -> D C' E' G' = 2 12 16 19). When that
//   pitch class is not in core there is no rotation. Tone i >= size = tone (i - size) + 12. Each step uses the
//   stack of the slot that contains it (a chord change mid-cycle re-maps the indices at the slot's first step).
//   A missing voicing (voicings.size() <= i) counts as an empty one (slotBassPitchClass then takes the inversion's
//   chord tone).
//  Base octave per slot: centreNote is clamped to [40, 96] here. The base (pitch of tone 0) that puts the cycle's mean
//   pitch nearest centreNote (ties lower); when a base within 6 semitones of the previous slot's final base (after
//   its bass clearance) keeps the mean within 7 of the centre, the closest such base wins (voice-led; equally close:
//   the lower). Bass clearance: when bassEvents has notes starting in the slot, the base moves up by octaves
//   until the cycle's lowest note is >= the lowest of those bass notes + 12, while the mean stays <= centreNote + 12.
//   Every note is then octave-folded into [kArpLowestNote, kArpHighestNote].
//  Time: start and end through swingWarp (t, grooveGridBeats (feel), feel.swing), except at rate 1/8T (straight);
//   humanize jitters timing only, as renderProgression (feel.humanize, hash of slot / step / channel), never before
//   the slot start or past its end. Length = max (warped end - warped start, 1/16 beat (kMinMelodyNoteBeats)) with the
//   straight end = step start + gate * step; then cut at the slot end and so a note ends at or before the next start
//   of the same pitch. A note is dropped only when those cuts leave less than 1e-4 beat, so every sounding step plays
//   at every gate, rate and swing (as the pattern bass). Velocity feel.velocity, flat (humanize never changes it).
std::vector<NoteEvent> renderArp (const Progression&, const std::vector<std::vector<int>>& voicings,
                                  const ArpSettings&, const RenderSettings& feel,
                                  const std::vector<NoteEvent>& bassEvents, int channel = 4);

} // namespace cb
