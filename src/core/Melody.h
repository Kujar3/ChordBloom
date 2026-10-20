#pragma once

// The melody drawn in the "harmonic guide" piano roll, plus the music-theory helpers the roll and the renderers use.
// Pure C++17, no JUCE. May allocate: never call from the audio thread.

#include "Pattern.h"

#include <optional>

namespace cb
{

//==============================================================================
struct MelodyNote
{
    int note = 72;              // MIDI note, kMelodyLowestNote..kMelodyHighestNote
    double startBeat = 0.0;     // quarter-note beats from the start of the progression, 0..kMaxMelodyBeat
    double lengthBeats = 1.0;   // kMinMelodyNoteBeats..kMaxMelodyNoteBeats
    float velocity = 0.8f;      // 0.05..1

    bool operator== (const MelodyNote& o) const
    {
        return note == o.note && startBeat == o.startBeat && lengthBeats == o.lengthBeats && velocity == o.velocity;
    }
    bool operator!= (const MelodyNote& o) const { return ! (*this == o); }
};

constexpr int kMelodyLowestNote = 36;                  // C2 (C3 in FL Studio's naming)
constexpr int kMelodyHighestNote = 96;                 // C7 (C8 in FL Studio's naming)
constexpr int kMaxMelodyNotes = 512;
constexpr double kMelodyTick = 1.0 / 48.0;             // time resolution: 1/16 notes and 1/8 / 1/16 triplets are exact
constexpr double kMinMelodyNoteBeats = 1.0 / 16.0;     // a 1/64 note
constexpr double kMaxMelodyNoteBeats = 64.0;
constexpr double kMaxMelodyBeat = 256.0;               // kMaxSlots * kMaxSlotBeats: the longest possible progression
constexpr float kDefaultMelodyVelocity = 0.8f;

struct Melody
{
    std::vector<MelodyNote> notes;   // canonical order: sorted by (startBeat, note, lengthBeats); at most kMaxMelodyNotes

    bool empty() const { return notes.empty(); }

    bool operator== (const Melody& o) const { return notes == o.notes; }
    bool operator!= (const Melody& o) const { return ! (*this == o); }
};

// Canonical form of a melody (everything the processor stores goes through this):
//  - notes with a non-finite start or length are dropped
//  - pitch clamped to [kMelodyLowestNote, kMelodyHighestNote]; velocity clamped to [0.05, 1] (non-finite -> default)
//  - start rounded to a multiple of kMelodyTick and clamped to [0, kMaxMelodyBeat - kMinMelodyNoteBeats]
//  - length rounded to the nearest multiple of kMelodyTick (never below kMinMelodyNoteBeats) and clamped to
//    [kMinMelodyNoteBeats, kMaxMelodyNoteBeats] and to the end at kMaxMelodyBeat
//  - sorted by (startBeat, note, lengthBeats); of several notes with the same pitch and start only the longest stays
//  - truncated to the first kMaxMelodyNotes notes
// Idempotent: sanitiseMelody (sanitiseMelody (m)) == sanitiseMelody (m).
Melody sanitiseMelody (const Melody&);

// Index of the note under (beat, pitch): a note of that pitch with startBeat <= beat < startBeat + lengthBeats.
// With overlapping candidates the one that starts last wins (it is drawn on top). -1 when there is none.
int melodyNoteAt (const Melody&, double beat, int pitch);

//==============================================================================
// Harmonic guide

enum class ToneRole
{
    Outside = 0,   // neither in the key's scale nor in the chord
    Scale,         // a scale tone of the key that is not in the chord
    ChordTone,     // a tone of the chord (extensions included) other than its root, in or out of the scale
    ChordRoot      // the chord's root
};

// What a pitch class is against the key and the chord sounding there (nullptr = no chord: Scale or Outside only).
// A chord tone outside the scale (G# of E7 in A minor) is a ChordTone.
ToneRole toneRole (int pitchClass, const Key&, const Chord* chord);

// The chord of the progression slot that contains `beat` (Progression::slotAtBeat), nullopt outside the progression.
std::optional<Chord> chordAtBeat (const Progression&, double beat);

//==============================================================================
// Snapping

enum class MelodySnap
{
    Off = 0,      // any pitch
    Scale,        // scale tones of the key, plus the tones of the chord at that point (E7's G# in A minor)
    ChordTones,   // tones of the chord at that point; scale tones where no chord sounds
    NumModes
};

constexpr int numMelodySnapModes = (int) MelodySnap::NumModes;
const char* melodySnapName (MelodySnap);   // "Off", "Scale", "Chord"

// The allowed MIDI note nearest to `note` for the snap mode and the chord at the note's position (ties: the lower
// note), always inside [kMelodyLowestNote, kMelodyHighestNote]. MelodySnap::Off only clamps to that range.
int snapMelodyPitch (int note, MelodySnap, const Key&, const std::optional<Chord>& chord);

// Grid snapping for the roll. grid > 0 in beats (rateInBeats of the roll's grid). roundToNearest = true rounds to
// the nearest grid line (moves, resizes), false takes the grid line at or before `beat` (the cell under the mouse,
// for drawing). Values within 1e-6 beats of a grid line count as on it. grid <= 0 returns `beat` unchanged.
double snapBeatToGrid (double beat, double grid, bool roundToNearest);

//==============================================================================
// Rendering & structural edits

// One loop of melody playback / export: the notes that start in [0, loopBeats), each ending at the latest at
// loopBeats, as NoteEvents on MIDI channel `channel` (the processor uses 3), sorted by (startBeat, note).
// A note that overlaps a later note of the same pitch is shortened to end where that note starts (a clean
// retrigger, never two overlapping notes of one pitch). Notes at or after loopBeats are left out ("parked": they
// stay in the melody and sound again once the progression is long enough). Empty when loopBeats <= 0.
std::vector<NoteEvent> renderMelody (const Melody&, double loopBeats, int channel = 3);

// Transposes every note by the same pitch-class move as transposing chord roots by `semitones`, taking the smaller
// way round: +7 moves notes down 5 semitones, -7 up 5, +6 / -6 up 6. A note that would leave the melody range moves
// by an octave less instead. Result is sanitised.
Melody transposeMelody (const Melody&, int semitones);

// Keeps the melody on its chords after a structural edit of the progression (insert, remove, move, duplicate,
// length change). sourceSlot[i] is the index in `before` of the slot that became after.slots[i], or -1 for a new
// slot (which starts without notes); sourceSlot.size() must equal after.slots.size(), otherwise the melody is
// returned sanitised but unmoved.
//  - Each note belongs to the slot of `before` that contains its start and keeps its offset into that slot, so it
//    moves with the slot. A slot that appears twice in sourceSlot (duplicate) gives both copies its notes.
//  - Notes of removed slots are dropped, and so are notes whose offset no longer fits their slot's new length.
//    Lengths are kept (a note may still ring over the next chord, as before the edit).
//  - Notes at or after before.totalBeats() (past the end of the old loop) keep their distance to the loop end.
// Result is sanitised.
Melody melodyFollowingSlots (const Melody&, const Progression& before, const Progression& after,
                             const std::vector<int>& sourceSlot);

} // namespace cb
