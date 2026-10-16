#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "core/Pattern.h"

namespace cb::midiexport
{
// Builds a type-1 MIDI file: track 0 = tempo + time signature (4/4) + track name, track 1 = the selected notes.
// Chord events keep channel 1, bass events channel 2. includeChords/includeBass filter by NoteEvent::channel.
juce::MidiFile buildMidiFile (const std::vector<NoteEvent>& events, double bpm,
                              bool includeChords, bool includeBass, int ticksPerQuarterNote = 960);

bool writeMidiFile (const juce::MidiFile&, const juce::File& destination);   // overwrites; creates parent dirs
}
