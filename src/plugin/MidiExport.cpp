#include "plugin/MidiExport.h"

#include <algorithm>
#include <cmath>

namespace cb::midiexport
{
namespace
{
struct TimedNote
{
    double tick = 0.0;
    bool on = false;
    int channel = 1;
    int note = 60;
    juce::uint8 velocity = 0;
    int order = 0;
};
} // namespace

juce::MidiFile buildMidiFile (const std::vector<NoteEvent>& events, double bpm,
                              bool includeChords, bool includeBass, int ticksPerQuarterNote)
{
    const int tpq = juce::jlimit (24, 32767, ticksPerQuarterNote);

    if (! std::isfinite (bpm) || bpm <= 0.0)
        bpm = 120.0;
    bpm = juce::jlimit (10.0, 999.0, bpm);

    juce::String trackName = "Chordbloom";
    if (includeChords && includeBass)
        trackName << " Chords + Bass";
    else if (includeChords)
        trackName << " Chords";
    else if (includeBass)
        trackName << " Bass";

    // Track 0: conductor (name, 4/4, tempo)
    juce::MidiMessageSequence tempoTrack;
    tempoTrack.addEvent (juce::MidiMessage::textMetaEvent (3, "Chordbloom"), 0.0);
    tempoTrack.addEvent (juce::MidiMessage::timeSignatureMetaEvent (4, 4), 0.0);
    tempoTrack.addEvent (juce::MidiMessage::tempoMetaEvent (juce::roundToInt (60000000.0 / bpm)), 0.0);

    // Track 1: notes. Collected first so note-offs sort before note-ons on the same tick.
    std::vector<TimedNote> timed;
    timed.reserve (events.size() * 2);
    double lastTick = 0.0;
    int order = 0;

    for (const auto& e : events)
    {
        const bool isBass = e.channel == 2;
        if (isBass ? ! includeBass : ! includeChords)
            continue;

        if (! std::isfinite (e.startBeat) || ! std::isfinite (e.lengthBeats) || e.note < 0 || e.note > 127)
            continue;

        const double startTick = std::round (juce::jmax (0.0, e.startBeat) * tpq);
        double endTick = std::round ((juce::jmax (0.0, e.startBeat) + juce::jmax (0.0, e.lengthBeats)) * tpq);
        if (endTick <= startTick)
            endTick = startTick + 1.0;

        const int channel = juce::jlimit (1, 16, e.channel);
        const float v = std::isfinite (e.velocity) ? e.velocity : 0.75f;
        const auto velocity = (juce::uint8) juce::jlimit (1, 127, juce::roundToInt (v * 127.0f));

        timed.push_back ({ startTick, true, channel, e.note, velocity, order++ });
        timed.push_back ({ endTick, false, channel, e.note, 0, order++ });
        lastTick = juce::jmax (lastTick, endTick);
    }

    std::sort (timed.begin(), timed.end(), [] (const TimedNote& a, const TimedNote& b)
    {
        if (a.tick != b.tick)
            return a.tick < b.tick;
        if (a.on != b.on)
            return ! a.on;   // offs first
        return a.order < b.order;
    });

    juce::MidiMessageSequence noteTrack;
    noteTrack.addEvent (juce::MidiMessage::textMetaEvent (3, trackName), 0.0);

    for (const auto& t : timed)
    {
        if (t.on)
            noteTrack.addEvent (juce::MidiMessage::noteOn (t.channel, t.note, t.velocity), t.tick);
        else
            noteTrack.addEvent (juce::MidiMessage::noteOff (t.channel, t.note), t.tick);
    }
    noteTrack.updateMatchedPairs();

    // Both tracks end at the last note-off rounded up to a whole beat (progression slots are whole beats).
    const double endTick = std::ceil (lastTick / tpq - 1.0e-9) * tpq;
    tempoTrack.addEvent (juce::MidiMessage::endOfTrack(), juce::jmax (0.0, endTick));
    noteTrack.addEvent (juce::MidiMessage::endOfTrack(), juce::jmax (lastTick, endTick));

    juce::MidiFile file;
    file.setTicksPerQuarterNote (tpq);
    file.addTrack (tempoTrack);
    file.addTrack (noteTrack);
    return file;
}

bool writeMidiFile (const juce::MidiFile& midiFile, const juce::File& destination)
{
    if (destination == juce::File() || destination.isDirectory())
        return false;

    if (! destination.getParentDirectory().createDirectory().wasOk())
        return false;

    juce::MemoryOutputStream out;
    if (! midiFile.writeTo (out, 1))
        return false;

    // replaceWithData writes a temporary sibling and moves it into place, so readers never see half a file.
    return destination.replaceWithData (out.getData(), out.getDataSize());
}
} // namespace cb::midiexport
