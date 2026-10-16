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

// One note track: name + the events of one part (or both), note-offs sorted before note-ons on the same tick.
juce::MidiMessageSequence buildNoteTrack (const std::vector<NoteEvent>& events, int tpq, bool includeChords,
                                          bool includeBass, const juce::String& trackName, double& lastTick, int& numNotes)
{
    std::vector<TimedNote> timed;
    timed.reserve (events.size() * 2);
    int order = 0;
    numNotes = 0;

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
        ++numNotes;
    }

    std::sort (timed.begin(), timed.end(), [] (const TimedNote& a, const TimedNote& b)
    {
        if (a.tick != b.tick)
            return a.tick < b.tick;
        if (a.on != b.on)
            return ! a.on;   // offs first
        return a.order < b.order;
    });

    juce::MidiMessageSequence track;
    track.addEvent (juce::MidiMessage::textMetaEvent (3, trackName), 0.0);

    for (const auto& t : timed)
    {
        if (t.on)
            track.addEvent (juce::MidiMessage::noteOn (t.channel, t.note, t.velocity), t.tick);
        else
            track.addEvent (juce::MidiMessage::noteOff (t.channel, t.note), t.tick);
    }
    track.updateMatchedPairs();
    return track;
}
} // namespace

juce::MidiFile buildMidiFile (const std::vector<NoteEvent>& events, double bpm,
                              bool includeChords, bool includeBass, int ticksPerQuarterNote)
{
    const int tpq = juce::jlimit (24, 32767, ticksPerQuarterNote);

    if (! std::isfinite (bpm) || bpm <= 0.0)
        bpm = 120.0;
    bpm = juce::jlimit (10.0, 999.0, bpm);

    // Track 0: conductor (name, 4/4, tempo)
    juce::MidiMessageSequence tempoTrack;
    tempoTrack.addEvent (juce::MidiMessage::textMetaEvent (3, "Chordbloom"), 0.0);
    tempoTrack.addEvent (juce::MidiMessage::timeSignatureMetaEvent (4, 4), 0.0);
    tempoTrack.addEvent (juce::MidiMessage::tempoMetaEvent (juce::roundToInt (60000000.0 / bpm)), 0.0);

    // Note tracks. Both parts: one named track per part (chords, then bass), so a DAW import puts them on separate
    // instruments; a part without notes is left out (unless both are empty).
    std::vector<juce::MidiMessageSequence> noteTracks;
    double lastTick = 0.0;

    if (includeChords && includeBass)
    {
        int numChords = 0, numBass = 0;
        auto chords = buildNoteTrack (events, tpq, true, false, "Chordbloom Chords", lastTick, numChords);
        auto bass = buildNoteTrack (events, tpq, false, true, "Chordbloom Bass", lastTick, numBass);
        if (numChords > 0 || numBass == 0)
            noteTracks.push_back (std::move (chords));
        if (numBass > 0)
            noteTracks.push_back (std::move (bass));
    }
    else
    {
        juce::String trackName = "Chordbloom";
        if (includeChords)
            trackName << " Chords";
        else if (includeBass)
            trackName << " Bass";

        int numNotes = 0;
        noteTracks.push_back (buildNoteTrack (events, tpq, includeChords, includeBass, trackName, lastTick, numNotes));
    }

    // Every track ends at the last note-off rounded up to a whole beat (progression slots are whole beats).
    const double endTick = std::ceil (lastTick / tpq - 1.0e-9) * tpq;
    tempoTrack.addEvent (juce::MidiMessage::endOfTrack(), juce::jmax (0.0, endTick));

    juce::MidiFile file;
    file.setTicksPerQuarterNote (tpq);
    file.addTrack (tempoTrack);
    for (auto& track : noteTracks)
    {
        track.addEvent (juce::MidiMessage::endOfTrack(), juce::jmax (lastTick, endTick));
        file.addTrack (track);
    }
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
