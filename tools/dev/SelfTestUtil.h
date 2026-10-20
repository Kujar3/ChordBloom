#pragma once

// Shared helpers of the ChordbloomDev selftest (owner: WP0, frozen; final-spec 8.2). Moved unchanged from
// tools/dev/SelfTest.cpp into namespace cbtest as inline functions / C++17 inline variables, so every per-WP selftest
// file (SelfTest.cpp legacy, SelfTestEngine.cpp WP4, SelfTestRoll.cpp WP5, SelfTestShell.cpp WP6) shares one
// pass / fail count and one set of rigs. Add new shared helpers through the coordinator only.

#include "plugin/MidiExport.h"
#include "plugin/PluginProcessor.h"
#include "plugin/PreviewSynth.h"
#include "ui/ChordPaletteView.h"
#include "ui/ControlsPanel.h"
#include "ui/ExportPanel.h"
#include "ui/HeaderBar.h"
#include "ui/KeyboardGuide.h"
#include "ui/MelodyRoll.h"
#include "ui/PluginEditor.h"
#include "ui/ProgressionLane.h"
#include "ui/RollToolbar.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace juce::detail
{
bool dispatchNextMessageOnSystemQueue (bool returnIfNoPendingMessages);   // juce_events (Windows / Linux)
}

namespace cbtest
{
inline int numPassed = 0;
inline int numFailed = 0;

inline void check (bool ok, const std::string& name, const std::string& detail = {})
{
    if (ok)
    {
        ++numPassed;
        std::cout << "PASS " << name << "\n";
    }
    else
    {
        ++numFailed;
        std::cout << "FAIL " << name << (detail.empty() ? std::string() : ": " + detail) << "\n";
    }
}

template <typename... Args>
inline std::string str (Args&&... args)
{
    std::ostringstream ss;
    (ss << ... << args);
    return ss.str();
}

inline void pumpMessages (int milliseconds)
{
    const double end = juce::Time::getMillisecondCounterHiRes() + milliseconds;
    for (;;)
    {
        while (juce::detail::dispatchNextMessageOnSystemQueue (true)) {}
        if (juce::Time::getMillisecondCounterHiRes() >= end)
            break;
        juce::Thread::sleep (2);
    }
}

inline void setParam (ChordbloomProcessor& p, const char* id, float plainValue)
{
    if (auto* param = p.getAPVTS().getParameter (id))
        param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

inline float getParam (ChordbloomProcessor& p, const char* id)
{
    if (auto* v = p.getAPVTS().getRawParameterValue (id))
        return v->load();
    return std::nanf ("");
}

inline std::string notesToString (const std::vector<int>& notes)
{
    std::string s = "[";
    for (size_t i = 0; i < notes.size(); ++i)
        s += (i > 0 ? " " : "") + std::to_string (notes[i]);
    return s + "]";
}

inline cb::Progression makeProgression (std::initializer_list<cb::Chord> chords, int lengthBeats = 4)
{
    cb::Progression p;
    for (auto& c : chords)
    {
        cb::ProgressionSlot s;
        s.chord = c;
        s.lengthBeats = lengthBeats;
        p.slots.push_back (s);
    }
    return p;
}

inline const cb::Chord C_ { 0, cb::ChordQuality::Major };
inline const cb::Chord Am_ { 9, cb::ChordQuality::Minor };
inline const cb::Chord F_ { 5, cb::ChordQuality::Major };
inline const cb::Chord G_ { 7, cb::ChordQuality::Major };
inline const cb::Chord Em_ { 4, cb::ChordQuality::Minor };

//==============================================================================
struct FakePlayHead final : juce::AudioPlayHead
{
    bool playing = false;
    bool hasInfo = true;
    double bpm = 120.0;
    double ppq = 0.0;
    bool reportLoop = false;            // report isLooping + loop points (some hosts do, FL's pattern mode may not)
    double loopStart = 0.0, loopEnd = 0.0;

    juce::Optional<PositionInfo> getPosition() const override
    {
        if (! hasInfo)
            return {};

        PositionInfo info;
        info.setIsPlaying (playing);
        info.setBpm (bpm);
        info.setPpqPosition (ppq);
        info.setTimeSignature (TimeSignature { 4, 4 });
        if (reportLoop)
        {
            info.setIsLooping (true);
            info.setLoopPoints (LoopPoints { loopStart, loopEnd });
        }
        return info;
    }
};

struct Ev
{
    juce::int64 sample = 0;
    bool on = false;
    int channel = 1;
    int note = 0;
    int velocity = 0;
};

// Drives a processor block by block, recording every MIDI event it outputs and validating the stream.
struct Rig
{
    explicit Rig (ChordbloomProcessor& proc, double sampleRate = 48000.0, int blockSize = 512, bool usePlayHead = true)
        : p (proc), sr (sampleRate), blockSize (blockSize)
    {
        p.setRateAndBufferSizeDetails (sr, blockSize);
        p.prepareToPlay (sr, blockSize);
        p.setPlayHead (usePlayHead ? &head : nullptr);
        buffer.setSize (2, blockSize);
    }

    ~Rig()
    {
        p.setPlayHead (nullptr);
        p.releaseResources();
    }

    double samplesPerBeat() const { return sr * 60.0 / head.bpm; }

    std::vector<Ev> processBlock (const juce::MidiBuffer& input = {})
    {
        // Fill with garbage: the processor must clear the output before the synth adds to it.
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), 0.5f, blockSize);

        juce::MidiBuffer midi (input);
        if (bypassed)
            p.processBlockBypassed (buffer, midi);
        else
            p.processBlock (buffer, midi);

        lastOut = midi;
        auto out = record (midi);

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const float* x = buffer.getReadPointer (ch);
            for (int i = 0; i < blockSize; ++i)
            {
                if (! std::isfinite (x[i]))
                    audioFinite = false;
                else
                    peak = juce::jmax (peak, std::abs (x[i]));
            }
        }

        samplePos += blockSize;
        if (head.playing)
        {
            head.ppq += blockSize * head.bpm / 60.0 / sr;
            if (hostLoopLen > 0.0 && head.ppq >= hostLoopStart + hostLoopLen)
                head.ppq -= hostLoopLen;   // host loop that does not split the block: the overshoot carries over
        }
        return out;
    }

    // A 0-sample processBlock call (VST3 parameter flush), optionally with a zeroed play head.
    std::vector<Ev> processZero (const juce::MidiBuffer& input = {})
    {
        juce::AudioBuffer<float> empty (2, 0);
        juce::MidiBuffer midi (input);
        p.processBlock (empty, midi);
        return record (midi);
    }

    std::vector<Ev> record (const juce::MidiBuffer& midi)
    {
        std::vector<Ev> out;
        lastOthers.clear();
        for (const auto meta : midi)
        {
            const auto msg = meta.getMessage();
            if (! (msg.isNoteOn() || msg.isNoteOff()))
            {
                ++numOtherEvents;
                lastOthers.push_back (msg);
                if (msg.isController() && msg.getControllerNumber() == 64)
                    pedal[msg.getChannel()] = msg.getControllerValue() >= 64;
                else if (msg.isController() && msg.getControllerNumber() == 121)
                    pedal[msg.getChannel()] = false;
                continue;
            }

            Ev e;
            e.sample = samplePos + meta.samplePosition;
            e.on = msg.isNoteOn();
            e.channel = msg.getChannel();
            e.note = msg.getNoteNumber();
            e.velocity = msg.getVelocity();
            out.push_back (e);

            const int key = e.channel * 128 + e.note;
            if (e.on)
            {
                if (open[key] > 0)
                    ++doubleOns;
                open[key] = 1;
            }
            else
            {
                if (open[key] == 0)
                    ++strayOffs;
                open[key] = 0;
            }
        }

        all.insert (all.end(), out.begin(), out.end());
        return out;
    }

    std::vector<Ev> run (int numBlocks)
    {
        std::vector<Ev> out;
        for (int i = 0; i < numBlocks; ++i)
        {
            auto evs = processBlock();
            out.insert (out.end(), evs.begin(), evs.end());
        }
        return out;
    }

    float blockPeak() const
    {
        float m = 0.0f;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            m = juce::jmax (m, buffer.getMagnitude (ch, 0, blockSize));
        return m;
    }

    int openCount() const
    {
        int n = 0;
        for (auto& kv : open)
            n += kv.second > 0 ? 1 : 0;
        return n;
    }

    int pedalsDown() const
    {
        int n = 0;
        for (auto& kv : pedal)
            n += kv.second ? 1 : 0;
        return n;
    }

    std::set<int> openNotes (int channel) const
    {
        std::set<int> s;
        for (auto& kv : open)
            if (kv.second > 0 && kv.first / 128 == channel)
                s.insert (kv.first % 128);
        return s;
    }

    ChordbloomProcessor& p;
    FakePlayHead head;
    double sr;
    int blockSize;
    juce::AudioBuffer<float> buffer;
    juce::int64 samplePos = 0;
    std::vector<Ev> all;
    std::map<int, int> open;
    int doubleOns = 0, strayOffs = 0, numOtherEvents = 0;
    std::vector<juce::MidiMessage> lastOthers;   // non-note messages of the last block
    juce::MidiBuffer lastOut;                     // everything the last processBlock put out, in order
    std::map<int, bool> pedal;                    // sustain pedal state downstream, per channel
    bool audioFinite = true;
    float peak = 0.0f;
    bool bypassed = false;   // drive processBlockBypassed instead of processBlock
    double hostLoopStart = 0.0, hostLoopLen = 0.0;   // > 0: the fake host loops (ppq wraps back while playing)
};

inline const Ev* findEvent (const std::vector<Ev>& evs, bool on, int channel, int note, juce::int64 sample, int tolerance = 1)
{
    for (auto& e : evs)
        if (e.on == on && e.channel == channel && e.note == note && std::abs (e.sample - sample) <= tolerance)
            return &e;
    return nullptr;
}

inline int countOns (const std::vector<Ev>& evs, juce::int64 from = 0, juce::int64 to = std::numeric_limits<juce::int64>::max(),
              int channel = 0)
{
    int n = 0;
    for (auto& e : evs)
        if (e.on && e.sample >= from && e.sample < to && (channel == 0 || e.channel == channel))
            ++n;
    return n;
}

inline std::set<int> onNotesAt (const std::vector<Ev>& evs, int channel, juce::int64 sample, int tolerance = 1)
{
    std::set<int> s;
    for (auto& e : evs)
        if (e.on && e.channel == channel && std::abs (e.sample - sample) <= tolerance)
            s.insert (e.note);
    return s;
}

inline juce::MidiBuffer noteOn (int note, int sample = 0, int channel = 1, int velocity = 100)
{
    juce::MidiBuffer b;
    b.addEvent (juce::MidiMessage::noteOn (channel, note, (juce::uint8) velocity), sample);
    return b;
}

inline juce::MidiBuffer noteOff (int note, int sample = 0, int channel = 1)
{
    juce::MidiBuffer b;
    b.addEvent (juce::MidiMessage::noteOff (channel, note), sample);
    return b;
}

inline std::set<int> pitchClassesOf (const std::set<int>& notes)
{
    std::set<int> s;
    for (int n : notes)
        s.insert (n % 12);
    return s;
}

inline std::set<int> pitchClassesOf (const cb::Chord& chord)
{
    std::set<int> s;
    for (int pcv : chord.pitchClasses())
        s.insert (((pcv % 12) + 12) % 12);
    return s;
}

struct ChangeCounter final : juce::ChangeListener
{
    int count = 0;
    void changeListenerCallback (juce::ChangeBroadcaster*) override { ++count; }
};

//==============================================================================
//==============================================================================
// Checks one rendered loop against the playback output.
inline void checkLoopAgainstRender (const std::vector<Ev>& evs, const std::vector<cb::NoteEvent>& expected, double samplesPerBeat,
                             double loopStartBeat, juce::int64 sampleOffset, const std::string& label)
{
    int missingOn = 0, missingOff = 0, badVelocity = 0;
    std::string firstMiss;
    for (auto& e : expected)
    {
        const auto onSample = sampleOffset + (juce::int64) std::floor ((loopStartBeat + e.startBeat) * samplesPerBeat);
        const auto offSample = sampleOffset + (juce::int64) std::floor ((loopStartBeat + e.startBeat + e.lengthBeats) * samplesPerBeat);
        const auto* on = findEvent (evs, true, e.channel, e.note, onSample);
        if (on == nullptr)
        {
            ++missingOn;
            if (firstMiss.empty())
                firstMiss = str ("on ch", e.channel, " n", e.note, " @", onSample);
        }
        else if (on->velocity != juce::jlimit (1, 127, juce::roundToInt (e.velocity * 127.0f)))
        {
            ++badVelocity;
        }

        if (findEvent (evs, false, e.channel, e.note, offSample) == nullptr)
        {
            ++missingOff;
            if (firstMiss.empty())
                firstMiss = str ("off ch", e.channel, " n", e.note, " @", offSample);
        }
    }

    check (missingOn == 0, label + ": every rendered note-on at its sample position", str (missingOn, " missing, first ", firstMiss));
    check (missingOff == 0, label + ": every rendered note-off at its sample position", str (missingOff, " missing, first ", firstMiss));
    check (badVelocity == 0, label + ": velocities match the render", str (badVelocity, " differ"));
}

inline juce::MouseEvent mouseEventFor (juce::Component& c, juce::Point<float> pos, juce::ModifierKeys mods, int clicks = 1,
                                bool dragged = false)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos, mods,
                             juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, pos, now,
                             clicks, dragged);
}

inline cb::MelodyNote melodyNote (int note, double start, double length, float velocity = cb::kDefaultMelodyVelocity)
{
    cb::MelodyNote n;
    n.note = note;
    n.startBeat = start;
    n.lengthBeats = length;
    n.velocity = velocity;
    return n;
}

inline cb::Melody makeMelody (std::initializer_list<cb::MelodyNote> notes)
{
    cb::Melody m;
    m.notes = notes;
    return cb::sanitiseMelody (m);
}

inline std::vector<double> melodyStarts (const cb::Melody& m)
{
    std::vector<double> v;
    for (auto& n : m.notes)
        v.push_back (n.startBeat);
    return v;
}

inline std::string startsToString (const std::vector<double>& v)
{
    std::ostringstream ss;
    ss << "[";
    for (size_t i = 0; i < v.size(); ++i)
        ss << (i > 0 ? " " : "") << v[i];
    ss << "]";
    return ss.str();
}

// The MIDI-out notes on `channel` between two samples.
inline std::vector<Ev> onChannel (const std::vector<Ev>& evs, int channel)
{
    std::vector<Ev> out;
    for (auto& e : evs)
        if (e.channel == channel)
            out.push_back (e);
    return out;
}

inline juce::Component* findComponentByTitle (juce::Component& root, const juce::String& title)
{
    if (root.getTitle() == title)
        return &root;

    for (auto* child : root.getChildren())
        if (auto* found = findComponentByTitle (*child, title))
            return found;

    return nullptr;
}

//==============================================================================
// Per-work-package selftests, registered in runSelfTest (SelfTest.cpp) after the legacy tests.
void runEngineSelfTests();   // tools/dev/SelfTestEngine.cpp (WP4)
void runRollSelfTests();     // tools/dev/SelfTestRoll.cpp (WP5)
void runShellSelfTests();    // tools/dev/SelfTestShell.cpp (WP6)
} // namespace cbtest
