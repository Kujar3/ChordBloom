#include "plugin/PluginProcessor.h"

#include "plugin/MidiExport.h"
#include "plugin/PreviewSynth.h"
#include "ui/PluginEditor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>

//==============================================================================
// ChordbloomProcessor implementation.
//
// Message thread: owns the progression, undo/redo, palette and all calls into src/core. Every change re-voices and
// re-renders one loop into an immutable EngineData and publishes it with an atomic pointer swap. Old EngineData
// objects live in a message-thread release pool and are freed only when the audio thread is provably not using them
// (single hazard pointer: `audioInUse`). Preview notes go through an SPSC AbstractFifo.
//
// Audio thread: never allocates (all tables are fixed-size members of Impl), never locks, never calls src/core.
// Every note generated in a block (preview, playback, transformed live input) is first collected as a "logical" event,
// sorted by (sample, order), then passed through a per-(channel, note) reference counter that guarantees a balanced
// note-on / note-off stream. Every active note is tracked, so transport stops, jumps, loop edits, mode switches,
// releaseResources and midiOut switching all produce the matching note-offs.
//==============================================================================

namespace
{
juce::StringArray namesFrom (int count, const std::function<const char* (int)>& nameOf)
{
    juce::StringArray names;
    for (int i = 0; i < count; ++i)
        names.add (nameOf (i));
    return names;
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    using namespace juce;
    namespace id = cb::ParamIDs;

    AudioProcessorValueTreeState::ParameterLayout layout;
    auto pid = [] (const char* s) { return ParameterID { s, 1 }; };

    layout.add (std::make_unique<AudioParameterChoice> (pid (id::keyRoot), "Key",
        StringArray { "C", "C#/Db", "D", "D#/Eb", "E", "F", "F#/Gb", "G", "G#/Ab", "A", "A#/Bb", "B" }, 0));
    layout.add (std::make_unique<AudioParameterChoice> (pid (id::scale), "Scale",
        namesFrom (cb::numScaleTypes, [] (int i) { return cb::scaleInfo ((cb::ScaleType) i).name; }), 0));
    layout.add (std::make_unique<AudioParameterInt> (pid (id::octave), "Octave", -3, 3, 0));
    layout.add (std::make_unique<AudioParameterChoice> (pid (id::voicing), "Voicing",
        namesFrom (cb::numVoicingStyles, [] (int i) { return cb::voicingStyleName ((cb::VoicingStyle) i); }), 0));
    layout.add (std::make_unique<AudioParameterBool> (pid (id::voiceLeading), "Voice Leading", true));
    layout.add (std::make_unique<AudioParameterChoice> (pid (id::pattern), "Pattern",
        namesFrom (cb::numRhythmPatterns, [] (int i) { return cb::rhythmPatternName ((cb::RhythmPattern) i); }), 0));
    layout.add (std::make_unique<AudioParameterChoice> (pid (id::rate), "Rate",
        namesFrom (cb::numRateDivisions, [] (int i) { return cb::rateDivisionName ((cb::RateDivision) i); }), 1));
    layout.add (std::make_unique<AudioParameterChoice> (pid (id::bassMode), "Bass",
        namesFrom (cb::numBassModes, [] (int i) { return cb::bassModeName ((cb::BassMode) i); }), 1));
    layout.add (std::make_unique<AudioParameterFloat> (pid (id::velocity), "Velocity", NormalisableRange<float> (0.05f, 1.0f), 0.75f));
    layout.add (std::make_unique<AudioParameterFloat> (pid (id::gate), "Gate", NormalisableRange<float> (0.05f, 1.0f), 0.9f));
    layout.add (std::make_unique<AudioParameterFloat> (pid (id::swing), "Swing", NormalisableRange<float> (0.0f, 1.0f), 0.0f));
    layout.add (std::make_unique<AudioParameterFloat> (pid (id::humanize), "Humanize", NormalisableRange<float> (0.0f, 1.0f), 0.0f));
    layout.add (std::make_unique<AudioParameterBool> (pid (id::previewOn), "Preview Sound", true));
    layout.add (std::make_unique<AudioParameterChoice> (pid (id::previewTone), "Preview Tone",
        StringArray { "Soft Keys", "Warm Pad", "Glass Pluck" }, 0));
    layout.add (std::make_unique<AudioParameterFloat> (pid (id::previewVolume), "Preview Volume",
        NormalisableRange<float> (-48.0f, 0.0f), -9.0f));
    layout.add (std::make_unique<AudioParameterBool> (pid (id::midiOut), "MIDI Out", true));
    layout.add (std::make_unique<AudioParameterBool> (pid (id::syncToHost), "Sync To Host", true));
    layout.add (std::make_unique<AudioParameterBool> (pid (id::scaleLock), "Scale Lock", false));
    layout.add (std::make_unique<AudioParameterBool> (pid (id::chordTrigger), "Chord Trigger", false));
    layout.add (std::make_unique<AudioParameterBool> (pid (id::triggerSevenths), "Trigger Sevenths", false));
    layout.add (std::make_unique<AudioParameterChoice> (pid (id::midiOutPart), "MIDI Out Part",
        StringArray { "Chords + Bass", "Chords only", "Bass only" }, 0));
    return layout;
}

//==============================================================================
constexpr int kStateVersion = 1;
constexpr int kMaxUndo = 100;
constexpr int kMaxTriggerNotes = 8;
constexpr int kPreviewFifoSize = 1024;
constexpr int kMaxVoices = 256;
constexpr int kMaxBlockEvents = 16384;
constexpr int kOffReserve = 4096;          // note-ons are refused before the last kOffReserve logical slots
constexpr double kMinChaseBeats = 0.05;    // notes already in progress are only re-struck if this much is left
constexpr int kMinEditorW = 960, kMaxEditorW = 1600, kMinEditorH = 600, kMaxEditorH = 1000;

const char* const kStateTag = "ChordbloomState";
const char* const kProgressionTag = "Progression";
const char* const kSlotTag = "Slot";
const char* const kEditorTag = "Editor";

const char* const kAllParamIds[] = {
    cb::ParamIDs::keyRoot, cb::ParamIDs::scale, cb::ParamIDs::octave, cb::ParamIDs::voicing, cb::ParamIDs::voiceLeading,
    cb::ParamIDs::pattern, cb::ParamIDs::rate, cb::ParamIDs::bassMode, cb::ParamIDs::velocity, cb::ParamIDs::gate,
    cb::ParamIDs::swing, cb::ParamIDs::humanize, cb::ParamIDs::previewOn, cb::ParamIDs::previewTone,
    cb::ParamIDs::previewVolume, cb::ParamIDs::midiOut, cb::ParamIDs::syncToHost, cb::ParamIDs::scaleLock,
    cb::ParamIDs::chordTrigger, cb::ParamIDs::triggerSevenths
};

// White-key pitch class -> degree (C D E F G A B -> 0..6), -1 for black keys.
constexpr int kWhiteKeyDegree[12] = { 0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6 };

inline uint8_t toMidiVelocity (float v) noexcept
{
    if (! std::isfinite (v))
        v = 0.75f;
    return (uint8_t) juce::jlimit (1, 127, juce::roundToInt (v * 127.0f));
}

//==============================================================================
// Immutable data handed to the audio thread.
struct EngineEvent
{
    double start = 0.0;   // beats from loop start
    double end = 0.0;     // beats from loop start, > start, <= totalBeats
    uint8_t note = 60;
    uint8_t channel = 1;
    uint8_t velocity = 96;
};

struct EngineData
{
    uint64_t serial = 0;
    std::vector<EngineEvent> events;   // sorted by start
    std::vector<double> slotStarts;    // numSlots + 1 entries (last = totalBeats)
    double totalBeats = 0.0;

    // Chord trigger voicings: [sevenths][degree] -> notes (ascending), relative to the C5 (MIDI 60..71) input octave.
    std::array<std::array<std::array<uint8_t, kMaxTriggerNotes>, 7>, 2> triggerNotes {};
    std::array<std::array<uint8_t, 7>, 2> triggerCounts {};

    // Scale lock: pitch class -> semitone offset to the nearest scale tone (ties snap down).
    std::array<int8_t, 12> scaleSnap {};
};

struct PreviewMsg
{
    uint8_t on = 0;
    uint8_t note = 0;
    uint8_t velocity = 0;
};

// Parameters that change what the message thread renders.
struct RenderSnapshot
{
    int keyRoot = 0, scale = 0, octave = 0, voicing = 0, pattern = 0, rate = 0, bass = 0;
    bool voiceLeading = true;
    float velocity = 0.0f, gate = 0.0f, swing = 0.0f, humanize = 0.0f;

    bool sameVoicing (const RenderSnapshot& o) const
    {
        return octave == o.octave && voicing == o.voicing && voiceLeading == o.voiceLeading;
    }

    bool operator== (const RenderSnapshot& o) const
    {
        return keyRoot == o.keyRoot && scale == o.scale && sameVoicing (o) && pattern == o.pattern && rate == o.rate
            && bass == o.bass && velocity == o.velocity && gate == o.gate && swing == o.swing && humanize == o.humanize;
    }
    bool operator!= (const RenderSnapshot& o) const { return ! (*this == o); }
};

bool sameVoicingSettings (const cb::VoicingSettings& a, const cb::VoicingSettings& b)
{
    return a.style == b.style && a.centerNote == b.centerNote && a.voiceLeading == b.voiceLeading;
}

cb::ProgressionSlot sanitiseSlot (cb::ProgressionSlot s)
{
    s.chord.root = ((s.chord.root % 12) + 12) % 12;
    s.chord.quality = (cb::ChordQuality) juce::jlimit (0, cb::numChordQualities - 1, (int) s.chord.quality);
    s.lengthBeats = juce::jlimit (cb::kMinSlotBeats, cb::kMaxSlotBeats, s.lengthBeats);
    const int numTones = juce::jmax (1, (int) cb::qualityIntervals (s.chord.quality).size());
    s.inversion = s.inversion < 0 ? -1 : juce::jmin (s.inversion, numTones - 1);
    s.octaveShift = juce::jlimit (-2, 2, s.octaveShift);
    return s;
}

cb::Chord sanitiseChord (cb::Chord c)
{
    c.root = ((c.root % 12) + 12) % 12;
    c.quality = (cb::ChordQuality) juce::jlimit (0, cb::numChordQualities - 1, (int) c.quality);
    return c;
}

cb::Progression sanitiseProgression (const cb::Progression& in)
{
    cb::Progression out;
    for (auto& s : in.slots)
    {
        if ((int) out.slots.size() >= cb::kMaxSlots)
            break;
        out.slots.push_back (sanitiseSlot (s));
    }
    return out;
}

juce::String partName (ChordbloomProcessor::ExportPart part)
{
    switch (part)
    {
        case ChordbloomProcessor::ExportPart::Chords: return "Chords";
        case ChordbloomProcessor::ExportPart::Bass:   return "Bass";
        case ChordbloomProcessor::ExportPart::Both:   break;
    }
    return "Chords + Bass";
}

// Rewrites the end-of-track markers so the clip is exactly one loop long (the last note may end before the loop end).
juce::MidiFile withLoopLength (const juce::MidiFile& in, double loopBeats)
{
    juce::MidiFile out;
    const int tpq = in.getTimeFormat() > 0 ? in.getTimeFormat() : 960;
    out.setTicksPerQuarterNote (tpq);
    const double loopTicks = std::round (juce::jmax (0.0, loopBeats) * tpq);

    for (int t = 0; t < in.getNumTracks(); ++t)
    {
        const auto* track = in.getTrack (t);
        juce::MidiMessageSequence seq;
        double lastTick = 0.0;

        if (track != nullptr)
        {
            for (const auto* holder : *track)
            {
                if (holder->message.isEndOfTrackMetaEvent())
                    continue;
                seq.addEvent (holder->message);
                lastTick = juce::jmax (lastTick, holder->message.getTimeStamp());
            }
        }

        seq.updateMatchedPairs();
        seq.addEvent (juce::MidiMessage::endOfTrack(), juce::jmax (loopTicks, lastTick));
        out.addTrack (seq);
    }
    return out;
}
} // namespace

//==============================================================================
struct ChordbloomProcessor::Impl final : private juce::Timer,
                                         private juce::AsyncUpdater,
                                         private juce::AudioProcessorValueTreeState::Listener
{
    explicit Impl (ChordbloomProcessor& p)
        : owner (p), apvts (p, nullptr, "Params", createParameterLayout())
    {
        auto get = [this] (const char* id)
        {
            auto* v = apvts.getRawParameterValue (id);
            jassert (v != nullptr);
            return v;
        };

        prm.keyRoot = get (cb::ParamIDs::keyRoot);
        prm.scale = get (cb::ParamIDs::scale);
        prm.octave = get (cb::ParamIDs::octave);
        prm.voicing = get (cb::ParamIDs::voicing);
        prm.voiceLeading = get (cb::ParamIDs::voiceLeading);
        prm.pattern = get (cb::ParamIDs::pattern);
        prm.rate = get (cb::ParamIDs::rate);
        prm.bassMode = get (cb::ParamIDs::bassMode);
        prm.velocity = get (cb::ParamIDs::velocity);
        prm.gate = get (cb::ParamIDs::gate);
        prm.swing = get (cb::ParamIDs::swing);
        prm.humanize = get (cb::ParamIDs::humanize);
        prm.previewOn = get (cb::ParamIDs::previewOn);
        prm.previewTone = get (cb::ParamIDs::previewTone);
        prm.previewVolume = get (cb::ParamIDs::previewVolume);
        prm.midiOut = get (cb::ParamIDs::midiOut);
        prm.syncToHost = get (cb::ParamIDs::syncToHost);
        prm.scaleLock = get (cb::ParamIDs::scaleLock);
        prm.chordTrigger = get (cb::ParamIDs::chordTrigger);
        prm.triggerSevenths = get (cb::ParamIDs::triggerSevenths);

        for (auto* id : kAllParamIds)
            apvts.addParameterListener (id, this);

        combined.ensureSize (32768);

        paletteKey = keyFromParams();
        palette = safeBuildPalette (paletteKey);
        renderedSnap = readSnapshot();
        rebuildAndPublish();

        startTimerHz (20);
    }

    ~Impl() override
    {
        stopTimer();
        cancelPendingUpdate();
        for (auto* id : kAllParamIds)
            apvts.removeParameterListener (id, this);
    }

    //==============================================================================
    // Parameters
    struct ParamPointers
    {
        std::atomic<float>* keyRoot = nullptr;
        std::atomic<float>* scale = nullptr;
        std::atomic<float>* octave = nullptr;
        std::atomic<float>* voicing = nullptr;
        std::atomic<float>* voiceLeading = nullptr;
        std::atomic<float>* pattern = nullptr;
        std::atomic<float>* rate = nullptr;
        std::atomic<float>* bassMode = nullptr;
        std::atomic<float>* velocity = nullptr;
        std::atomic<float>* gate = nullptr;
        std::atomic<float>* swing = nullptr;
        std::atomic<float>* humanize = nullptr;
        std::atomic<float>* previewOn = nullptr;
        std::atomic<float>* previewTone = nullptr;
        std::atomic<float>* previewVolume = nullptr;
        std::atomic<float>* midiOut = nullptr;
        std::atomic<float>* syncToHost = nullptr;
        std::atomic<float>* scaleLock = nullptr;
        std::atomic<float>* chordTrigger = nullptr;
        std::atomic<float>* triggerSevenths = nullptr;
    };

    static float rawValue (const std::atomic<float>* p, float fallback = 0.0f) noexcept
    {
        if (p == nullptr)
            return fallback;
        const float v = p->load (std::memory_order_relaxed);
        return std::isfinite (v) ? v : fallback;
    }

    static int intValue (const std::atomic<float>* p, int lo, int hi, int fallback = 0) noexcept
    {
        return juce::jlimit (lo, hi, (int) std::lround (rawValue (p, (float) fallback)));
    }

    static bool boolValue (const std::atomic<float>* p) noexcept { return rawValue (p) >= 0.5f; }

    cb::Key keyFromParams() const
    {
        cb::Key k;
        k.tonic = intValue (prm.keyRoot, 0, 11);
        k.scale = (cb::ScaleType) intValue (prm.scale, 0, cb::numScaleTypes - 1);
        return k;
    }

    cb::VoicingSettings voicingFromParams() const
    {
        cb::VoicingSettings vs;
        vs.style = (cb::VoicingStyle) intValue (prm.voicing, 0, cb::numVoicingStyles - 1);
        vs.centerNote = 60 + 12 * intValue (prm.octave, -3, 3);
        vs.voiceLeading = boolValue (prm.voiceLeading);
        return vs;
    }

    cb::RenderSettings renderFromParams() const
    {
        cb::RenderSettings rs;
        rs.pattern = (cb::RhythmPattern) intValue (prm.pattern, 0, cb::numRhythmPatterns - 1);
        rs.rate = (cb::RateDivision) intValue (prm.rate, 0, cb::numRateDivisions - 1, 1);
        rs.bass = (cb::BassMode) intValue (prm.bassMode, 0, cb::numBassModes - 1, 1);
        rs.velocity = juce::jlimit (0.05f, 1.0f, rawValue (prm.velocity, 0.75f));
        rs.gate = juce::jlimit (0.05f, 1.0f, rawValue (prm.gate, 0.9f));
        rs.swing = juce::jlimit (0.0f, 1.0f, rawValue (prm.swing, 0.0f));
        rs.humanize = juce::jlimit (0.0f, 1.0f, rawValue (prm.humanize, 0.0f));
        return rs;
    }

    RenderSnapshot readSnapshot() const
    {
        RenderSnapshot s;
        s.keyRoot = intValue (prm.keyRoot, 0, 11);
        s.scale = intValue (prm.scale, 0, cb::numScaleTypes - 1);
        s.octave = intValue (prm.octave, -3, 3);
        s.voicing = intValue (prm.voicing, 0, cb::numVoicingStyles - 1);
        s.voiceLeading = boolValue (prm.voiceLeading);
        s.pattern = intValue (prm.pattern, 0, cb::numRhythmPatterns - 1);
        s.rate = intValue (prm.rate, 0, cb::numRateDivisions - 1, 1);
        s.bass = intValue (prm.bassMode, 0, cb::numBassModes - 1, 1);
        s.velocity = rawValue (prm.velocity, 0.75f);
        s.gate = rawValue (prm.gate, 0.9f);
        s.swing = rawValue (prm.swing);
        s.humanize = rawValue (prm.humanize);
        return s;
    }

    //==============================================================================
    // Message thread: parameter change handling
    void parameterChanged (const juce::String&, float) override
    {
        // May be called on any thread (host automation can come from the audio thread) and under the APVTS
        // adapter's listener lock: keep this trivial. The 20 Hz timer catches everything; on the message thread an
        // async update makes UI-driven changes feel immediate.
        paramsDirty.store (true);
        if (juce::MessageManager::existsAndIsCurrentThread())
            triggerAsyncUpdate();
    }

    void handleAsyncUpdate() override { serviceMessageThread(); }
    void timerCallback() override { serviceMessageThread(); }

    void serviceMessageThread()
    {
        applyPendingState();
        paramsDirty.store (false);
        refreshFromParams (false);
        collectGarbage();
    }

    void refreshFromParams (bool force)
    {
        const auto snap = readSnapshot();
        ensurePalette();
        const bool keyChanged = snap.keyRoot != renderedSnap.keyRoot || snap.scale != renderedSnap.scale;
        const bool changed = force || snap != renderedSnap;
        const bool voicingChanged = ! snap.sameVoicing (renderedSnap);

        if (changed)
        {
            renderedSnap = snap;
            rebuildAndPublish();
        }

        if (keyChanged || voicingChanged || force)
            owner.sendChangeMessage();
    }

    // Rebuilds the palette when the key parameters changed. Returns true when it did.
    bool ensurePalette() const
    {
        const auto key = keyFromParams();
        if (key == paletteKey && ! palette.empty())
            return false;

        paletteKey = key;
        palette = safeBuildPalette (key);
        return true;
    }

    static std::vector<cb::PaletteSection> safeBuildPalette (const cb::Key& key)
    {
        std::vector<cb::PaletteSection> result;
        try
        {
            result = cb::buildPalette (key);
        }
        catch (...)
        {
            result.clear();
        }

        if (result.empty())
        {
            const char* titles[] = { "Triads", "Sevenths", "Color", "Borrowed", "Secondary" };
            for (int i = 0; i < cb::numPaletteSections; ++i)
                result.push_back ({ (cb::PaletteSectionId) i, titles[i], {} });
        }
        return result;
    }

    //==============================================================================
    // Message thread: rendering & publishing
    const std::vector<std::vector<int>>& voicingsFor (const cb::VoicingSettings& vs) const
    {
        if (! voicingCacheValid || cachedVoicingProgression != progression || ! sameVoicingSettings (cachedVoicingSettings, vs))
        {
            try
            {
                cachedVoicings = cb::voiceProgression (progression, vs);
            }
            catch (...)
            {
                cachedVoicings.clear();
            }

            cachedVoicings.resize (progression.slots.size());
            cachedVoicingProgression = progression;
            cachedVoicingSettings = vs;
            voicingCacheValid = true;
        }
        return cachedVoicings;
    }

    std::vector<cb::NoteEvent> renderEvents() const
    {
        if (progression.empty())
            return {};

        const auto& voicings = voicingsFor (voicingFromParams());
        try
        {
            return cb::renderProgression (progression, voicings, renderFromParams());
        }
        catch (...)
        {
            return {};
        }
    }

    static std::vector<uint8_t> sanitiseNotes (const std::vector<int>& notes, size_t maxNotes)
    {
        std::vector<uint8_t> out;
        for (int n : notes)
            if (n >= 0 && n <= 127)
                out.push_back ((uint8_t) n);
        std::sort (out.begin(), out.end());
        out.erase (std::unique (out.begin(), out.end()), out.end());
        if (out.size() > maxNotes)
            out.resize (maxNotes);
        return out;
    }

    void rebuildAndPublish()
    {
        auto data = std::make_unique<EngineData>();
        data->serial = nextSerial++;

        const auto key = keyFromParams();
        const auto vs = voicingFromParams();

        // Progression loop
        const double total = (double) progression.totalBeats();
        data->totalBeats = total;
        data->slotStarts.reserve (progression.slots.size() + 1);
        for (int i = 0; i <= (int) progression.slots.size(); ++i)
            data->slotStarts.push_back (progression.slotStartBeat (i));

        if (total > 0.0)
        {
            const auto events = renderEvents();
            data->events.reserve (events.size());

            for (const auto& e : events)
            {
                if (! std::isfinite (e.startBeat) || ! std::isfinite (e.lengthBeats) || e.note < 0 || e.note > 127)
                    continue;

                const double start = juce::jmax (0.0, e.startBeat);
                if (start >= total)
                    continue;

                double end = juce::jmin (total, start + juce::jmax (e.lengthBeats, 1.0 / 128.0));
                if (end <= start)
                    end = juce::jmin (total, start + 1.0e-4);
                if (end <= start)
                    continue;

                EngineEvent ev;
                ev.start = start;
                ev.end = end;
                ev.note = (uint8_t) e.note;
                ev.channel = (uint8_t) juce::jlimit (1, 16, e.channel);
                ev.velocity = toMidiVelocity (e.velocity);
                data->events.push_back (ev);
            }

            std::stable_sort (data->events.begin(), data->events.end(),
                              [] (const EngineEvent& a, const EngineEvent& b) { return a.start < b.start; });
        }

        // Chord trigger voicings (both triads and sevenths, so toggling triggerSevenths needs no re-render)
        for (int s = 0; s < 2; ++s)
        {
            std::vector<int> tonicVoicing;
            for (int degree = 0; degree < 7; ++degree)
            {
                std::vector<int> notes;
                try
                {
                    const auto chord = cb::diatonicChord (key, degree, s == 1);
                    if (degree == 0)
                    {
                        notes = cb::voiceChord (chord, vs, -1, 0, {});
                        tonicVoicing = notes;
                    }
                    else
                    {
                        // Voice every degree close to the tonic voicing so jumping between keys stays smooth.
                        notes = cb::voiceChord (chord, vs, -1, 0, vs.voiceLeading ? tonicVoicing : std::vector<int>());
                    }
                }
                catch (...)
                {
                    notes.clear();
                }

                const auto clean = sanitiseNotes (notes, (size_t) kMaxTriggerNotes);
                data->triggerCounts[(size_t) s][(size_t) degree] = (uint8_t) clean.size();
                for (size_t i = 0; i < clean.size(); ++i)
                    data->triggerNotes[(size_t) s][(size_t) degree][i] = clean[i];
            }
        }

        // Scale lock table
        for (int p = 0; p < 12; ++p)
        {
            int8_t offset = 0;
            if (! key.contains (p))
            {
                for (int d = 1; d <= 6; ++d)
                {
                    if (key.contains (((p - d) % 12 + 12) % 12)) { offset = (int8_t) -d; break; }
                    if (key.contains ((p + d) % 12))             { offset = (int8_t) d;  break; }
                }
            }
            data->scaleSnap[(size_t) p] = offset;
        }

        publish (std::move (data));
    }

    void publish (std::unique_ptr<EngineData> data)
    {
        auto* raw = data.get();
        pool.push_back (std::move (data));
        latest.store (raw);
        collectGarbage();
    }

    void collectGarbage()
    {
        auto* current = latest.load();
        auto* used = audioInUse.load();
        pool.erase (std::remove_if (pool.begin(), pool.end(),
                                    [&] (const std::unique_ptr<EngineData>& e) { return e.get() != current && e.get() != used; }),
                    pool.end());
    }

    //==============================================================================
    // Message thread: progression editing
    void syncStateCopy()
    {
        const std::lock_guard<std::mutex> lock (stateMutex);
        stateProgression = progression;
    }

    void progressionChanged()
    {
        syncStateCopy();
        rebuildAndPublish();
        owner.sendChangeMessage();
    }

    bool commit (const cb::Progression& proposed)
    {
        auto next = sanitiseProgression (proposed);
        if (next == progression)
            return false;

        undoStack.push_back (progression);
        if ((int) undoStack.size() > kMaxUndo)
            undoStack.erase (undoStack.begin());
        redoStack.clear();

        progression = std::move (next);
        progressionChanged();
        return true;
    }

    //==============================================================================
    // Message thread: preview
    bool pushPreview (bool on, int note, uint8_t velocity)
    {
        if (fifo.getFreeSpace() < 1)
        {
            previewOverflow.store (true);
            return false;
        }

        const PreviewMsg msg { (uint8_t) (on ? 1 : 0), (uint8_t) juce::jlimit (0, 127, note), velocity };
        fifo.write (1).forEach ([&] (int index) { previewBuffer[(size_t) index] = msg; });
        return true;
    }

    void stopPreviewNotes()
    {
        for (int n : previewNotes)
            pushPreview (false, n, 0);
        previewNotes.clear();
        previewChord.reset();
    }

    void startPreviewNotes (const std::vector<int>& notes)
    {
        const auto clean = sanitiseNotes (notes, 16);
        if (clean.empty() || fifo.getFreeSpace() < (int) clean.size())
            return;

        const auto vel = toMidiVelocity (juce::jlimit (0.05f, 1.0f, rawValue (prm.velocity, 0.75f)));
        for (auto n : clean)
            if (pushPreview (true, n, vel))
                previewNotes.push_back (n);
    }

    //==============================================================================
    // State
    void writeState (juce::MemoryBlock& dest)
    {
        juce::XmlElement root (kStateTag);
        root.setAttribute ("version", kStateVersion);

        if (auto params = apvts.copyState().createXml())
            root.addChildElement (params.release());

        auto* prog = root.createNewChildElement (kProgressionTag);
        {
            const std::lock_guard<std::mutex> lock (stateMutex);
            for (auto& s : stateProgression.slots)
            {
                auto* e = prog->createNewChildElement (kSlotTag);
                e->setAttribute ("root", s.chord.root);
                e->setAttribute ("quality", (int) s.chord.quality);
                e->setAttribute ("length", s.lengthBeats);
                e->setAttribute ("inversion", s.inversion);
                e->setAttribute ("octave", s.octaveShift);
            }
        }

        auto* editor = root.createNewChildElement (kEditorTag);
        editor->setAttribute ("width", editorW.load());
        editor->setAttribute ("height", editorH.load());

        juce::AudioProcessor::copyXmlToBinary (root, dest);
    }

    void applyParamsXml (const juce::XmlElement& xml)
    {
        auto tree = juce::ValueTree::fromXml (xml);
        if (! tree.isValid() || tree.getType() != apvts.state.getType())
            return;

        // Sanitise: every known parameter gets a finite, in-range value; missing ones fall back to their default.
        for (auto* p : owner.getParameters())
        {
            auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p);
            if (rp == nullptr)
                continue;

            const auto& range = rp->getNormalisableRange();
            const double def = (double) rp->convertFrom0to1 (rp->getDefaultValue());
            auto child = tree.getChildWithProperty ("id", rp->paramID);

            if (! child.isValid())
            {
                child = juce::ValueTree ("PARAM");
                child.setProperty ("id", rp->paramID, nullptr);
                tree.appendChild (child, nullptr);
            }

            const auto v = child.getProperty ("value");
            double value = (v.isVoid() || v.isUndefined()) ? def : (double) v;
            if (! std::isfinite (value))
                value = def;
            value = juce::jlimit ((double) range.start, (double) range.end, value);
            child.setProperty ("value", value, nullptr);
        }

        apvts.replaceState (tree);
    }

    static cb::Progression parseProgression (const juce::XmlElement& xml)
    {
        cb::Progression p;
        for (auto* e : xml.getChildWithTagNameIterator (kSlotTag))
        {
            if ((int) p.slots.size() >= cb::kMaxSlots)
                break;

            cb::ProgressionSlot s;
            s.chord.root = e->getIntAttribute ("root", 0);
            s.chord.quality = (cb::ChordQuality) juce::jlimit (0, cb::numChordQualities - 1, e->getIntAttribute ("quality", 0));
            s.lengthBeats = e->getIntAttribute ("length", 4);
            s.inversion = e->getIntAttribute ("inversion", -1);
            s.octaveShift = e->getIntAttribute ("octave", 0);
            p.slots.push_back (sanitiseSlot (s));
        }
        return p;
    }

    void readState (const void* data, int sizeInBytes)
    {
        if (data == nullptr || sizeInBytes <= 0)
            return;

        const auto xml = juce::AudioProcessor::getXmlFromBinary (data, sizeInBytes);
        if (xml == nullptr)
            return;

        const juce::XmlElement* paramsXml = nullptr;
        const juce::XmlElement* progXml = nullptr;
        const juce::XmlElement* editorXml = nullptr;
        bool isFullState = false;

        if (xml->hasTagName (kStateTag))
        {
            isFullState = true;
            paramsXml = xml->getChildByName (apvts.state.getType());
            progXml = xml->getChildByName (kProgressionTag);
            editorXml = xml->getChildByName (kEditorTag);
        }
        else if (xml->hasTagName (apvts.state.getType().toString()))
        {
            paramsXml = xml.get();   // bare APVTS state
        }
        else
        {
            return;
        }

        if (paramsXml != nullptr)
            applyParamsXml (*paramsXml);

        if (editorXml != nullptr)
        {
            const int w = editorXml->getIntAttribute ("width", 0);
            const int h = editorXml->getIntAttribute ("height", 0);
            if (w > 0 && h > 0)
            {
                editorW.store (juce::jlimit (kMinEditorW, kMaxEditorW, w));
                editorH.store (juce::jlimit (kMinEditorH, kMaxEditorH, h));
            }
            else
            {
                editorW.store (0);
                editorH.store (0);
            }
        }

        if (! isFullState)
        {
            if (juce::MessageManager::existsAndIsCurrentThread())
                refreshFromParams (true);
            return;
        }

        const auto loaded = progXml != nullptr ? parseProgression (*progXml) : cb::Progression();

        if (juce::MessageManager::existsAndIsCurrentThread())
        {
            {
                const std::lock_guard<std::mutex> lock (stateMutex);
                pendingProgression.reset();
            }
            applyLoadedProgression (loaded);
        }
        else
        {
            {
                const std::lock_guard<std::mutex> lock (stateMutex);
                pendingProgression = loaded;
                stateProgression = loaded;
            }
            triggerAsyncUpdate();
        }
    }

    void applyLoadedProgression (const cb::Progression& loaded)
    {
        progression = sanitiseProgression (loaded);
        undoStack.clear();
        redoStack.clear();
        syncStateCopy();
        renderedSnap = readSnapshot();
        ensurePalette();
        rebuildAndPublish();
        owner.sendChangeMessage();
    }

    void applyPendingState()
    {
        std::optional<cb::Progression> pending;
        {
            const std::lock_guard<std::mutex> lock (stateMutex);
            pending.swap (pendingProgression);
        }
        if (pending.has_value())
            applyLoadedProgression (*pending);
    }

    //==============================================================================
    // Audio thread
    struct Voice
    {
        double endT = 0.0;   // timeline beats
        uint8_t note = 0;
        uint8_t channel = 1;
    };

    struct LogicalEvent
    {
        int sample = 0;
        int seq = 0;
        uint8_t on = 0;
        uint8_t channel = 1;
        uint8_t note = 0;
        uint8_t velocity = 0;
    };

    struct InputEntry
    {
        uint8_t count = 0;
        uint8_t outChannel = 1;
        int8_t degree = -1;
        std::array<uint8_t, kMaxTriggerNotes> notes {};
    };

    enum class PlayMode { None, Host, Local };

    struct AudioState
    {
        PlayMode mode = PlayMode::None;
        double expectedT = 0.0;
        double localClock = 0.0;
        double lastBpm = 120.0;
        uint64_t dataSerial = 0;
        uint32_t seenLocalGen = 0;
        bool loopWasEmpty = true;
        bool forcePanic = false;
        bool prevMidiOut = true;
        bool prevPreviewOn = true;

        // timing of the current block (for beat -> sample conversion)
        int numSamples = 0;
        double blockT0 = 0.0;
        double beatsPerSample = 0.0;

        std::array<Voice, kMaxVoices> voices {};
        int numVoices = 0;

        std::array<LogicalEvent, kMaxBlockEvents> events {};
        int numEvents = 0;
        int seq = 0;

        std::array<std::array<uint16_t, 128>, 16> ref {};     // how many sources hold (channel, note)
        std::array<uint8_t, 128> notesOnPerPitch {};          // number of channels with ref > 0, per note
        std::array<std::array<bool, 128>, 16> hostOn {};      // what the host has been sent
        std::array<bool, 128> previewHeld {};                 // preview notes (channel 1)
        std::array<InputEntry, 16 * 128> input {};            // live input (channel, note) -> output notes
        int lastTriggerKey = -1;
    };

    // Hazard-pointer acquire: once this returns p, the message thread will not free p until a later acquire.
    EngineData* acquireEngineData() noexcept
    {
        auto* p = latest.load();
        for (;;)
        {
            audioInUse.store (p);
            auto* q = latest.load();
            if (q == p)
                return p;
            p = q;
        }
    }

    bool emitOn (int channel, int note, int velocity, int sample) noexcept
    {
        if (au.numEvents >= kMaxBlockEvents - kOffReserve || note < 0 || note > 127)
            return false;

        auto& e = au.events[(size_t) au.numEvents++];
        e = { sample, au.seq++, 1, (uint8_t) channel, (uint8_t) note, (uint8_t) juce::jlimit (1, 127, velocity) };
        return true;
    }

    void emitOff (int channel, int note, int sample) noexcept
    {
        if (note < 0 || note > 127)
            return;

        if (au.numEvents >= kMaxBlockEvents)
        {
            au.forcePanic = true;   // could not record the note-off: silence everything next block
            return;
        }

        auto& e = au.events[(size_t) au.numEvents++];
        e = { sample, au.seq++, 0, (uint8_t) channel, (uint8_t) note, 0 };
    }

    void addRaw (uint8_t status, uint8_t d1, uint8_t d2, int sample) noexcept
    {
        const uint8_t bytes[3] = { status, d1, d2 };
        combined.addEvent (bytes, 3, sample);
    }

    int clampSample (int s) const noexcept { return juce::jlimit (0, juce::jmax (0, au.numSamples - 1), s); }

    int beatToSample (double t) const noexcept
    {
        if (au.beatsPerSample <= 0.0)
            return 0;
        const double s = std::floor ((t - au.blockT0) / au.beatsPerSample);
        if (! (s > 0.0))
            return 0;
        return clampSample (s >= (double) au.numSamples ? au.numSamples - 1 : (int) s);
    }

    void flushLogicalEvents() noexcept
    {
        std::sort (au.events.begin(), au.events.begin() + au.numEvents,
                   [] (const LogicalEvent& a, const LogicalEvent& b)
                   { return a.sample != b.sample ? a.sample < b.sample : a.seq < b.seq; });

        for (int i = 0; i < au.numEvents; ++i)
        {
            const auto& e = au.events[(size_t) i];
            const int chIndex = juce::jlimit (0, 15, (int) e.channel - 1);
            auto& r = au.ref[(size_t) chIndex][e.note];
            const auto onStatus = (uint8_t) (0x90 | chIndex);
            const auto offStatus = (uint8_t) (0x80 | chIndex);

            if (e.on != 0)
            {
                if (r > 0)
                    addRaw (offStatus, e.note, 0, e.sample);   // retrigger: keep the stream balanced
                else
                    ++au.notesOnPerPitch[e.note];

                addRaw (onStatus, e.note, e.velocity, e.sample);
                if (r < 0xffff)
                    ++r;
            }
            else
            {
                if (r == 0)
                {
                    addRaw (offStatus, e.note, 0, e.sample);   // unmatched (e.g. key held before load): harmless
                }
                else if (--r == 0)
                {
                    addRaw (offStatus, e.note, 0, e.sample);
                    if (au.notesOnPerPitch[e.note] > 0)
                        --au.notesOnPerPitch[e.note];
                }
            }
        }

        au.numEvents = 0;
    }

    void panicAll() noexcept
    {
        for (int ch = 0; ch < 16; ++ch)
        {
            for (int n = 0; n < 128; ++n)
            {
                if (au.ref[(size_t) ch][(size_t) n] > 0)
                    addRaw ((uint8_t) (0x80 | ch), (uint8_t) n, 0, 0);
                au.ref[(size_t) ch][(size_t) n] = 0;
            }
        }

        au.notesOnPerPitch.fill (0);
        au.numVoices = 0;
        au.previewHeld.fill (false);
        for (auto& e : au.input)
            e = {};
        au.lastTriggerKey = -1;
        triggeredDegree.store (-1);
        au.mode = PlayMode::None;
        au.loopWasEmpty = true;
    }

    //------------------------------------------------------------------------------
    void drainPreview() noexcept
    {
        const int ready = fifo.getNumReady();
        if (ready > 0)
        {
            fifo.read (ready).forEach ([this] (int index)
            {
                const auto& m = previewBuffer[(size_t) index];
                const int n = m.note & 0x7f;

                if (m.on != 0)
                {
                    if (au.previewHeld[(size_t) n])
                        emitOff (ChordbloomProcessor::kChordChannel, n, 0);
                    au.previewHeld[(size_t) n] = emitOn (ChordbloomProcessor::kChordChannel, n, m.velocity, 0);
                }
                else if (au.previewHeld[(size_t) n])
                {
                    emitOff (ChordbloomProcessor::kChordChannel, n, 0);
                    au.previewHeld[(size_t) n] = false;
                }
            });
        }

        if (previewOverflow.exchange (false))
        {
            for (int n = 0; n < 128; ++n)
            {
                if (au.previewHeld[(size_t) n])
                {
                    emitOff (ChordbloomProcessor::kChordChannel, n, 0);
                    au.previewHeld[(size_t) n] = false;
                }
            }
        }
    }

    //------------------------------------------------------------------------------
    void releaseInputEntry (InputEntry& entry, int sample) noexcept
    {
        for (int i = 0; i < entry.count; ++i)
            emitOff (entry.outChannel, entry.notes[(size_t) i], sample);
        entry.count = 0;
        entry.degree = -1;
    }

    void updateTriggeredDegreeAfterRelease (int releasedKey) noexcept
    {
        if (releasedKey != au.lastTriggerKey)
            return;

        au.lastTriggerKey = -1;
        int degree = -1;
        for (int i = 0; i < (int) au.input.size(); ++i)
        {
            if (au.input[(size_t) i].degree >= 0)
            {
                au.lastTriggerKey = i;
                degree = au.input[(size_t) i].degree;
                break;
            }
        }
        triggeredDegree.store (degree);
    }

    void handleInputNoteOn (const EngineData* d, int channel, int note, int velocity, int sample,
                            bool scaleLockOn, bool chordTriggerOn, bool seventhsOn) noexcept
    {
        const int key = (channel - 1) * 128 + note;
        auto& entry = au.input[(size_t) key];

        if (entry.count > 0 || entry.degree >= 0)
        {
            releaseInputEntry (entry, sample);   // repeated note-on without note-off
            updateTriggeredDegreeAfterRelease (key);
        }

        entry.outChannel = (uint8_t) channel;
        entry.count = 0;
        entry.degree = -1;

        const int pitchClass = note % 12;
        const int degree = kWhiteKeyDegree[pitchClass];

        if (chordTriggerOn && d != nullptr && degree >= 0)
        {
            const int set = seventhsOn ? 1 : 0;
            const int shift = (note / 12 - 5) * 12;   // MIDI 60..71 (C5 in FL naming) plays the voicing as is
            const int count = d->triggerCounts[(size_t) set][(size_t) degree];

            for (int i = 0; i < count && entry.count < kMaxTriggerNotes; ++i)
            {
                const int out = (int) d->triggerNotes[(size_t) set][(size_t) degree][(size_t) i] + shift;
                if (out >= 0 && out <= 127 && emitOn (channel, out, velocity, sample))
                    entry.notes[(size_t) entry.count++] = (uint8_t) out;
            }

            entry.degree = (int8_t) degree;
            au.lastTriggerKey = key;
            triggeredDegree.store (degree);
            return;
        }

        int out = note;
        if (scaleLockOn && d != nullptr)
        {
            out = note + d->scaleSnap[(size_t) pitchClass];
            if (out < 0 || out > 127)
                out = note;
        }

        if (emitOn (channel, out, velocity, sample))
            entry.notes[(size_t) entry.count++] = (uint8_t) out;
    }

    void handleInputNoteOff (int channel, int note, int sample) noexcept
    {
        const int key = (channel - 1) * 128 + note;
        auto& entry = au.input[(size_t) key];

        if (entry.count == 0 && entry.degree < 0)
        {
            emitOff (channel, note, sample);   // never saw the note-on: pass the note-off through
            return;
        }

        releaseInputEntry (entry, sample);
        updateTriggeredDegreeAfterRelease (key);
    }

    void releaseInputChannel (int channel, int sample) noexcept
    {
        for (int n = 0; n < 128; ++n)
        {
            const int key = (channel - 1) * 128 + n;
            auto& entry = au.input[(size_t) key];
            if (entry.count > 0 || entry.degree >= 0)
            {
                releaseInputEntry (entry, sample);
                updateTriggeredDegreeAfterRelease (key);
            }
        }
    }

    void handleInput (const juce::MidiBuffer& midi, const EngineData* d, bool scaleLockOn, bool chordTriggerOn,
                      bool seventhsOn) noexcept
    {
        for (const auto meta : midi)
        {
            const auto* raw = meta.data;
            const int size = meta.numBytes;
            const int sample = clampSample (meta.samplePosition);

            if (raw == nullptr || size <= 0)
                continue;

            const uint8_t status = raw[0];
            const uint8_t type = (uint8_t) (status & 0xf0);
            const int channel = (status & 0x0f) + 1;

            if (size >= 3 && status < 0xf0 && (type == 0x90 || type == 0x80))
            {
                const int note = raw[1] & 0x7f;
                const int velocity = raw[2] & 0x7f;

                if (type == 0x90 && velocity > 0)
                    handleInputNoteOn (d, channel, note, velocity, sample, scaleLockOn, chordTriggerOn, seventhsOn);
                else
                    handleInputNoteOff (channel, note, sample);
                continue;
            }

            if (size >= 3 && type == 0xb0 && status < 0xf0 && (raw[1] == 120 || raw[1] == 123))
                releaseInputChannel (channel, sample);   // all sound off / all notes off

            combined.addEvent (raw, size, sample);       // everything else passes through untouched
        }
    }

    //------------------------------------------------------------------------------
    // Playback voices
    int findVoice (int note, int channel) const noexcept
    {
        for (int i = 0; i < au.numVoices; ++i)
            if (au.voices[(size_t) i].note == note && au.voices[(size_t) i].channel == channel)
                return i;
        return -1;
    }

    void removeVoice (int index) noexcept
    {
        au.voices[(size_t) index] = au.voices[(size_t) (au.numVoices - 1)];
        --au.numVoices;
    }

    void releaseAllVoices (int sample) noexcept
    {
        for (int i = 0; i < au.numVoices; ++i)
            emitOff (au.voices[(size_t) i].channel, au.voices[(size_t) i].note, sample);
        au.numVoices = 0;
    }

    void retireVoices (double t, bool inclusive) noexcept
    {
        for (int i = au.numVoices; --i >= 0;)
        {
            const auto& v = au.voices[(size_t) i];
            if (inclusive ? v.endT <= t : v.endT < t)
            {
                emitOff (v.channel, v.note, beatToSample (v.endT));
                removeVoice (i);
            }
        }
    }

    void startVoice (const EngineEvent& e, double startT, double endT, int sample) noexcept
    {
        const int existing = findVoice (e.note, e.channel);
        if (existing >= 0)
        {
            emitOff (e.channel, e.note, sample);
            removeVoice (existing);
        }

        if (au.numVoices >= kMaxVoices)
            return;

        if (! emitOn (e.channel, e.note, e.velocity, sample))
            return;

        au.voices[(size_t) au.numVoices++] = { juce::jmax (endT, startT + 1.0e-6), e.note, e.channel };
    }

    // Re-establishes the voices at timeline position t for (possibly new) data d: voices still covered by an event
    // with the same note keep sounding (no retrigger for e.g. a velocity tweak), the others get note-offs, and notes
    // that should be sounding at t but are not are started ("chase").
    void resyncVoices (const EngineData& d, double t) noexcept
    {
        if (t < 0.0 || d.totalBeats <= 0.0)
        {
            releaseAllVoices (0);
            return;
        }

        const double L = d.totalBeats;
        const double k = std::floor (t / L);
        const double base = k * L;
        const double pos = t - base;

        for (int i = au.numVoices; --i >= 0;)
        {
            auto& v = au.voices[(size_t) i];
            bool covered = false;

            for (const auto& e : d.events)
            {
                if (e.start >= pos)
                    break;
                if (e.note == v.note && e.channel == v.channel && e.end > pos)
                {
                    covered = true;
                    v.endT = base + e.end;
                    break;
                }
            }

            if (! covered)
            {
                emitOff (v.channel, v.note, 0);
                removeVoice (i);
            }
        }

        for (const auto& e : d.events)
        {
            if (e.start >= pos)
                break;
            if (e.end - pos >= kMinChaseBeats && findVoice (e.note, e.channel) < 0)
                startVoice (e, t, base + e.end, 0);
        }
    }

    void scanEvents (const EngineData& d, double from, double to) noexcept
    {
        const double L = d.totalBeats;
        from = juce::jmax (from, 0.0);
        if (L <= 0.0 || to <= from)
            return;

        const auto begin = d.events.begin();
        const auto end = d.events.end();

        for (double k = std::floor (from / L), guard = 0; guard < 4096; k += 1.0, guard += 1)
        {
            const double base = k * L;
            if (base >= to)
                break;

            const double lo = juce::jmax (from - base, 0.0);
            const double hi = juce::jmin (to - base, L);
            if (hi <= lo)
                continue;

            auto it = std::lower_bound (begin, end, lo, [] (const EngineEvent& e, double v) { return e.start < v; });
            for (; it != end && it->start < hi; ++it)
            {
                const double t = base + it->start;
                retireVoices (t, true);
                startVoice (*it, t, base + it->end, beatToSample (t));
            }
        }
    }

    void runPlayback (const EngineData* d, bool syncOn, int numSamples) noexcept
    {
        // Host transport
        bool playing = false;
        double bpm = au.lastBpm;
        bool havePpq = false;
        double ppq = 0.0;
        const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;

        if (auto* ph = owner.getPlayHead())
        {
            if (const auto pos = ph->getPosition())
            {
                playing = pos->getIsPlaying();

                if (const auto b = pos->getBpm())
                    if (std::isfinite (*b) && *b > 1.0)
                        bpm = juce::jlimit (20.0, 999.0, *b);

                if (const auto q = pos->getPpqPosition())
                {
                    if (std::isfinite (*q))
                    {
                        ppq = *q;
                        havePpq = true;
                    }
                }
                else if (const auto secs = pos->getTimeInSeconds())
                {
                    if (std::isfinite (*secs))
                    {
                        ppq = *secs * bpm / 60.0;
                        havePpq = true;
                    }
                }
                else if (const auto samples = pos->getTimeInSamples())
                {
                    ppq = (double) *samples / sr * bpm / 60.0;
                    havePpq = true;
                }
            }
        }

        au.lastBpm = bpm;
        hostBpm.store (bpm);
        hostPlaying.store (playing);

        const bool localOn = localPlayback.load();
        const uint32_t gen = localGen.load();
        const PlayMode desired = (playing && syncOn) ? PlayMode::Host : (localOn ? PlayMode::Local : PlayMode::None);

        const bool dataChanged = d != nullptr && d->serial != au.dataSerial;
        if (d != nullptr)
            au.dataSerial = d->serial;

        bool restart = false;
        if (desired != au.mode)
        {
            releaseAllVoices (0);
            au.mode = desired;
            restart = true;
            if (desired == PlayMode::Local)
                au.localClock = 0.0;
            au.seenLocalGen = gen;
        }
        else if (desired == PlayMode::Local && gen != au.seenLocalGen)
        {
            releaseAllVoices (0);
            au.localClock = 0.0;
            au.seenLocalGen = gen;
            restart = true;
        }

        if (au.mode == PlayMode::None)
        {
            releaseAllVoices (0);
            playingSlot.store (-1);
            au.expectedT = 0.0;
            return;
        }

        au.numSamples = numSamples;
        au.beatsPerSample = bpm / (60.0 * sr);
        const double blockBeats = numSamples * au.beatsPerSample;

        double t0 = 0.0;
        if (au.mode == PlayMode::Host)
        {
            t0 = havePpq ? ppq : au.expectedT;
            if (! restart && std::abs (t0 - au.expectedT) > 0.05 + blockBeats)
            {
                releaseAllVoices (0);   // jump / host loop / relocate
                restart = true;
            }
        }
        else
        {
            t0 = au.localClock;
        }

        au.blockT0 = t0;
        const double t1 = t0 + blockBeats;

        const bool hasLoop = d != nullptr && d->totalBeats > 0.0;
        if (! hasLoop)
        {
            releaseAllVoices (0);
            playingSlot.store (-1);
            au.expectedT = t1;
            au.localClock = 0.0;   // local playback starts from the top once chords are added
            au.loopWasEmpty = true;
            return;
        }

        if (au.loopWasEmpty)
        {
            restart = true;
            au.loopWasEmpty = false;
        }

        double scanFrom = au.expectedT;
        if (restart || dataChanged)
        {
            resyncVoices (*d, t0);
            scanFrom = t0;
        }

        scanEvents (*d, scanFrom, t1);
        retireVoices (t1, false);

        au.expectedT = t1;
        if (au.mode == PlayMode::Local)
            au.localClock = t1;

        // Position display
        if (t0 >= 0.0)
        {
            const double L = d->totalBeats;
            double pos = std::fmod (t0, L);
            if (pos < 0.0)
                pos += L;

            const auto& starts = d->slotStarts;
            int slot = -1;
            if (starts.size() >= 2)
            {
                const auto it = std::upper_bound (starts.begin(), starts.end(), pos);
                slot = juce::jlimit (0, (int) starts.size() - 2, (int) (it - starts.begin()) - 1);
            }

            playbackBeat.store (pos);
            playingSlot.store (slot);
        }
        else
        {
            playbackBeat.store (0.0);
            playingSlot.store (-1);
        }
    }

    //------------------------------------------------------------------------------
    void process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) noexcept
    {
        const int numSamples = juce::jmax (0, buffer.getNumSamples());
        const int numChannels = buffer.getNumChannels();

        for (int ch = 0; ch < numChannels; ++ch)
            buffer.clear (ch, 0, numSamples);

        combined.clear();
        au.numEvents = 0;
        au.seq = 0;
        au.numSamples = numSamples;

        const bool previewOnNow = boolValue (prm.previewOn);
        const bool midiOutNow = boolValue (prm.midiOut);
        const bool syncOn = boolValue (prm.syncToHost);
        const bool scaleLockOn = boolValue (prm.scaleLock);
        const bool chordTriggerOn = boolValue (prm.chordTrigger);
        const bool seventhsOn = boolValue (prm.triggerSevenths);

        const EngineData* d = acquireEngineData();

        if (panicRequested.exchange (false) || au.forcePanic)
        {
            au.forcePanic = false;
            panicAll();
        }

        drainPreview();
        handleInput (midi, d, scaleLockOn, chordTriggerOn, seventhsOn);
        runPlayback (d, syncOn, numSamples);
        flushLogicalEvents();

        // Built-in sound
        if (numChannels > 0 && numSamples > 0)
        {
            if (previewOnNow)
            {
                synth.setTone ((PreviewSynth::Tone) intValue (prm.previewTone, 0, (int) PreviewSynth::Tone::NumTones - 1));
                synth.setGainDecibels (juce::jlimit (-48.0f, 0.0f, rawValue (prm.previewVolume, -9.0f)));
                synth.render (buffer, combined, numSamples);
            }
            else
            {
                if (au.prevPreviewOn)
                    synth.allNotesOff (true);
                synth.render (buffer, emptyMidi, numSamples);   // let release tails fade instead of clicking
            }

            // Safety net: never hand NaN/inf to the host.
            float probe = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float* x = buffer.getReadPointer (ch);
                for (int i = 0; i < numSamples; ++i)
                    probe += x[i] * 0.0f;
            }
            if (! std::isfinite (probe))
            {
                for (int ch = 0; ch < numChannels; ++ch)
                    buffer.clear (ch, 0, numSamples);
                synth.reset();
            }
        }
        else if (! previewOnNow && au.prevPreviewOn)
        {
            synth.allNotesOff (true);
        }
        au.prevPreviewOn = previewOnNow;

        // MIDI output
        midi.clear();
        if (midiOutNow)
        {
            midi.addEvents (combined, 0, -1, 0);
            for (const auto meta : combined)
            {
                if (meta.numBytes < 3 || meta.data[0] >= 0xf0)
                    continue;
                const uint8_t type = (uint8_t) (meta.data[0] & 0xf0);
                const int ch = meta.data[0] & 0x0f;
                const int note = meta.data[1] & 0x7f;
                if (type == 0x90 && meta.data[2] > 0)
                    au.hostOn[(size_t) ch][(size_t) note] = true;
                else if (type == 0x80 || type == 0x90)
                    au.hostOn[(size_t) ch][(size_t) note] = false;
            }
        }
        else if (au.prevMidiOut)
        {
            // MIDI out was just switched off: close every note the host has been sent.
            for (int ch = 0; ch < 16; ++ch)
            {
                for (int n = 0; n < 128; ++n)
                {
                    if (au.hostOn[(size_t) ch][(size_t) n])
                    {
                        const uint8_t bytes[3] = { (uint8_t) (0x80 | ch), (uint8_t) n, 0 };
                        midi.addEvent (bytes, 3, 0);
                        au.hostOn[(size_t) ch][(size_t) n] = false;
                    }
                }
            }
        }
        au.prevMidiOut = midiOutNow;

        publishSoundingNotes();
    }

    void publishSoundingNotes() noexcept
    {
        uint64_t lo = 0, hi = 0;
        for (int n = 0; n < 64; ++n)
            if (au.notesOnPerPitch[(size_t) n] > 0)
                lo |= (uint64_t) 1 << n;
        for (int n = 64; n < 128; ++n)
            if (au.notesOnPerPitch[(size_t) n] > 0)
                hi |= (uint64_t) 1 << (n - 64);
        soundingLo.store (lo);
        soundingHi.store (hi);
    }

    // Host bypass. JUCE stops calling processBlock while bypassed, so everything Chordbloom holds is closed here:
    // note-offs for every note the host was sent, all internal note state reset (playback chases again afterwards),
    // previews queued meanwhile are dropped. Input MIDI passes through untouched (JUCE's default bypass behaviour).
    // The preview synth keeps rendering so its release / reverb tails fade instead of clicking off.
    void processBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) noexcept
    {
        const int numSamples = juce::jmax (0, buffer.getNumSamples());
        const int numChannels = buffer.getNumChannels();

        for (int ch = 0; ch < numChannels; ++ch)
            buffer.clear (ch, 0, numSamples);

        if (const int ready = fifo.getNumReady(); ready > 0)
            fifo.read (ready).forEach ([] (int) {});
        previewOverflow.store (false);

        combined.clear();
        au.numEvents = 0;
        au.seq = 0;
        au.numSamples = numSamples;
        panicAll();
        combined.clear();   // panicAll's note-offs are replaced by the exact host-side ones below
        playingSlot.store (-1);
        playbackBeat.store (0.0);

        if (numChannels > 0 && numSamples > 0)
        {
            synth.allNotesOff (true);
            synth.render (buffer, emptyMidi, numSamples);

            float probe = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float* x = buffer.getReadPointer (ch);
                for (int i = 0; i < numSamples; ++i)
                    probe += x[i] * 0.0f;
            }
            if (! std::isfinite (probe))
            {
                for (int ch = 0; ch < numChannels; ++ch)
                    buffer.clear (ch, 0, numSamples);
                synth.reset();
            }
        }

        for (int ch = 0; ch < 16; ++ch)
        {
            for (int n = 0; n < 128; ++n)
            {
                if (au.hostOn[(size_t) ch][(size_t) n])
                {
                    const uint8_t bytes[3] = { (uint8_t) (0x80 | ch), (uint8_t) n, 0 };
                    midi.addEvent (bytes, 3, 0);
                    au.hostOn[(size_t) ch][(size_t) n] = false;
                }
            }
        }

        publishSoundingNotes();
    }

    //==============================================================================
    ChordbloomProcessor& owner;
    juce::AudioProcessorValueTreeState apvts;
    ParamPointers prm;

    // Message thread
    cb::Progression progression;
    std::vector<cb::Progression> undoStack, redoStack;
    mutable std::vector<cb::PaletteSection> palette;
    mutable cb::Key paletteKey;
    RenderSnapshot renderedSnap;
    std::atomic<bool> paramsDirty { true };

    mutable std::vector<std::vector<int>> cachedVoicings;
    mutable cb::Progression cachedVoicingProgression;
    mutable cb::VoicingSettings cachedVoicingSettings;
    mutable bool voicingCacheValid = false;

    std::vector<int> previewNotes;
    std::vector<int> lastPreviewVoicing;
    std::optional<cb::Chord> previewChord;

    // State shared with host threads (never touched by the audio thread)
    std::mutex stateMutex;
    cb::Progression stateProgression;
    std::optional<cb::Progression> pendingProgression;
    std::atomic<int> editorW { 0 }, editorH { 0 };

    // Publishing
    std::vector<std::unique_ptr<EngineData>> pool;
    std::atomic<EngineData*> latest { nullptr };
    std::atomic<EngineData*> audioInUse { nullptr };
    uint64_t nextSerial = 1;

    // Preview FIFO (message thread -> audio thread)
    juce::AbstractFifo fifo { kPreviewFifoSize };
    std::array<PreviewMsg, kPreviewFifoSize> previewBuffer {};
    std::atomic<bool> previewOverflow { false };

    // Transport / display (any thread)
    std::atomic<bool> localPlayback { false };
    std::atomic<uint32_t> localGen { 0 };
    std::atomic<bool> hostPlaying { false };
    std::atomic<int> playingSlot { -1 };
    std::atomic<double> playbackBeat { 0.0 };
    std::atomic<double> hostBpm { 120.0 };
    std::atomic<uint64_t> soundingLo { 0 }, soundingHi { 0 };
    std::atomic<int> triggeredDegree { -1 };
    std::atomic<bool> panicRequested { false };

    // Audio thread
    AudioState au;
    PreviewSynth synth;
    juce::MidiBuffer combined, emptyMidi;
    double sampleRate = 44100.0;
};

//==============================================================================
ChordbloomProcessor::ChordbloomProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    impl = std::make_unique<Impl> (*this);
}

ChordbloomProcessor::~ChordbloomProcessor() = default;

void ChordbloomProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    impl->sampleRate = (std::isfinite (sampleRate) && sampleRate > 0.0) ? sampleRate : 44100.0;
    impl->synth.prepare (impl->sampleRate, juce::jmax (1, samplesPerBlock));
    impl->synth.reset();
    impl->combined.ensureSize ((size_t) juce::jmax (32768, samplesPerBlock * 16));
    impl->panicRequested.store (true);   // close anything still open from before (sent in the next block)
}

void ChordbloomProcessor::releaseResources()
{
    // MIDI cannot be sent from here: the next processed block starts with note-offs for everything still on.
    impl->panicRequested.store (true);
    impl->synth.reset();
}

bool ChordbloomProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainInputChannelSet().isDisabled();
}

void ChordbloomProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    impl->process (buffer, midiMessages);
}

void ChordbloomProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    impl->processBypassed (buffer, midiMessages);
}

juce::AudioProcessorEditor* ChordbloomProcessor::createEditor() { return new ChordbloomEditor (*this); }

//==============================================================================
void ChordbloomProcessor::getStateInformation (juce::MemoryBlock& destData) { impl->writeState (destData); }
void ChordbloomProcessor::setStateInformation (const void* data, int sizeInBytes) { impl->readState (data, sizeInBytes); }

//==============================================================================
juce::AudioProcessorValueTreeState& ChordbloomProcessor::getAPVTS() noexcept { return impl->apvts; }

cb::Key ChordbloomProcessor::getKey() const { return impl->keyFromParams(); }

const std::vector<cb::PaletteSection>& ChordbloomProcessor::getPalette() const
{
    impl->ensurePalette();   // stays consistent with the key even before the async refresh has run
    return impl->palette;
}

cb::VoicingSettings ChordbloomProcessor::getVoicingSettings() const { return impl->voicingFromParams(); }
cb::RenderSettings ChordbloomProcessor::getRenderSettings() const { return impl->renderFromParams(); }

//==============================================================================
const cb::Progression& ChordbloomProcessor::getProgression() const { return impl->progression; }

void ChordbloomProcessor::setProgression (const cb::Progression& p) { impl->commit (p); }

bool ChordbloomProcessor::addChord (const cb::Chord& chord, int insertIndex)
{
    auto p = impl->progression;
    if ((int) p.slots.size() >= cb::kMaxSlots)
        return false;

    cb::ProgressionSlot slot;
    slot.chord = sanitiseChord (chord);

    if (insertIndex < 0 || insertIndex > (int) p.slots.size())
        p.slots.push_back (slot);
    else
        p.slots.insert (p.slots.begin() + insertIndex, slot);

    impl->commit (p);
    return true;
}

void ChordbloomProcessor::removeSlot (int index)
{
    auto p = impl->progression;
    if (index < 0 || index >= (int) p.slots.size())
        return;

    p.slots.erase (p.slots.begin() + index);
    impl->commit (p);
}

void ChordbloomProcessor::moveSlot (int fromIndex, int toIndex)
{
    auto p = impl->progression;
    const int n = (int) p.slots.size();
    if (fromIndex < 0 || fromIndex >= n)
        return;

    toIndex = juce::jlimit (0, n - 1, toIndex);
    if (toIndex == fromIndex)
        return;

    const auto slot = p.slots[(size_t) fromIndex];
    p.slots.erase (p.slots.begin() + fromIndex);
    p.slots.insert (p.slots.begin() + toIndex, slot);
    impl->commit (p);
}

void ChordbloomProcessor::updateSlot (int index, const cb::ProgressionSlot& slot)
{
    auto p = impl->progression;
    if (index < 0 || index >= (int) p.slots.size())
        return;

    p.slots[(size_t) index] = sanitiseSlot (slot);
    impl->commit (p);
}

bool ChordbloomProcessor::duplicateSlot (int index)
{
    auto p = impl->progression;
    if (index < 0 || index >= (int) p.slots.size() || (int) p.slots.size() >= cb::kMaxSlots)
        return false;

    const auto copy = p.slots[(size_t) index];
    p.slots.insert (p.slots.begin() + index + 1, copy);
    impl->commit (p);
    return true;
}

void ChordbloomProcessor::clearProgression() { impl->commit ({}); }

bool ChordbloomProcessor::undo()
{
    auto& i = *impl;
    if (i.undoStack.empty())
        return false;

    i.redoStack.push_back (i.progression);
    if ((int) i.redoStack.size() > kMaxUndo)
        i.redoStack.erase (i.redoStack.begin());

    i.progression = i.undoStack.back();
    i.undoStack.pop_back();
    i.progressionChanged();
    return true;
}

bool ChordbloomProcessor::redo()
{
    auto& i = *impl;
    if (i.redoStack.empty())
        return false;

    i.undoStack.push_back (i.progression);
    if ((int) i.undoStack.size() > kMaxUndo)
        i.undoStack.erase (i.undoStack.begin());

    i.progression = i.redoStack.back();
    i.redoStack.pop_back();
    i.progressionChanged();
    return true;
}

bool ChordbloomProcessor::canUndo() const { return ! impl->undoStack.empty(); }
bool ChordbloomProcessor::canRedo() const { return ! impl->redoStack.empty(); }

std::vector<std::vector<int>> ChordbloomProcessor::getProgressionVoicings() const
{
    return impl->voicingsFor (impl->voicingFromParams());
}

std::vector<cb::NoteEvent> ChordbloomProcessor::renderProgressionEvents() const { return impl->renderEvents(); }

//==============================================================================
void ChordbloomProcessor::startPreviewChord (const cb::Chord& chord)
{
    auto& i = *impl;
    i.stopPreviewNotes();

    const auto c = sanitiseChord (chord);
    std::vector<int> notes;
    try
    {
        notes = cb::voiceChord (c, i.voicingFromParams(), -1, 0, i.lastPreviewVoicing);
    }
    catch (...)
    {
        notes.clear();
    }

    if (notes.empty())
        return;

    i.startPreviewNotes (notes);
    i.lastPreviewVoicing = notes;
    i.previewChord = c;
}

void ChordbloomProcessor::startPreviewSlot (int slotIndex)
{
    auto& i = *impl;
    i.stopPreviewNotes();

    if (slotIndex < 0 || slotIndex >= (int) i.progression.slots.size())
        return;

    const auto& voicings = i.voicingsFor (i.voicingFromParams());
    if (slotIndex >= (int) voicings.size() || voicings[(size_t) slotIndex].empty())
        return;

    const auto notes = voicings[(size_t) slotIndex];
    i.startPreviewNotes (notes);
    i.lastPreviewVoicing = notes;
    i.previewChord = i.progression.slots[(size_t) slotIndex].chord;
}

void ChordbloomProcessor::startPreviewNote (int midiNote)
{
    auto& i = *impl;
    i.stopPreviewNotes();
    if (midiNote < 0 || midiNote > 127)
        return;
    i.startPreviewNotes ({ midiNote });
}

void ChordbloomProcessor::stopPreview() { impl->stopPreviewNotes(); }

std::optional<cb::Chord> ChordbloomProcessor::getPreviewChord() const { return impl->previewChord; }
std::vector<int> ChordbloomProcessor::getPreviewNotes() const { return impl->previewNotes; }

//==============================================================================
void ChordbloomProcessor::setLocalPlayback (bool shouldPlay)
{
    auto& i = *impl;
    const bool wasOn = i.localPlayback.load();
    if (shouldPlay && ! wasOn)
        i.localGen.fetch_add (1);
    i.localPlayback.store (shouldPlay);
    if (wasOn != shouldPlay)
        sendChangeMessage();
}

bool ChordbloomProcessor::isLocalPlaybackOn() const noexcept { return impl->localPlayback.load(); }
bool ChordbloomProcessor::isHostPlaying() const noexcept { return impl->hostPlaying.load(); }
int ChordbloomProcessor::getPlayingSlot() const noexcept { return impl->playingSlot.load(); }
double ChordbloomProcessor::getPlaybackBeat() const noexcept { return impl->playbackBeat.load(); }
double ChordbloomProcessor::getHostBpm() const noexcept { return impl->hostBpm.load(); }

std::bitset<128> ChordbloomProcessor::getSoundingNotes() const noexcept
{
    const uint64_t lo = impl->soundingLo.load();
    const uint64_t hi = impl->soundingHi.load();
    std::bitset<128> bits;
    for (int n = 0; n < 64; ++n)
    {
        if ((lo >> n) & 1u)
            bits.set ((size_t) n);
        if ((hi >> n) & 1u)
            bits.set ((size_t) (n + 64));
    }
    return bits;
}

int ChordbloomProcessor::getTriggeredDegree() const noexcept { return impl->triggeredDegree.load(); }

//==============================================================================
juce::File ChordbloomProcessor::writeMidiToTempFile (ExportPart part) const
{
    if (impl->progression.empty())
        return {};

    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Chordbloom");
    const auto keyName = juce::String::fromUTF8 (getKey().name().c_str());
    const auto fileName = juce::File::createLegalFileName ("Chordbloom - " + keyName + " - " + partName (part) + ".mid");
    const auto file = dir.getChildFile (fileName);

    if (saveMidiFile (file, part))
        return file;

    // The previous file may still be locked by the host (e.g. mid-drag): fall back to a fresh name.
    const auto alternative = file.getNonexistentSibling (false);
    if (saveMidiFile (alternative, part))
        return alternative;

    return {};
}

bool ChordbloomProcessor::saveMidiFile (const juce::File& destination, ExportPart part) const
{
    if (impl->progression.empty() || destination == juce::File())
        return false;

    const auto events = renderProgressionEvents();
    const bool chords = part != ExportPart::Bass;
    const bool bass = part != ExportPart::Chords;
    const auto file = cb::midiexport::buildMidiFile (events, getHostBpm(), chords, bass, 960);
    return cb::midiexport::writeMidiFile (withLoopLength (file, (double) impl->progression.totalBeats()), destination);
}

//==============================================================================
juce::Point<int> ChordbloomProcessor::getSavedEditorSize() const { return { impl->editorW.load(), impl->editorH.load() }; }

void ChordbloomProcessor::setSavedEditorSize (juce::Point<int> size)
{
    if (size.x <= 0 || size.y <= 0)
        return;
    impl->editorW.store (size.x);
    impl->editorH.store (size.y);
}

// STUBS (round 2) - to be implemented by the processor agent
juce::File ChordbloomProcessor::writeChordMidiToTempFile (const cb::Chord&) const { return {}; }
juce::File ChordbloomProcessor::writeSlotMidiToTempFile (int) const { return {}; }
void ChordbloomProcessor::transposeProgression (int) {}
int ChordbloomProcessor::bestTranspositionForKey() const { return 0; }

#if ! CHORDBLOOM_DEV_TOOL
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new ChordbloomProcessor(); }
#endif
