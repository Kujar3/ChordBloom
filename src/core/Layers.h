#pragma once

// Phase 2 note layers of the roll (Melody, Arp, Bass): identity, limits, the auto / custom state, auto renders as
// editable notes, and keeping custom notes on their chords. Pure C++17, no JUCE, deterministic. May allocate.

#include "Melody.h"

#include <optional>
#include <string>

namespace cb
{

enum class Layer
{
    Melody = 0,   // the user's lead (drawn or generated); MIDI channel 3; never auto
    Arp,          // renderArp until edited; MIDI channel 4
    Bass,         // renderProgression's bass until edited; MIDI channel 2
    NumLayers     // = the roll's tab order. Stored only as layerKey strings (state, properties), never as ints.
};
constexpr int numLayers = (int) Layer::NumLayers;

const char* layerName (Layer);                            // "Melody", "Arp", "Bass"
const char* layerKey (Layer);                             // "melody", "arp", "bass"
std::optional<Layer> layerFromKey (const std::string&);   // case-insensitive; nullopt for anything else
int layerMidiChannel (Layer);                             // 3, 4, 2
NoteLimits layerLimits (Layer);   // Melody kMelodyLimits; Arp { 28, 108, kMaxLayerNotes }; Bass { 24, 72, kMaxLayerNotes }

// Stored state of the Arp and Bass layers (the melody is always user notes and lives in cb::Melody).
//  custom == false: auto. The notes are rendered from the chords and the layer's parameters whenever either changes;
//                   `notes` is empty (the processor keeps it so).
//  custom == true:  the user edited the layer. `notes` (canonical for layerLimits) play instead of the render; they
//                   follow progression edits (layerNotesFollowing) and transposition (transposeMelody with the limits).
struct LayerState
{
    bool custom = false;
    Melody notes;

    bool operator== (const LayerState& o) const { return custom == o.custom && notes == o.notes; }
    bool operator!= (const LayerState& o) const { return ! (*this == o); }
};

// Rendered events (one loop, any channel) -> the notes the roll shows and edits: start and length rounded to
// kMelodyTick (length >= kMinMelodyNoteBeats), velocity kept, pitch octave-folded into `limits`, events with a
// non-finite time dropped; sanitised for `limits`. The processor feeds it a humanize-free render (swing kept), so the
// result never changes with Humanize.
Melody notesFromEvents (const std::vector<NoteEvent>&, const NoteLimits&);

// True when both progressions have the same number of slots with the same lengths (chords may differ).
bool sameStructure (const Progression&, const Progression&);

// The pitch with the same chord role in `to` that `note` has in `from`. fromBassPc / toBassPc = slotBassPitchClass of
// the two slots. Each chord's tones are taken in bass-rotated order: pitchClasses() rotated so that the bass pc comes
// first when it is a chord tone, otherwise unrotated (C/E: E G C; F/A: A C F; Cmaj7: C E G B). Roles, first match wins:
//  1. bass: pc (note) == fromBassPc -> toBassPc
//  2. tone k of `from` in its bass-rotated order -> tone k of `to` in its own bass-rotated order, or tone 0 of that
//     order when `to` has fewer tones. So an inversion keeps every chord tone: C/E -> Am maps E G C to A C E,
//     F/A -> C maps A C F to C E G, Cmaj7 -> Am maps the 7th to A
//  3. any other pitch keeps its interval above the root (to.root + interval); when that pitch class is neither in the
//     key's scale nor a tone of `to`, it moves to the nearest scale tone (ties down)
// The new pitch class goes to the octave nearest `note` (ties down), then is octave-folded into `limits`.
// from == to && fromBassPc == toBassPc: `note` folded into `limits`.
int remapPitchToChord (int note, const Chord& from, int fromBassPc, const Chord& to, int toBassPc, const Key&,
                       const NoteLimits&);

// Everything layerNotesFollowing needs to know about one progression edit.
struct LayerFollowContext
{
    Progression before, after;
    std::vector<int> sourceSlot;     // as melodyFollowingSlots: after-slot i came from before-slot sourceSlot[i]; -1 = new
    std::vector<int> beforeBassPc;   // slotBassPitchClass per slot of `before` (missing entries: the chord root)
    std::vector<int> afterBassPc;    // per slot of `after` (missing entries: the chord root)
    Key key;
    Melody autoAfter;                // the layer's auto notes (notesFromEvents form) for `after`, rendered with after's voicings
    Melody autoBefore;               // the layer's auto notes (notesFromEvents form) for `before`, with before's voicings
};

// The single follow function: keeps a layer's notes on their chords after any progression edit.
//  Melody: exactly melodyFollowingSlots (notes, before, after, sourceSlot) (no remap, no fill).
//  Arp / Bass:
//   0. untouched slots follow the auto pattern: an after-slot i with sourceSlot[i] >= 0 whose source slot is unedited
//      (the notes of `notes` that start in before-slot sourceSlot[i] equal the notes of autoBefore that start in it:
//      the editedSlots test) gets the notes of autoAfter that start in after-slot i instead of its moved notes. Steps
//      1-3b apply only to the notes of edited slots and to the notes past the old loop end;
//   1. every note moves with its slot exactly as melodyFollowingSlots moves melody notes, inside layerLimits (notes
//      past the old loop end keep their distance to it; notes of removed slots and notes that no longer fit their slot
//      are dropped; a duplicated slot copies its notes);
//   2. a moved note whose after-slot has another chord or another bass pitch class than its before-slot is remapped
//      with remapPitchToChord (before-slot chord and bass pc -> after-slot chord and bass pc); notes past the old loop
//      end are moved but never remapped;
//   3. every new slot (sourceSlot[i] == -1) gets the notes of autoAfter that start inside it;
//   3b. an edited slot that grew (after.slots[i].lengthBeats > before.slots[sourceSlot[i]].lengthBeats) also gets the
//      notes of autoAfter that start in its new tail [slot start + old length, slot end).
//  sourceSlot.size() != after.slots.size(): the notes come back sanitised for the layer but unmoved.
// Result sanitised for layerLimits (layer). A result equal to autoAfter means nothing is edited any more: the processor
// then makes the layer auto in the same step.
Melody layerNotesFollowing (const Melody& notes, Layer, const LayerFollowContext&);

// Per slot of the progression: true when the notes of `custom` that start in the slot differ from the notes of
// `autoNotes` that start in it (the chord strip's edited dot, "Edited 2/4"). Notes outside the loop are ignored.
std::vector<bool> editedSlots (const Melody& custom, const Melody& autoNotes, const Progression&);

// "Reset this chord": the notes of `custom` that start in `slot` replaced by the notes of `autoNotes` that start in it;
// everything else unchanged; sanitised for `limits`. An invalid slot returns `custom` sanitised.
Melody replaceSlotNotes (const Melody& custom, const Melody& autoNotes, const Progression&, int slot, const NoteLimits&);

} // namespace cb
