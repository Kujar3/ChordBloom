#include "plugin/MidiExport.h"

// STUB - to be implemented (owner: processor agent)

namespace cb::midiexport
{
juce::MidiFile buildMidiFile (const std::vector<NoteEvent>&, double, bool, bool, int ticksPerQuarterNote)
{
    juce::MidiFile file;
    file.setTicksPerQuarterNote (ticksPerQuarterNote);
    return file;
}

bool writeMidiFile (const juce::MidiFile&, const juce::File&) { return false; }
}
