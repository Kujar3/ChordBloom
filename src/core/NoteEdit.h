#pragma once

// Phase 2 roll editing: pure operations on a note list and a selection, used by MelodyRoll and unit-tested.
// A selection is a list of indices into the input's notes; invalid and duplicate indices are ignored. Every operation
// returns the edited list sanitised for the given limits plus the selection re-identified in it: the notes the
// operation kept selected, moved or produced, found again by (pitch, start) after sanitising (canonical notes never
// share both; when two collapsed into one, the survivor - the longer note - is selected), ascending.
// Pure C++17, no JUCE, deterministic.

#include "Melody.h"

namespace cb
{

struct NoteEditResult
{
    Melody notes;
    std::vector<int> selection;
};

struct NoteEditContext
{
    const Progression* progression = nullptr;   // chords for snapping; nullptr = no chords
    Key key;
    MelodySnap snap = MelodySnap::Off;
    NoteLimits limits;
    double maxBeat = kMaxMelodyBeat;            // starts stay in [0, maxBeat - kMinMelodyNoteBeats]
};

std::vector<int> validSelection (const Melody&, const std::vector<int>& selection);   // sorted, unique, in range
std::vector<int> allNoteIndices (const Melody&);
// Notes overlapping [beatFrom, beatTo) whose pitch is in [pitchLow, pitchHigh] (the marquee). Both ranges are
// normalised first (swapped when reversed), so a leftward or downward marquee works the same.
std::vector<int> notesInArea (const Melody&, double beatFrom, double beatTo, int pitchLow, int pitchHigh);
std::vector<int> notesStartingIn (const Melody&, double beatFrom, double beatTo);    // start in [beatFrom, beatTo)
// Matched by (pitch, start); ascending unique indices; wanted notes that are not found are skipped.
std::vector<int> findNotes (const Melody&, const std::vector<MelodyNote>& wanted);

// Group move. deltaBeats (already grid-rounded by the caller) and deltaSemitones are each clamped once for the whole
// group so every selected note stays inside [0, maxBeat - kMinMelodyNoteBeats] and the pitch limits (individual notes
// are never folded); then, unless snap is Off, each moved note is re-snapped (snapMelodyPitch with the limits) against
// the chord at its new start. A group whose clamped (deltaBeats, deltaSemitones) is (0, 0) comes back unchanged
// (sanitised, selection kept) without any re-snap; with a clamped deltaBeats of 0, only notes whose pitch moved are
// re-snapped (a click or a sub-grid drag never changes an off-snap note).
NoteEditResult moveNotes (const Melody&, const std::vector<int>& selection, double deltaBeats, int deltaSemitones,
                          const NoteEditContext&);

// Every selected note's length changes by deltaBeats, each clamped to [minLength, kMaxMelodyNoteBeats] and to end at
// kMaxMelodyBeat at the latest.
NoteEditResult resizeNotes (const Melody&, const std::vector<int>& selection, double deltaBeats, double minLength,
                            const NoteLimits&);

// Offset of the Duplicate command: the selection's span (earliest start to latest end) rounded up to `grid`; when that
// is >= 1 beat, rounded up to whole beats; when that is > 2 beats, rounded up to whole bars (multiples of 4). At least
// one grid step. Empty selection: 4. Examples at grid 1/4: span 0.5 -> 0.5, 1.5 -> 2, 3.5 -> 4, 5 -> 8.
// grid <= 0: no grid rounding (the beat / bar rounding still applies), at least kMelodyTick.
double defaultDuplicateOffset (const Melody&, const std::vector<int>& selection, double grid);

// Copies of the selected notes shifted by offsetBeats (pitches unchanged); the copies are the new selection. Copies
// that would start at or after kMaxMelodyBeat are dropped.
NoteEditResult duplicateNotes (const Melody&, const std::vector<int>& selection, double offsetBeats, const NoteLimits&);

NoteEditResult deleteNotes (const Melody&, const std::vector<int>& selection, const NoteLimits&);   // selection empty

NoteEditResult setNoteVelocity (const Melody&, const std::vector<int>& selection, float velocity, const NoteLimits&);
NoteEditResult offsetNoteVelocity (const Melody&, const std::vector<int>& selection, float delta, const NoteLimits&);
// (velocities always clamped to [0.05, 1])

// Transposition by semitones, or by scale steps of `key` (a scale tone moves to the scale tone `steps` away; a
// non-scale tone first goes to its nearest scale tone in the direction of travel, and that counts as the first step:
// C# +1 in C major -> D, C# +2 -> E). Refused (the input comes back sanitised with the selection kept) when any
// selected note would leave the limits.
NoteEditResult transposeNotes (const Melody&, const std::vector<int>& selection, int semitones, const NoteLimits&);
NoteEditResult transposeNotesInScale (const Melody&, const std::vector<int>& selection, int steps, const Key&,
                                      const NoteLimits&);

// Starts rounded to the nearest grid line, lengths kept (grid <= 0: unchanged). Legato: each selected note ends where
// the next strictly later onset of any note in the list (selected or not, any pitch) begins, capped at
// kMaxMelodyNoteBeats; a note with no later onset keeps its length.
NoteEditResult quantizeNotes (const Melody&, const std::vector<int>& selection, double grid, const NoteLimits&);
NoteEditResult legatoNotes (const Melody&, const std::vector<int>& selection, const NoteLimits&);

// Clipboard. copyNotes: the selected notes with starts relative to sourceBeat = the first selected start floored to
// `grid` (grid <= 0: not floored). pasteNotes: the clip inserted at atBeat, pitches octave-folded into the limits,
// notes at or after kMaxMelodyBeat dropped; the pasted notes are the new selection.
struct NoteClip
{
    Melody notes;
    double sourceBeat = 0.0;
    bool empty() const { return notes.empty(); }
};
NoteClip copyNotes (const Melody&, const std::vector<int>& selection, double grid);
NoteEditResult pasteNotes (const Melody&, const NoteClip&, double atBeat, const NoteLimits&);

} // namespace cb
