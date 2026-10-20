#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "core/Pattern.h"

namespace cb::midiexport
{
// Builds a type-1 MIDI file: track 0 = tempo + time signature (4/4) + track name, track 1 = the selected notes.
// Chord events keep channel 1, bass events channel 2. includeChords/includeBass filter by NoteEvent::channel.
// With both parts, each part gets its own named track ("Chordbloom Chords" = track 1, "Chordbloom Bass" = track 2;
// a part without notes is left out), so a DAW import puts chords and bass on separate instruments.
juce::MidiFile buildMidiFile (const std::vector<NoteEvent>& events, double bpm,
                              bool includeChords, bool includeBass, int ticksPerQuarterNote = 960);

// (Added with the melody roll.) Melody file: track 0 as above, track 1 "Chordbloom Melody" with every event of
// `melodyEvents` (as from cb::renderMelody) on its own channel (3), note-offs before note-ons on the same tick.
juce::MidiFile buildMelodyMidiFile (const std::vector<NoteEvent>& melodyEvents, double bpm, int ticksPerQuarterNote = 960);

bool writeMidiFile (const juce::MidiFile&, const juce::File& destination);   // overwrites; creates parent dirs
}
