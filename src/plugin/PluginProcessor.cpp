#include "plugin/PluginProcessor.h"

#include "plugin/GhostRegistry.h"
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
    // Host-facing text ("54%", "-9.0 dB") instead of 7-decimal floats: readable in FL's hint bar / automation, and
    // value -> text -> value -> text round-trips exactly (clap-validator param-conversions).
    const auto percent = AudioParameterFloatAttributes()
        .withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v * 100.0f)) + "%"; })
        .withValueFromStringFunction ([] (const String& text)
        {
            const auto t = text.trim();
            const float v = t.retainCharacters ("0123456789.-").getFloatValue();
            return (t.containsChar ('%') || v > 1.0f) ? v / 100.0f : v;   // "54%" / "54" -> 0.54, "0.54" -> 0.54
        });
    const auto decibels = AudioParameterFloatAttributes()
        .withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; })
        .withValueFromStringFunction ([] (const String& text) { return text.retainCharacters ("0123456789.-").getFloatValue(); })
        .withLabel ({});

    layout.add (std::make_unique<AudioParameterFloat> (pid (id::velocity), "Velocity", NormalisableRange<float> (0.05f, 1.0f), 0.75f, percent));
    layout.add (std::make_unique<AudioParameterFloat> (pid (id::gate), "Gate", NormalisableRange<float> (0.05f, 1.0f), 0.9f, percent));
    layout.add (std::make_unique<AudioParameterFloat> (pid (id::swing), "Swing", NormalisableRange<float> (0.0f, 1.0f), 0.0f, percent));
    layout.add (std::make_unique<AudioParameterFloat> (pid (id::humanize), "Humanize", NormalisableRange<float> (0.0f, 1.0f), 0.0f, percent));
    layout.add (std::make_unique<AudioParameterBool> (pid (id::previewOn), "Preview Sound", true));
    layout.add (std::make_unique<AudioParameterChoice> (pid (id::previewTone), "Preview Tone",
        StringArray { "Soft Keys", "Warm Pad", "Glass Pluck" }, 0));
    layout.add (std::make_unique<AudioParameterFloat> (pid (id::previewVolume), "Preview Volume",
        NormalisableRange<float> (-48.0f, 0.0f), -9.0f, decibels));
    layout.add (std::make_unique<AudioParameterBool> (pid (id::midiOut), "MIDI Out", true));
    layout.add (std::make_unique<AudioParameterBool> (pid (id::syncToHost), "Play With Host", true));
    layout.add (std::make_unique<AudioParameterBool> (pid (id::scaleLock), "Scale Lock", false));
    layout.add (std::make_unique<AudioParameterBool> (pid (id::chordTrigger), "Chord Trigger", false));
    layout.add (std::make_unique<AudioParameterBool> (pid (id::triggerSevenths), "Trigger Sevenths", false));
    // Index 0 was "Chords + Bass" before the melody existed (old projects have no melody, so nothing changes for them);
    // new choices are only ever appended so stored values keep their meaning.
    layout.add (std::make_unique<AudioParameterChoice> (pid (id::midiOutPart), "MIDI Out Part",
        StringArray { "All parts", "Chords only", "Bass only", "Melody only", "Chords + Bass" }, 0));
    layout.add (std::make_unique<AudioParameterBool> (pid (id::melodyOn), "Melody", true));
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
constexpr int kMinEditorW = 960, kMaxEditorW = 1600, kMinEditorH = 560, kMaxEditorH = 1000;   // = ChordbloomEditor limits

const char* const kStateTag = "ChordbloomState";
const char* const kProgressionTag = "Progression";
const char* const kSlotTag = "Slot";
const char* const kEditorTag = "Editor";
const char* const kMelodyTag = "Melody";
const char* const kNoteTag = "Note";
constexpr double kStateTicksPerBeat = 48.0;   // melody note times are stored as whole 1/48 beats (exact)

const char* const kAllParamIds[] = {
    cb::ParamIDs::keyRoot, cb::ParamIDs::scale, cb::ParamIDs::octave, cb::ParamIDs::voicing, cb::ParamIDs::voiceLeading,
    cb::ParamIDs::pattern, cb::ParamIDs::rate, cb::ParamIDs::bassMode, cb::ParamIDs::velocity, cb::ParamIDs::gate,
    cb::ParamIDs::swing, cb::ParamIDs::humanize, cb::ParamIDs::previewOn, cb::ParamIDs::previewTone,
    cb::ParamIDs::previewVolume, cb::ParamIDs::midiOut, cb::ParamIDs::syncToHost, cb::ParamIDs::scaleLock,
    cb::ParamIDs::chordTrigger, cb::ParamIDs::triggerSevenths, cb::ParamIDs::melodyOn
};

// MIDI-out parts that each midiOutPart choice lets through (bit 0 chords, bit 1 bass, bit 2 melody).
constexpr uint8_t kOutPartMask[] = { 0b111, 0b001, 0b010, 0b100, 0b011 };
constexpr int kNumOutPartChoices = (int) std::size (kOutPartMask);

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
    uint64_t structuralSerial = 0;     // unchanged when only velocity / gate / swing / humanize changed (no chase then)
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
    uint8_t channel = 1;   // chord previews and keyboard clicks: 1; melody auditions: 3
};

// Parameters that change what the message thread renders.
struct RenderSnapshot
{
    int keyRoot = 0, scale = 0, octave = 0, voicing = 0, pattern = 0, rate = 0, bass = 0;
    bool voiceLeading = true, melodyOn = true;
    float velocity = 0.0f, gate = 0.0f, swing = 0.0f, humanize = 0.0f;

    bool sameVoicing (const RenderSnapshot& o) const
    {
        return octave == o.octave && voicing == o.voicing && voiceLeading == o.voiceLeading;
    }

    bool operator== (const RenderSnapshot& o) const
    {
        return keyRoot == o.keyRoot && scale == o.scale && sameVoicing (o) && pattern == o.pattern && rate == o.rate
            && bass == o.bass && melodyOn == o.melodyOn && velocity == o.velocity && gate == o.gate && swing == o.swing
            && humanize == o.humanize;
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
        case ChordbloomProcessor::ExportPart::Melody: return "Melody";
        case ChordbloomProcessor::ExportPart::Both:   break;
    }
    return "Chords + Bass";
}

juce::String toJuce (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }

//==============================================================================
// Export file names. '#' is kept (F# minor stays "F#"); only what Windows forbids is removed, '/' becomes '-'.
constexpr int kMaxFileNameChars = 60;   // including ".mid"

juce::String legalFileStem (const juce::String& raw)
{
    auto s = raw.replace ("6/9", "69").replaceCharacters ("/\\", "--").removeCharacters ("<>:\"|?*");
    juce::String clean;
    for (auto p = s.getCharPointer(); ! p.isEmpty();)
    {
        const auto c = p.getAndAdvance();
        if (c >= 0x20 && c != 0x7f)
            clean += juce::String::charToString (c);
    }
    while (clean.contains ("  "))
        clean = clean.replace ("  ", " ");
    clean = clean.trim().substring (0, kMaxFileNameChars - 4).trimCharactersAtEnd (". ");
    return clean.isEmpty() ? juce::String ("Chordbloom") : clean;
}

// "A minor", "C major", "D dorian", "E harmonic minor"
juce::String shortKeyName (const cb::Key& key)
{
    juce::String scale = "major";
    if (key.scale == cb::ScaleType::NaturalMinor)
        scale = "minor";
    else if (key.scale != cb::ScaleType::Major)
        scale = juce::String (cb::scaleInfo (key.scale).name).toLowerCase();
    return toJuce (key.tonicName()) + " " + scale;
}

juce::String chordLabel (const cb::Chord& chord, const cb::Key& key)
{
    try
    {
        return legalFileStem (toJuce (chord.name (key)));
    }
    catch (...)
    {
        return "Chord";
    }
}

// "Am F C G - A minor - Chords" (the chord list is shortened with " ..." so the whole file name stays <= 60 chars)
juce::String progressionFileStem (const cb::Progression& prog, const cb::Key& key, const juce::String& partLabel)
{
    const juce::String suffix = " - " + shortKeyName (key) + (partLabel.isEmpty() ? juce::String() : " - " + partLabel);
    if (prog.empty())
        return legalFileStem ("Chordbloom" + suffix);

    juce::StringArray names;
    for (auto& slot : prog.slots)
        names.add (chordLabel (slot.chord, key));

    const int budget = juce::jmax (8, kMaxFileNameChars - 4 - suffix.length());
    auto list = names.joinIntoString (" ");
    if (list.length() > budget)
    {
        list.clear();
        for (auto& n : names)
        {
            const auto candidate = list.isEmpty() ? n : list + " " + n;
            if (candidate.length() + 4 > budget)
                break;
            list = candidate;
        }
        if (list.isEmpty())
            list = names[0].substring (0, juce::jmax (1, budget - 4));
        list << " ...";
    }
    return legalFileStem (list + suffix);
}

// Writes into <temp>/Chordbloom/<fileName>; when that file is locked (e.g. the host still reads it), a fresh sibling.
juce::File writeTempMidi (const juce::String& fileName, const juce::MidiFile& mf)
{
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Chordbloom");
    const auto file = dir.getChildFile (fileName);
    if (cb::midiexport::writeMidiFile (mf, file))
        return file;

    const auto alternative = file.getNonexistentSibling (false);
    if (cb::midiexport::writeMidiFile (mf, alternative))
        return alternative;
    return {};
}

//==============================================================================
// User defaults (last key / scale / tone picked in the editor), shared by every instance in the process.
std::mutex& userDefaultsMutex()
{
    static std::mutex m;
    return m;
}

struct UserDefaultsLocation
{
    bool overridden = false;
    juce::File file;
};

UserDefaultsLocation& userDefaultsLocation()
{
    static UserDefaultsLocation loc;
    return loc;
}

juce::PropertiesFile::Options userDefaultsOptions()
{
    // <user app data>/Chordbloom/Chordbloom Defaults.settings. Not "Chordbloom.settings": that is the Standalone
    // app's own settings file (audio setup + state), which it rewrites from memory and would drop our keys from.
    juce::PropertiesFile::Options o;
    o.applicationName = "Chordbloom Defaults";
    o.filenameSuffix = ".settings";
    o.folderName = "Chordbloom";
    o.osxLibrarySubFolder = "Application Support";
    o.commonToAllUsers = false;
    o.storageFormat = juce::PropertiesFile::storeAsXML;
    o.millisecondsBeforeSaving = -1;   // saved explicitly
    return o;
}

juce::File userDefaultsFile()
{
    const std::lock_guard<std::mutex> lock (userDefaultsMutex());
    const auto& loc = userDefaultsLocation();
    if (loc.overridden)
        return loc.file;
   #if CHORDBLOOM_DEV_TOOL
    return {};   // dev tool (selftest / snapshots) stays deterministic unless a test sets a file
   #else
    return userDefaultsOptions().getDefaultFile();
   #endif
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
        prm.midiOutPart = get (cb::ParamIDs::midiOutPart);
        prm.melodyOn = get (cb::ParamIDs::melodyOn);

        applyUserDefaults();   // before the listeners: a fresh instance simply starts there (a loaded state overrides it)

        for (auto* id : kAllParamIds)
            apvts.addParameterListener (id, this);

        combined.ensureSize (32768);
        outBuffer.ensureSize (32768);
        forcedOut.ensureSize (32768);   // room for every (channel, note) off plus the controller resets

        paletteKey = keyFromParams();
        palette = safeBuildPalette (paletteKey);
        renderedSnap = readSnapshot();
        rebuildAndPublish();
        publishGhost();

        startTimerHz (20);
    }

    ~Impl() override
    {
        stopTimer();
        cancelPendingUpdate();
        cb::ghosts::unregisterInstance (instanceNumber);
        flushUserDefaults();
        for (auto* id : kAllParamIds)
            apvts.removeParameterListener (id, this);
    }

    // One undo step / a loaded state: the progression and the melody together.
    struct SongState
    {
        cb::Progression progression;
        cb::Melody melody;
    };

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
        std::atomic<float>* midiOutPart = nullptr;
        std::atomic<float>* melodyOn = nullptr;
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
        s.melodyOn = boolValue (prm.melodyOn);
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
        checkUserDefaults();
    }

    void refreshFromParams (bool force)
    {
        const auto snap = readSnapshot();
        ensurePalette();
        const bool keyChanged = snap.keyRoot != renderedSnap.keyRoot || snap.scale != renderedSnap.scale;
        const bool changed = force || snap != renderedSnap;
        const bool voicingChanged = ! snap.sameVoicing (renderedSnap);

        // Only velocity / gate / swing / humanize changed: playback re-times the sounding notes but never strikes
        // notes mid-step (no chase), see resyncTimingOnly.
        const bool timingOnly = ! force && snap.keyRoot == renderedSnap.keyRoot && snap.scale == renderedSnap.scale
                             && snap.sameVoicing (renderedSnap) && snap.pattern == renderedSnap.pattern
                             && snap.rate == renderedSnap.rate && snap.bass == renderedSnap.bass
                             && snap.melodyOn == renderedSnap.melodyOn;

        if (changed)
        {
            renderedSnap = snap;
            rebuildAndPublish (! timingOnly);
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

    std::vector<cb::NoteEvent> renderMelodyEvents() const
    {
        const double total = (double) progression.totalBeats();
        if (total <= 0.0 || melody.empty())
            return {};

        try
        {
            return cb::renderMelody (melody, total, ChordbloomProcessor::kMelodyChannel);
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

    void rebuildAndPublish (bool structural = true)
    {
        auto data = std::make_unique<EngineData>();
        data->serial = nextSerial++;
        if (structural)
            ++structuralSerial;
        data->structuralSerial = structuralSerial;

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
            auto events = renderEvents();

            if (boolValue (prm.melodyOn))
            {
                const auto melodyEvents = renderMelodyEvents();
                events.insert (events.end(), melodyEvents.begin(), melodyEvents.end());
            }

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
    // Message thread: user defaults (I26)
    int packedPrefs() const noexcept
    {
        return intValue (prm.keyRoot, 0, 11) + 16 * intValue (prm.scale, 0, cb::numScaleTypes - 1)
             + 256 * intValue (prm.previewTone, 0, 2);
    }

    void applyUserDefaults()
    {
        const auto file = userDefaultsFile();
        if (file != juce::File() && file.existsAsFile())
        {
            juce::PropertiesFile props (file, userDefaultsOptions());
            auto apply = [&] (const char* id, int maxValue)
            {
                if (! props.containsKey (id))
                    return;
                const int v = props.getIntValue (id, -1);
                if (v < 0 || v > maxValue)
                    return;
                if (auto* param = apvts.getParameter (id))
                    param->setValueNotifyingHost (param->convertTo0to1 ((float) v));
            };
            apply (cb::ParamIDs::keyRoot, 11);
            apply (cb::ParamIDs::scale, cb::numScaleTypes - 1);
            apply (cb::ParamIDs::previewTone, 2);
        }
        prefsBaseline.store (packedPrefs());
    }

    // The editor's key / scale / tone menus report the user's own picks (rememberUserDefaults); only those are
    // written, debounced, into the user defaults file. Host automation, validators fuzzing parameters while the editor
    // is open, preset / state loads and undo never change the defaults.
    void requestUserDefaultsSave()
    {
        if ((loadGen.load() & 1) != 0)
            return;

        userPickPending = true;
        pendingPrefsTime = juce::Time::getMillisecondCounterHiRes();
    }

    void checkUserDefaults()
    {
        // The pick's parameter change lands asynchronously after the menu closes: read the values once it settled.
        if (userPickPending && juce::Time::getMillisecondCounterHiRes() - pendingPrefsTime >= 300.0)
        {
            userPickPending = false;
            const int g1 = loadGen.load();
            const int now = packedPrefs();
            if (g1 == loadGen.load() && (g1 & 1) == 0)
                pendingPrefs = now;
        }

        if (pendingPrefs >= 0)
            flushUserDefaults();
    }

    void flushUserDefaults()
    {
        if (userPickPending && (loadGen.load() & 1) == 0)   // closing right after a pick: still record it
        {
            userPickPending = false;
            pendingPrefs = packedPrefs();
        }

        if (pendingPrefs < 0)
            return;

        const int v = pendingPrefs;
        pendingPrefs = -1;
        const auto file = userDefaultsFile();
        if (file == juce::File())
            return;

        juce::PropertiesFile props (file, userDefaultsOptions());
        props.setValue (cb::ParamIDs::keyRoot, v % 16);
        props.setValue (cb::ParamIDs::scale, (v / 16) % 16);
        props.setValue (cb::ParamIDs::previewTone, v / 256);
        props.saveIfNeeded();
    }

    //==============================================================================
    // Message thread: progression editing
    void syncStateCopy()
    {
        const std::lock_guard<std::mutex> lock (stateMutex);
        stateProgression = progression;
        stateMelody = melody;
    }

    // Every instance's melody and loop length go to the process-wide ghost registry (the other instances' rolls).
    void publishGhost() { cb::ghosts::publish (instanceNumber, melody, (double) progression.totalBeats()); }

    void progressionChanged()
    {
        syncStateCopy();
        rebuildAndPublish();
        publishGhost();
        owner.sendChangeMessage();
    }

    // One undo step for a new progression and / or melody (nothing happens when both are unchanged).
    bool commitSong (const cb::Progression& proposed, const cb::Melody& proposedMelody)
    {
        auto next = sanitiseProgression (proposed);
        auto nextMelody = cb::sanitiseMelody (proposedMelody);
        if (next == progression && nextMelody == melody)
            return false;

        undoStack.push_back ({ progression, melody });
        if ((int) undoStack.size() > kMaxUndo)
            undoStack.erase (undoStack.begin());
        redoStack.clear();

        progression = std::move (next);
        melody = std::move (nextMelody);
        progressionChanged();
        return true;
    }

    bool commit (const cb::Progression& proposed) { return commitSong (proposed, melody); }

    // A structural edit: the melody moves along with its chords. sourceSlot[i] = old index of new slot i (-1 = new).
    bool commitFollowing (const cb::Progression& proposed, std::vector<int> sourceSlot)
    {
        const auto next = sanitiseProgression (proposed);
        sourceSlot.resize (next.slots.size(), -1);
        cb::Melody moved;
        try
        {
            moved = cb::melodyFollowingSlots (melody, progression, next, sourceSlot);
        }
        catch (...)
        {
            moved = melody;
        }
        return commitSong (next, moved);
    }

    //==============================================================================
    // Message thread: preview
    bool pushPreview (bool on, int note, uint8_t velocity, int channel)
    {
        if (fifo.getFreeSpace() < 1)
        {
            previewOverflow.store (true);
            return false;
        }

        const PreviewMsg msg { (uint8_t) (on ? 1 : 0), (uint8_t) juce::jlimit (0, 127, note), velocity,
                               (uint8_t) juce::jlimit (1, 16, channel) };
        fifo.write (1).forEach ([&] (int index) { previewBuffer[(size_t) index] = msg; });
        return true;
    }

    void stopPreviewNotes()
    {
        for (int n : previewNotes)
            pushPreview (false, n, 0, previewChannel);
        previewNotes.clear();
        previewChord.reset();
        ++previewSerial;   // every audition starts with a stop, so each one (and each stop) gets a fresh serial
    }

    void startPreviewNotes (const std::vector<int>& notes, int channel = ChordbloomProcessor::kChordChannel)
    {
        const auto clean = sanitiseNotes (notes, 16);
        if (clean.empty() || fifo.getFreeSpace() < (int) clean.size())
            return;

        const auto vel = channel == ChordbloomProcessor::kMelodyChannel
                             ? toMidiVelocity (cb::kDefaultMelodyVelocity)
                             : toMidiVelocity (juce::jlimit (0.05f, 1.0f, rawValue (prm.velocity, 0.75f)));
        previewChannel = channel;
        for (auto n : clean)
            if (pushPreview (true, n, vel, channel))
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

        auto* mel = root.createNewChildElement (kMelodyTag);
        mel->setAttribute ("ticksPerBeat", (int) kStateTicksPerBeat);
        {
            const std::lock_guard<std::mutex> lock (stateMutex);
            for (auto& n : stateMelody.notes)
            {
                auto* e = mel->createNewChildElement (kNoteTag);
                e->setAttribute ("note", n.note);
                e->setAttribute ("start", (int) std::lround (n.startBeat * kStateTicksPerBeat));
                e->setAttribute ("length", (int) std::lround (n.lengthBeats * kStateTicksPerBeat));
                e->setAttribute ("velocity", (double) n.velocity);
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

    static cb::Melody parseMelody (const juce::XmlElement& xml)
    {
        const double ticksPerBeat = juce::jlimit (1.0, 9600.0, xml.getDoubleAttribute ("ticksPerBeat", kStateTicksPerBeat));
        cb::Melody m;
        for (auto* e : xml.getChildWithTagNameIterator (kNoteTag))
        {
            if ((int) m.notes.size() >= cb::kMaxMelodyNotes)
                break;

            cb::MelodyNote n;
            n.note = e->getIntAttribute ("note", 72);
            n.startBeat = e->getDoubleAttribute ("start", 0.0) / ticksPerBeat;
            n.lengthBeats = e->getDoubleAttribute ("length", ticksPerBeat) / ticksPerBeat;
            n.velocity = (float) e->getDoubleAttribute ("velocity", (double) cb::kDefaultMelodyVelocity);
            m.notes.push_back (n);
        }
        return cb::sanitiseMelody (m);
    }

    void readState (const void* data, int sizeInBytes)
    {
        if (data == nullptr || sizeInBytes <= 0)
            return;

        // A loaded state never becomes the user's defaults: odd loadGen = loading; the baseline follows the state.
        struct LoadScope
        {
            explicit LoadScope (Impl& i) : impl (i) { impl.loadGen.fetch_add (1); }
            ~LoadScope()
            {
                impl.prefsBaseline.store (impl.packedPrefs());
                impl.loadGen.fetch_add (1);
            }
            Impl& impl;
        } loadScope (*this);

        const auto xml = juce::AudioProcessor::getXmlFromBinary (data, sizeInBytes);
        if (xml == nullptr)
            return;

        const juce::XmlElement* paramsXml = nullptr;
        const juce::XmlElement* progXml = nullptr;
        const juce::XmlElement* editorXml = nullptr;
        const juce::XmlElement* melodyXml = nullptr;
        bool isFullState = false;

        if (xml->hasTagName (kStateTag))
        {
            isFullState = true;
            paramsXml = xml->getChildByName (apvts.state.getType());
            progXml = xml->getChildByName (kProgressionTag);
            editorXml = xml->getChildByName (kEditorTag);
            melodyXml = xml->getChildByName (kMelodyTag);
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

        SongState loaded;
        loaded.progression = progXml != nullptr ? parseProgression (*progXml) : cb::Progression();
        loaded.melody = melodyXml != nullptr ? parseMelody (*melodyXml) : cb::Melody();   // older states: no melody

        if (juce::MessageManager::existsAndIsCurrentThread())
        {
            {
                const std::lock_guard<std::mutex> lock (stateMutex);
                pendingSong.reset();
            }
            applyLoadedSong (loaded);
        }
        else
        {
            {
                const std::lock_guard<std::mutex> lock (stateMutex);
                pendingSong = loaded;
                stateProgression = loaded.progression;
                stateMelody = loaded.melody;
            }
            triggerAsyncUpdate();
        }
    }

    void applyLoadedSong (const SongState& loaded)
    {
        progression = sanitiseProgression (loaded.progression);
        melody = cb::sanitiseMelody (loaded.melody);
        undoStack.clear();
        redoStack.clear();
        syncStateCopy();
        renderedSnap = readSnapshot();
        ensurePalette();
        rebuildAndPublish();
        publishGhost();
        owner.sendChangeMessage();
    }

    void applyPendingState()
    {
        std::optional<SongState> pending;
        {
            const std::lock_guard<std::mutex> lock (stateMutex);
            pending.swap (pendingSong);
        }
        if (pending.has_value())
            applyLoadedSong (*pending);
    }

    //==============================================================================
    // Audio thread
    struct Voice
    {
        double endT = 0.0;        // timeline beats
        double startT = 0.0;      // timeline beat of the event that started it
        double pendingT = -1.0;   // >= 0: a timing-only re-render moved this note's start later, to pendingT; it keeps
                                  // sounding and the scan must not strike it a second time there
        uint8_t note = 0;
        uint8_t channel = 1;
    };

    // MIDI-out parts for the midiOutPart filter: playback bass is "bass"; playback melody and melody auditions are
    // "melody"; playback chords, other previews and live input are "chords".
    static constexpr uint8_t kPartChords = 0;
    static constexpr uint8_t kPartBass = 1;
    static constexpr uint8_t kPartMelody = 2;
    static constexpr int kNumParts = 3;

    struct LogicalEvent
    {
        int sample = 0;
        int seq = 0;
        uint8_t on = 0;
        uint8_t channel = 1;
        uint8_t note = 0;
        uint8_t velocity = 0;
        uint8_t part = kPartChords;
        uint8_t passthroughOnly = 0;   // unmatched live note-off: forwarded only when nothing else holds the note
        uint8_t raw = 0;               // live non-note message (CC, bend, program change, SysEx ...) passed through
        int rawSize = 0;
        const uint8_t* rawData = nullptr;   // points into the block's input MidiBuffer (unchanged until the flush)
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
        double hostOffset = 0.0;          // Host mode: timeline = ppq + hostOffset (grows by the loop length per host loop)
        uint64_t dataSerial = 0;
        uint64_t structuralSerial = 0;
        uint32_t seenLocalGen = 0;
        bool loopWasEmpty = true;
        bool forcePanic = false;
        bool prevMidiOut = true;
        bool prevPreviewOn = true;
        bool wasBypassed = false;         // the previous block was processBlockBypassed (hostOn etc. = what passed through)
        int outPart = 0;                  // midiOutPart choice (kOutPartMask): 0 = all parts, 1 = chords, 2 = bass,
                                          // 3 = melody, 4 = chords + bass

        // timing of the current block (for beat -> sample conversion)
        int numSamples = 0;
        double blockT0 = 0.0;
        double beatsPerSample = 0.0;

        std::array<Voice, kMaxVoices> voices {};
        int numVoices = 0;

        std::array<LogicalEvent, kMaxBlockEvents> events {};
        int numEvents = 0;
        int seq = 0;

        // Built-in synth stream (every source)
        std::array<std::array<uint16_t, 128>, 16> ref {};     // how many sources hold (channel, note)
        std::array<uint8_t, 128> notesOnPerPitch {};          // number of channels with ref > 0, per note

        // MIDI-out stream (only the parts midiOutPart lets through): holders per part, and how many of them were sent
        std::array<std::array<std::array<uint16_t, 128>, 16>, kNumParts> partHeld {};
        std::array<std::array<std::array<uint16_t, 128>, 16>, kNumParts> outCounted {};

        std::array<std::array<bool, 128>, 16> hostOn {};      // what the host has been sent
        std::array<bool, 16> sustainDown {}, sostenutoDown {}, bendOffCentre {};   // forwarded controller state
        std::array<uint8_t, 128> previewHeld {};              // preview notes: the MIDI channel holding each (0 = none)
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

    static uint8_t playbackPart (int channel) noexcept
    {
        if (channel == ChordbloomProcessor::kBassChannel)
            return kPartBass;
        return channel == ChordbloomProcessor::kMelodyChannel ? kPartMelody : kPartChords;
    }

    bool partAllowed (int part) const noexcept
    {
        return part >= 0 && part < kNumParts && ((kOutPartMask[(size_t) au.outPart] >> part) & 1) != 0;
    }

    // How many parts other than `exceptPart` have sent (channel index, note) to the MIDI out.
    int countedByOthers (int exceptPart, int chIndex, int note) const noexcept
    {
        int n = 0;
        for (int p = 0; p < kNumParts; ++p)
            if (p != exceptPart)
                n += au.outCounted[(size_t) p][(size_t) chIndex][(size_t) note];
        return n;
    }

    bool emitOn (int channel, int note, int velocity, int sample, uint8_t part = kPartChords) noexcept
    {
        if (au.numEvents >= kMaxBlockEvents - kOffReserve || note < 0 || note > 127)
            return false;

        auto& e = au.events[(size_t) au.numEvents++];
        e.sample = sample;
        e.seq = au.seq++;
        e.on = 1;
        e.channel = (uint8_t) channel;
        e.note = (uint8_t) note;
        e.velocity = (uint8_t) juce::jlimit (1, 127, velocity);
        e.part = part;
        e.passthroughOnly = 0;
        e.raw = 0;
        return true;
    }

    void emitOff (int channel, int note, int sample, uint8_t part = kPartChords, bool passthroughOnly = false) noexcept
    {
        if (note < 0 || note > 127)
            return;

        if (au.numEvents >= kMaxBlockEvents)
        {
            au.forcePanic = true;   // could not record the note-off: silence everything next block
            return;
        }

        auto& e = au.events[(size_t) au.numEvents++];
        e.sample = sample;
        e.seq = au.seq++;
        e.on = 0;
        e.channel = (uint8_t) channel;
        e.note = (uint8_t) note;
        e.velocity = 0;
        e.part = part;
        e.passthroughOnly = (uint8_t) (passthroughOnly ? 1 : 0);
        e.raw = 0;
    }

    // F6: a live non-note message takes its place in the (sample, seq) order with the block's note events, so
    // "note-off, then pedal down" at one sample stays in that order. Never refused: when the event list is full it is
    // written straight away (its order relative to same-sample notes is then the old one, but it is never dropped).
    void emitRaw (const uint8_t* data, int size, int sample) noexcept
    {
        if (data == nullptr || size <= 0)
            return;

        if (au.numEvents >= kMaxBlockEvents - kOffReserve)
        {
            combined.addEvent (data, size, sample);
            if (partAllowed (kPartChords))
                outBuffer.addEvent (data, size, sample);
            return;
        }

        auto& e = au.events[(size_t) au.numEvents++];
        e.sample = sample;
        e.seq = au.seq++;
        e.on = 0;
        e.channel = 1;
        e.note = 0;
        e.velocity = 0;
        e.part = kPartChords;
        e.passthroughOnly = 0;
        e.raw = 1;
        e.rawSize = size;
        e.rawData = data;
    }

    static void addRaw (juce::MidiBuffer& dest, uint8_t status, uint8_t d1, uint8_t d2, int sample) noexcept
    {
        const uint8_t bytes[3] = { status, d1, d2 };
        dest.addEvent (bytes, 3, sample);
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

    // Turns the block's logical events into two balanced note streams: `combined` (every source, drives the built-in
    // synth) and `outBuffer` (only the parts midiOutPart lets through, becomes the MIDI out).
    void flushLogicalEvents() noexcept
    {
        std::sort (au.events.begin(), au.events.begin() + au.numEvents,
                   [] (const LogicalEvent& a, const LogicalEvent& b)
                   { return a.sample != b.sample ? a.sample < b.sample : a.seq < b.seq; });

        for (int i = 0; i < au.numEvents; ++i)
        {
            const auto& e = au.events[(size_t) i];

            if (e.raw != 0)
            {
                // Live input other than notes: passes through untouched (counts as "chords" for midiOutPart).
                combined.addEvent (e.rawData, e.rawSize, e.sample);
                if (partAllowed (kPartChords))
                    outBuffer.addEvent (e.rawData, e.rawSize, e.sample);
                continue;
            }

            const int chIndex = juce::jlimit (0, 15, (int) e.channel - 1);
            auto& r = au.ref[(size_t) chIndex][e.note];
            const auto onStatus = (uint8_t) (0x90 | chIndex);
            const auto offStatus = (uint8_t) (0x80 | chIndex);

            const int outSum = countedByOthers (-1, chIndex, e.note);

            if (e.passthroughOnly != 0)
            {
                // A live note-off whose note-on was never seen (key held before load / bypass, or a second off from
                // overlapping piano-roll notes): pass it on, but never cut a note another source holds.
                if (r == 0)
                    addRaw (combined, offStatus, e.note, 0, e.sample);
                if (outSum == 0 && partAllowed (kPartChords))
                    addRaw (outBuffer, offStatus, e.note, 0, e.sample);
                continue;
            }

            // Built-in synth stream
            if (e.on != 0)
            {
                if (r > 0)
                    addRaw (combined, offStatus, e.note, 0, e.sample);   // retrigger: keep the stream balanced
                else
                    ++au.notesOnPerPitch[e.note];

                addRaw (combined, onStatus, e.note, e.velocity, e.sample);
                if (r < 0xffff)
                    ++r;
            }
            else if (r > 0 && --r == 0)
            {
                addRaw (combined, offStatus, e.note, 0, e.sample);
                if (au.notesOnPerPitch[e.note] > 0)
                    --au.notesOnPerPitch[e.note];
            }

            // MIDI-out stream
            const int part = juce::jlimit (0, kNumParts - 1, (int) e.part);
            auto& held = au.partHeld[(size_t) part][(size_t) chIndex][e.note];
            auto& counted = au.outCounted[(size_t) part][(size_t) chIndex][e.note];

            if (e.on != 0)
            {
                if (held < 0xffff)
                    ++held;

                if (partAllowed (part))
                {
                    if (outSum > 0)
                        addRaw (outBuffer, offStatus, e.note, 0, e.sample);
                    addRaw (outBuffer, onStatus, e.note, e.velocity, e.sample);
                    if (counted < 0xffff)
                        ++counted;
                }
            }
            else
            {
                if (held > 0)
                    --held;
                if (counted > held)
                {
                    --counted;
                    if (outSum == 1)
                        addRaw (outBuffer, offStatus, e.note, 0, e.sample);
                }
            }
        }

        au.numEvents = 0;
    }

    // midiOutPart changed: close the MIDI-out notes of the parts that are no longer sent (their holders keep sounding
    // in the built-in synth); newly allowed parts are sent from their next note-on.
    void applyOutPart (int newPart) noexcept
    {
        if (newPart == au.outPart)
            return;

        const bool chordsWereSent = partAllowed (kPartChords);
        au.outPart = newPart;

        for (int part = 0; part < kNumParts; ++part)
        {
            if (partAllowed (part))
                continue;

            for (int ch = 0; ch < 16; ++ch)
            {
                for (int n = 0; n < 128; ++n)
                {
                    auto& counted = au.outCounted[(size_t) part][(size_t) ch][(size_t) n];
                    if (counted == 0)
                        continue;
                    counted = 0;
                    if (countedByOthers (part, ch, n) == 0)
                        addRaw (outBuffer, (uint8_t) (0x80 | ch), (uint8_t) n, 0, 0);
                }
            }
        }

        if (chordsWereSent && ! partAllowed (kPartChords))
            resetForwardedControllers (outBuffer);   // live CCs stop being forwarded: release a held pedal / bend
    }

    void resetForwardedControllers (juce::MidiBuffer& dest) noexcept
    {
        for (int ch = 0; ch < 16; ++ch)
        {
            const auto cc = (uint8_t) (0xb0 | ch);
            if (au.sustainDown[(size_t) ch])
                addRaw (dest, cc, 64, 0, 0);
            if (au.sostenutoDown[(size_t) ch])
                addRaw (dest, cc, 66, 0, 0);
            if (au.bendOffCentre[(size_t) ch])
                addRaw (dest, (uint8_t) (0xe0 | ch), 0x00, 0x40, 0);
        }
        au.sustainDown.fill (false);
        au.sostenutoDown.fill (false);
        au.bendOffCentre.fill (false);
    }

    // Keeps track of what the host has been sent: open notes and held pedals / bent pitch per channel.
    void trackForwarded (const juce::MidiBuffer& sent) noexcept
    {
        for (const auto meta : sent)
        {
            if (meta.numBytes < 3 || meta.data[0] >= 0xf0)
                continue;
            const uint8_t type = (uint8_t) (meta.data[0] & 0xf0);
            const auto ch = (size_t) (meta.data[0] & 0x0f);
            const uint8_t d1 = meta.data[1] & 0x7f;
            const uint8_t d2 = meta.data[2] & 0x7f;

            if (type == 0x90 && d2 > 0)
                au.hostOn[ch][d1] = true;
            else if (type == 0x80 || type == 0x90)
                au.hostOn[ch][d1] = false;
            else if (type == 0xb0 && d1 == 64)
                au.sustainDown[ch] = d2 >= 64;
            else if (type == 0xb0 && d1 == 66)
                au.sostenutoDown[ch] = d2 >= 64;
            else if (type == 0xb0 && d1 == 121)
                au.sustainDown[ch] = au.sostenutoDown[ch] = au.bendOffCentre[ch] = false;
            else if (type == 0xe0)
                au.bendOffCentre[ch] = ! (d1 == 0 && d2 == 0x40);
        }
    }

    void panicAll() noexcept
    {
        for (int ch = 0; ch < 16; ++ch)
        {
            for (int n = 0; n < 128; ++n)
            {
                if (au.ref[(size_t) ch][(size_t) n] > 0)
                    addRaw (combined, (uint8_t) (0x80 | ch), (uint8_t) n, 0, 0);
                au.ref[(size_t) ch][(size_t) n] = 0;

                if (countedByOthers (-1, ch, n) > 0)
                    addRaw (outBuffer, (uint8_t) (0x80 | ch), (uint8_t) n, 0, 0);
                for (size_t part = 0; part < (size_t) kNumParts; ++part)
                {
                    au.outCounted[part][(size_t) ch][(size_t) n] = 0;
                    au.partHeld[part][(size_t) ch][(size_t) n] = 0;
                }
            }
        }

        au.notesOnPerPitch.fill (0);
        au.numVoices = 0;
        au.previewHeld.fill (0);
        for (auto& e : au.input)
            e = {};
        au.lastTriggerKey = -1;
        publishTrigger (-1);
        au.mode = PlayMode::None;
        au.hostOffset = 0.0;
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
                const int channel = juce::jlimit (1, 16, (int) m.channel);
                auto& held = au.previewHeld[(size_t) n];

                if (m.on != 0)
                {
                    if (held != 0)
                        emitOff (held, n, 0, playbackPart (held));
                    held = emitOn (channel, n, m.velocity, 0, playbackPart (channel)) ? (uint8_t) channel : (uint8_t) 0;
                }
                else if (held != 0)
                {
                    emitOff (held, n, 0, playbackPart (held));
                    held = 0;
                }
            });
        }

        if (previewOverflow.exchange (false))
        {
            for (int n = 0; n < 128; ++n)
            {
                if (const int held = au.previewHeld[(size_t) n]; held != 0)
                {
                    emitOff (held, n, 0, playbackPart (held));
                    au.previewHeld[(size_t) n] = 0;
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

    // B26: publishes the exact notes of the chord-trigger key last pressed (au.lastTriggerKey) for the keyboard guide,
    // then the degree (stored last, so a reader that sees a degree also sees its notes or a newer set).
    void publishTrigger (int degree) noexcept
    {
        uint64_t lo = 0, hi = 0;

        if (au.lastTriggerKey >= 0 && au.lastTriggerKey < (int) au.input.size())
        {
            const auto& entry = au.input[(size_t) au.lastTriggerKey];

            for (int i = 0; i < (int) entry.count; ++i)
            {
                const int n = entry.notes[(size_t) i] & 0x7f;

                if (n < 64)
                    lo |= (uint64_t) 1 << n;
                else
                    hi |= (uint64_t) 1 << (n - 64);
            }
        }

        triggerLo.store (lo);
        triggerHi.store (hi);
        triggeredDegree.store (degree);
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
        publishTrigger (degree);
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
            publishTrigger (degree);
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
            emitOff (channel, note, sample, kPartChords, true);   // never saw the note-on: pass-through only
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
                releaseInputChannel (channel, sample);   // all sound off / all notes off (their offs come first)

            emitRaw (raw, size, sample);   // everything else passes through untouched, in input order (F6)
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

    void stopVoice (int index, int sample) noexcept
    {
        const auto& v = au.voices[(size_t) index];
        emitOff (v.channel, v.note, sample, playbackPart (v.channel));
        removeVoice (index);
    }

    void releaseAllVoices (int sample) noexcept
    {
        for (int i = au.numVoices; --i >= 0;)
            stopVoice (i, sample);
    }

    // Host relocation: notes struck before `t` (timeline beats) stop now, notes struck just before the jump stay
    // candidates for "still covered at the new position" in resyncVoices (no double attack).
    void releaseVoicesStartedBefore (double t) noexcept
    {
        for (int i = au.numVoices; --i >= 0;)
            if (au.voices[(size_t) i].startT < t)
                stopVoice (i, 0);
    }

    void retireVoices (double t, bool inclusive) noexcept
    {
        for (int i = au.numVoices; --i >= 0;)
        {
            const auto& v = au.voices[(size_t) i];
            if (inclusive ? v.endT <= t : v.endT < t)
                stopVoice (i, beatToSample (v.endT));
        }
    }

    void startVoice (const EngineEvent& e, double startT, double endT, int sample) noexcept
    {
        const int existing = findVoice (e.note, e.channel);
        if (existing >= 0)
            stopVoice (existing, sample);

        if (au.numVoices >= kMaxVoices)
            return;

        if (! emitOn (e.channel, e.note, e.velocity, sample, playbackPart (e.channel)))
            return;

        auto& v = au.voices[(size_t) au.numVoices++];
        v.endT = juce::jmax (endT, startT + 1.0e-6);
        v.startT = startT;
        v.pendingT = -1.0;
        v.note = e.note;
        v.channel = e.channel;
    }

    // Re-establishes the voices at timeline position t for (possibly new) data d: voices still covered by an event
    // with the same note keep sounding (no retrigger), the others get note-offs, and with `chase` notes that should be
    // sounding at t but are not are started. Without chase (timing-only changes are handled by resyncTimingOnly)
    // nothing new is struck mid-step.
    void resyncVoices (const EngineData& d, double t, bool chase) noexcept
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
                    v.startT = base + e.start;
                    v.pendingT = -1.0;
                    break;
                }
            }

            if (! covered)
                stopVoice (i, 0);
        }

        if (! chase)
            return;

        for (const auto& e : d.events)
        {
            if (e.start >= pos)
                break;
            if (e.end - pos >= kMinChaseBeats && findVoice (e.note, e.channel) < 0)
                startVoice (e, base + e.start, base + e.end, 0);
        }
    }

    // Velocity / gate / swing / humanize changed (B7): every sounding voice is matched to the same note's event whose
    // start is nearest to the voice's own start (swing and humanize move starts by at most ~1/3 of a step). The voice
    // takes the event's new end (a shorter gate may end it right now); a start that moved past the playhead is
    // remembered so the scan does not strike the note again. Nothing is chased: notes that already ended under the old
    // gate are not struck mid-step.
    void resyncTimingOnly (const EngineData& d, double t) noexcept
    {
        if (t < 0.0 || d.totalBeats <= 0.0)
        {
            releaseAllVoices (0);
            return;
        }

        constexpr double kMatchBeats = 0.4;
        const double L = d.totalBeats;
        const double base = std::floor (t / L) * L;
        const double pos = t - base;

        for (int i = au.numVoices; --i >= 0;)
        {
            auto& v = au.voices[(size_t) i];
            const EngineEvent* best = nullptr;
            double bestDist = kMatchBeats;

            for (const auto& e : d.events)
            {
                if (e.start > pos + kMatchBeats)
                    break;
                if (e.note != v.note || e.channel != v.channel)
                    continue;
                const double dist = std::abs (base + e.start - v.startT);
                if (dist < bestDist)
                {
                    bestDist = dist;
                    best = &e;
                }
            }

            if (best == nullptr || base + best->end <= t)
            {
                stopVoice (i, 0);
                continue;
            }

            v.endT = base + best->end;
            v.startT = base + best->start;
            v.pendingT = v.startT > t ? v.startT : -1.0;
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

                const int existing = findVoice (it->note, it->channel);
                if (existing >= 0)
                {
                    auto& v = au.voices[(size_t) existing];
                    if (v.pendingT >= 0.0 && std::abs (v.pendingT - t) < 1.0e-7)
                    {
                        v.pendingT = -1.0;   // already sounding since before a swing / humanize tweak moved it here
                        continue;
                    }
                }

                startVoice (*it, t, base + it->end, beatToSample (t));
            }
        }
    }

    // A host loop the progression runs straight through: shorter than the progression (FL pattern mode with a short
    // pattern) or a whole number of progression lengths (the position inside the progression does not change).
    static bool loopContinues (double loopBeats, double totalBeats) noexcept
    {
        if (! (loopBeats > 0.0) || ! (totalBeats > 0.0))
            return false;
        if (loopBeats < totalBeats - 1.0e-4)
            return true;
        const double k = std::round (loopBeats / totalBeats);
        return k >= 1.0 && std::abs (loopBeats - k * totalBeats) < 1.0e-4;
    }

    // The host went back by `span` beats while playing. Returns the host loop length when this is a loop wrap the
    // progression continues across (B4 / B6), 0 for a relocation (the playhead then follows the host's ppq again).
    // A wrap lands on the loop start (when the host reports loop points) or on a bar line, and jumps back by the loop
    // length / a whole number of bars (within a block, for hosts that do not split the block at the loop end).
    static double hostLoopWrap (double span, double ppq, double tol, bool haveLoop, double loopStart, double loopEnd,
                                double barBeats, double totalBeats) noexcept
    {
        if (! (span > 0.0))
            return 0.0;

        double loopBeats = 0.0;
        if (haveLoop)
        {
            loopBeats = loopEnd - loopStart;
            if (ppq < loopStart - 0.05 || ppq > loopStart + tol || std::abs (span - loopBeats) > tol)
                return 0.0;
        }
        else
        {
            const double intoBar = ppq - std::floor (ppq / barBeats) * barBeats;
            const bool atBarLine = intoBar <= tol || barBeats - intoBar <= 0.05;
            const double bars = std::round (span / barBeats);
            if (! atBarLine || bars < 1.0 || std::abs (span - bars * barBeats) > tol)
                return 0.0;
            loopBeats = bars * barBeats;
        }

        return loopContinues (loopBeats, totalBeats) ? loopBeats : 0.0;
    }

    void runPlayback (const EngineData* d, bool syncOn, int numSamples) noexcept
    {
        // Host transport
        bool playing = false;
        double bpm = au.lastBpm;
        bool havePpq = false;
        double ppq = 0.0;
        bool haveLoop = false;
        double loopStart = 0.0, loopEnd = 0.0, barBeats = 4.0;
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

                if (pos->getIsLooping())
                {
                    if (const auto lp = pos->getLoopPoints())
                    {
                        if (std::isfinite (lp->ppqStart) && std::isfinite (lp->ppqEnd) && lp->ppqEnd - lp->ppqStart > 1.0e-3)
                        {
                            haveLoop = true;
                            loopStart = lp->ppqStart;
                            loopEnd = lp->ppqEnd;
                        }
                    }
                }

                if (const auto ts = pos->getTimeSignature())
                    if (ts->numerator > 0 && ts->denominator > 0)
                        barBeats = juce::jlimit (0.25, 64.0, ts->numerator * 4.0 / ts->denominator);
            }
        }

        au.lastBpm = bpm;
        hostBpm.store (bpm);
        hostPlaying.store (playing);

        bool localOn = localPlayback.load();
        if (au.mode == PlayMode::Host && ! playing && localOn)
        {
            // The host stopped while following it: a Play left on in Chordbloom stops too instead of starting its own
            // loop (B5). Lock-free store; the lane's transport poll updates the button.
            localPlayback.store (false);
            localOn = false;
        }

        const uint32_t gen = localGen.load();
        const PlayMode desired = (playing && syncOn) ? PlayMode::Host : (localOn ? PlayMode::Local : PlayMode::None);

        const bool dataChanged = d != nullptr && d->serial != au.dataSerial;
        const bool structural = d != nullptr && d->structuralSerial != au.structuralSerial;
        if (d != nullptr)
        {
            au.dataSerial = d->serial;
            au.structuralSerial = d->structuralSerial;
        }

        bool restart = false;
        if (desired != au.mode)
        {
            releaseAllVoices (0);
            au.mode = desired;
            restart = true;
            if (desired == PlayMode::Local)
                au.localClock = 0.0;
            if (desired == PlayMode::Host)
                hostLoopBeats.store (0.0);
            au.hostOffset = 0.0;
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
        const double tol = 0.05 + blockBeats;
        const double L = d != nullptr ? d->totalBeats : 0.0;

        double t0 = 0.0;
        if (au.mode == PlayMode::Host)
        {
            t0 = havePpq ? ppq + au.hostOffset : au.expectedT;
            if (havePpq && ! restart && std::abs (t0 - au.expectedT) > tol)
            {
                const double wrap = L > 0.0 ? hostLoopWrap (au.expectedT - t0, ppq, tol, haveLoop, loopStart, loopEnd,
                                                            barBeats, L)
                                            : 0.0;
                if (wrap > 0.0)
                {
                    // Host loop: the progression keeps running seamlessly (nothing released, nothing re-struck).
                    au.hostOffset += wrap;
                    t0 = ppq + au.hostOffset;
                    hostLoopBeats.store (wrap < L - 1.0e-4 ? wrap : 0.0);
                }
                else
                {
                    // Relocation: follow the host's position. Notes struck in the previous block stay candidates for
                    // "still covered" (hosts that do not split the block at their loop end), older ones stop now.
                    releaseVoicesStartedBefore (au.expectedT - tol);
                    au.hostOffset = 0.0;
                    hostLoopBeats.store (0.0);   // the progression restarts with the host: no continued host loop now
                    t0 = ppq;
                    restart = true;
                }
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
            if (restart || structural)
                resyncVoices (*d, t0, true);
            else
                resyncTimingOnly (*d, t0);
            scanFrom = t0;
        }

        // The host's loop end falls inside this block and the wrap will be a relocation: stop at the loop end, so the
        // progression position after it is not struck for a few milliseconds before the host jumps back (B6).
        double scanTo = t1;
        if (au.mode == PlayMode::Host && haveLoop && ! loopContinues (loopEnd - loopStart, L))
        {
            const double loopEndT = loopEnd + au.hostOffset;
            if (t0 < loopEndT && loopEndT < t1)
                scanTo = loopEndT;
        }

        scanEvents (*d, scanFrom, scanTo);
        retireVoices (t1, false);

        au.expectedT = scanTo;
        if (au.mode == PlayMode::Local)
            au.localClock = t1;

        // Position display
        if (t0 >= 0.0)
        {
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
    // F5: first block after a host bypass. hostOn / the pedal state now describe what the host received during the
    // bypass (input passed through untouched). Keys still held are adopted as live input, so their release sends
    // exactly one note-off and playback of the same pitch retriggers instead of double-striking. When live input is
    // not forwarded now (MIDI Out off, or Bass only), they are closed instead and a pedal / bend that passed through
    // is released - like switching MIDI Out off (B8) or to Bass only.
    void endBypass (bool midiOutNow) noexcept
    {
        au.wasBypassed = false;
        const bool forwardLive = midiOutNow && partAllowed (kPartChords);

        for (int ch = 0; ch < 16; ++ch)
        {
            for (int n = 0; n < 128; ++n)
            {
                if (! au.hostOn[(size_t) ch][(size_t) n])
                    continue;

                if (forwardLive)
                {
                    auto& entry = au.input[(size_t) (ch * 128 + n)];
                    if (entry.count > 0 || entry.degree >= 0)
                        continue;
                    entry.count = 1;
                    entry.outChannel = (uint8_t) (ch + 1);
                    entry.degree = -1;
                    entry.notes[0] = (uint8_t) n;
                    // The built-in synth never heard it (ref stays 0); the MIDI out holds it once, as chords.
                    au.partHeld[0][(size_t) ch][(size_t) n] = 1;
                    au.outCounted[0][(size_t) ch][(size_t) n] = 1;
                }
                else
                {
                    addRaw (forcedOut, (uint8_t) (0x80 | ch), (uint8_t) n, 0, 0);
                    au.hostOn[(size_t) ch][(size_t) n] = false;
                }
            }
        }

        if (! forwardLive)
            resetForwardedControllers (forcedOut);
    }

    void process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) noexcept
    {
        const int numSamples = juce::jmax (0, buffer.getNumSamples());
        const int numChannels = buffer.getNumChannels();

        for (int ch = 0; ch < numChannels; ++ch)
            buffer.clear (ch, 0, numSamples);

        combined.clear();
        outBuffer.clear();
        forcedOut.clear();
        au.numEvents = 0;
        au.seq = 0;
        au.numSamples = numSamples;

        const bool previewOnNow = boolValue (prm.previewOn);
        const bool midiOutNow = boolValue (prm.midiOut);
        const bool syncOn = boolValue (prm.syncToHost);
        const bool scaleLockOn = boolValue (prm.scaleLock);
        const bool chordTriggerOn = boolValue (prm.chordTrigger);
        const bool seventhsOn = boolValue (prm.triggerSevenths);
        const int outPartNow = intValue (prm.midiOutPart, 0, kNumOutPartChoices - 1);

        const EngineData* d = acquireEngineData();

        if (panicRequested.exchange (false) || au.forcePanic)
        {
            au.forcePanic = false;
            panicAll();
        }

        applyOutPart (outPartNow);
        if (au.wasBypassed)
            endBypass (midiOutNow);
        drainPreview();
        handleInput (midi, d, scaleLockOn, chordTriggerOn, seventhsOn);

        // A 0-sample call (e.g. the VST3 wrapper's parameter flush) has no usable transport (its play head can be
        // zeroed and would read as a stop): playback just carries on with the next real block. Previews and live
        // input are handled as usual, and the synth below applies them, so it stays in step with the MIDI out (B1).
        if (numSamples > 0)
            runPlayback (d, syncOn, numSamples);

        flushLogicalEvents();

        // Built-in sound. render() also runs for 0-sample blocks: it then applies the note on/offs without rendering.
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
        au.prevPreviewOn = previewOnNow;

        if (numChannels > 0 && numSamples > 0)
        {
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

        // MIDI output
        midi.clear();
        midi.addEvents (forcedOut, 0, -1, 0);   // F5: closes what passed through a bypass, even with MIDI Out off
        if (midiOutNow)
        {
            midi.addEvents (outBuffer, 0, -1, 0);
            trackForwarded (outBuffer);
        }
        else if (au.prevMidiOut)
        {
            // MIDI out was just switched off: close every note the host has been sent, then release a held sustain /
            // sostenuto pedal and centre a bent pitch, so the routed instrument is not left sustaining (B8).
            for (int ch = 0; ch < 16; ++ch)
            {
                for (int n = 0; n < 128; ++n)
                {
                    if (au.hostOn[(size_t) ch][(size_t) n])
                    {
                        addRaw (midi, (uint8_t) (0x80 | ch), (uint8_t) n, 0, 0);
                        au.hostOn[(size_t) ch][(size_t) n] = false;
                    }
                }
            }
            resetForwardedControllers (midi);
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
    // note-offs for every note the host was sent (on the first bypassed block), all internal note state reset
    // (playback chases again afterwards), previews queued meanwhile are dropped. Input MIDI passes through untouched
    // (JUCE's default bypass behaviour) and is tracked like any forwarded MIDI (F5), so the first block after the
    // bypass knows which keys and pedals the host still holds (endBypass).
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
        outBuffer.clear();
        au.numEvents = 0;
        au.seq = 0;
        au.numSamples = numSamples;
        panicAll();
        combined.clear();   // panicAll's note-offs are replaced by the exact host-side ones below
        outBuffer.clear();
        playingSlot.store (-1);
        playbackBeat.store (0.0);

        synth.allNotesOff (true);
        synth.render (buffer, emptyMidi, numSamples);

        if (numChannels > 0 && numSamples > 0)
        {
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

        // First bypassed block: note-offs for every note the host was sent, placed before this block's input, so a
        // new input note-on at sample 0 for the same pitch is not cancelled by its own cleanup (B17). Later bypassed
        // blocks: hostOn only holds what passed through, which the host closes itself.
        if (! au.wasBypassed)
        {
            for (int ch = 0; ch < 16; ++ch)
            {
                for (int n = 0; n < 128; ++n)
                {
                    if (au.hostOn[(size_t) ch][(size_t) n])
                    {
                        addRaw (combined, (uint8_t) (0x80 | ch), (uint8_t) n, 0, 0);
                        au.hostOn[(size_t) ch][(size_t) n] = false;
                    }
                }
            }
        }

        if (! combined.isEmpty())
        {
            combined.addEvents (midi, 0, -1, 0);   // input lands after the offs at equal timestamps
            midi.clear();
            midi.addEvents (combined, 0, -1, 0);
            combined.clear();
        }

        trackForwarded (midi);   // F5: what passes through while bypassed is what the host holds afterwards
        au.wasBypassed = true;

        publishSoundingNotes();
    }

    //==============================================================================
    ChordbloomProcessor& owner;
    juce::AudioProcessorValueTreeState apvts;
    ParamPointers prm;

    // Message thread
    cb::Progression progression;
    cb::Melody melody;
    std::vector<SongState> undoStack, redoStack;
    const int instanceNumber = cb::ghosts::registerInstance();
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
    std::optional<cb::Chord> lastPreviewVoicingChord;     // chord + settings lastPreviewVoicing was voiced for
    cb::VoicingSettings lastPreviewVoicingSettings;
    std::optional<cb::Chord> previewChord;
    int previewChannel = ChordbloomProcessor::kChordChannel;   // channel of the notes in previewNotes
    uint32_t previewSerial = 0;   // message thread: changes with every preview start / stop (getPreviewSerial)
    uint64_t structuralSerial = 0;

    // User defaults
    std::atomic<int> prefsBaseline { -1 };
    std::atomic<int> loadGen { 0 };
    int pendingPrefs = -1;
    bool userPickPending = false;
    double pendingPrefsTime = 0.0;

    // State shared with host threads (never touched by the audio thread)
    std::mutex stateMutex;
    cb::Progression stateProgression;
    cb::Melody stateMelody;
    std::optional<SongState> pendingSong;
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
    std::atomic<uint64_t> triggerLo { 0 }, triggerHi { 0 };   // B26: notes of the held chord-trigger chord (bit = MIDI note)
    std::atomic<bool> panicRequested { false };
    std::atomic<double> hostLoopBeats { 0.0 };

    // Audio thread
    AudioState au;
    PreviewSynth synth;
    juce::MidiBuffer combined;    // everything generated this block -> built-in synth
    juce::MidiBuffer outBuffer;   // what midiOutPart lets through -> MIDI out
    juce::MidiBuffer forcedOut;   // F5: always sent (closes keys / pedals that passed through a bypass)
    juce::MidiBuffer emptyMidi;
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
    impl->outBuffer.ensureSize ((size_t) juce::jmax (32768, samplesPerBlock * 16));
    impl->forcedOut.ensureSize (32768);
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

    // Appending leaves the melody where it is (notes drawn past the end now play over the new chord); inserting
    // moves the notes of the later chords along with them.
    if (insertIndex < 0 || insertIndex >= (int) p.slots.size())
    {
        p.slots.push_back (slot);
        impl->commit (p);
        return true;
    }

    std::vector<int> source;
    for (int i = 0; i < (int) p.slots.size(); ++i)
        source.push_back (i);
    source.insert (source.begin() + insertIndex, -1);

    p.slots.insert (p.slots.begin() + insertIndex, slot);
    impl->commitFollowing (p, source);
    return true;
}

void ChordbloomProcessor::removeSlot (int index)
{
    auto p = impl->progression;
    if (index < 0 || index >= (int) p.slots.size())
        return;

    std::vector<int> source;
    for (int i = 0; i < (int) p.slots.size(); ++i)
        if (i != index)
            source.push_back (i);

    p.slots.erase (p.slots.begin() + index);
    impl->commitFollowing (p, source);
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

    std::vector<int> source;
    for (int i = 0; i < n; ++i)
        source.push_back (i);
    source.erase (source.begin() + fromIndex);
    source.insert (source.begin() + toIndex, fromIndex);

    const auto slot = p.slots[(size_t) fromIndex];
    p.slots.erase (p.slots.begin() + fromIndex);
    p.slots.insert (p.slots.begin() + toIndex, slot);
    impl->commitFollowing (p, source);
}

void ChordbloomProcessor::updateSlot (int index, const cb::ProgressionSlot& slot)
{
    auto p = impl->progression;
    if (index < 0 || index >= (int) p.slots.size())
        return;

    const bool lengthChanged = sanitiseSlot (slot).lengthBeats != p.slots[(size_t) index].lengthBeats;
    p.slots[(size_t) index] = sanitiseSlot (slot);

    if (! lengthChanged)
    {
        impl->commit (p);
        return;
    }

    // A new length moves the notes of the later chords; notes that no longer fit this chord go.
    std::vector<int> source;
    for (int i = 0; i < (int) p.slots.size(); ++i)
        source.push_back (i);
    impl->commitFollowing (p, source);
}

bool ChordbloomProcessor::duplicateSlot (int index)
{
    auto p = impl->progression;
    if (index < 0 || index >= (int) p.slots.size() || (int) p.slots.size() >= cb::kMaxSlots)
        return false;

    std::vector<int> source;
    for (int i = 0; i < (int) p.slots.size(); ++i)
        source.push_back (i);
    source.insert (source.begin() + index + 1, index);   // the copy gets the original's notes too

    const auto copy = p.slots[(size_t) index];
    p.slots.insert (p.slots.begin() + index + 1, copy);
    impl->commitFollowing (p, source);
    return true;
}

void ChordbloomProcessor::clearProgression() { impl->commit ({}); }

bool ChordbloomProcessor::undo()
{
    auto& i = *impl;
    if (i.undoStack.empty())
        return false;

    i.redoStack.push_back ({ i.progression, i.melody });
    if ((int) i.redoStack.size() > kMaxUndo)
        i.redoStack.erase (i.redoStack.begin());

    i.progression = i.undoStack.back().progression;
    i.melody = i.undoStack.back().melody;
    i.undoStack.pop_back();
    i.progressionChanged();
    return true;
}

bool ChordbloomProcessor::redo()
{
    auto& i = *impl;
    if (i.redoStack.empty())
        return false;

    i.undoStack.push_back ({ i.progression, i.melody });
    if ((int) i.undoStack.size() > kMaxUndo)
        i.undoStack.erase (i.undoStack.begin());

    i.progression = i.redoStack.back().progression;
    i.melody = i.redoStack.back().melody;
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
const cb::Melody& ChordbloomProcessor::getMelody() const { return impl->melody; }
void ChordbloomProcessor::setMelody (const cb::Melody& melody) { impl->commitSong (impl->progression, melody); }
void ChordbloomProcessor::clearMelody() { setMelody ({}); }
std::vector<cb::NoteEvent> ChordbloomProcessor::renderMelodyEvents() const { return impl->renderMelodyEvents(); }

std::vector<ChordbloomProcessor::GhostMelody> ChordbloomProcessor::getGhostMelodies() const
{
    std::vector<GhostMelody> result;
    for (auto& e : cb::ghosts::othersThan (impl->instanceNumber))
        result.push_back ({ e.instanceNumber, std::move (e.melody), e.loopBeats });
    return result;
}

uint64_t ChordbloomProcessor::getGhostSerial() const noexcept { return cb::ghosts::serial(); }
int ChordbloomProcessor::getInstanceNumber() const noexcept { return impl->instanceNumber; }

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
    i.lastPreviewVoicingChord = c;
    i.lastPreviewVoicingSettings = i.voicingFromParams();
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
    i.lastPreviewVoicingChord = i.progression.slots[(size_t) slotIndex].chord;
    i.lastPreviewVoicingSettings = i.voicingFromParams();
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

void ChordbloomProcessor::startPreviewMelodyNote (int midiNote)
{
    auto& i = *impl;
    i.stopPreviewNotes();
    if (midiNote < 0 || midiNote > 127)
        return;
    i.startPreviewNotes ({ midiNote }, kMelodyChannel);
}

void ChordbloomProcessor::stopPreview() { impl->stopPreviewNotes(); }

std::optional<cb::Chord> ChordbloomProcessor::getPreviewChord() const { return impl->previewChord; }
uint32_t ChordbloomProcessor::getPreviewSerial() const { return impl->previewSerial; }
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
double ChordbloomProcessor::getHostLoopBeats() const noexcept { return impl->hostLoopBeats.load(); }

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

std::vector<int> ChordbloomProcessor::getTriggeredNotes() const
{
    std::vector<int> notes;

    if (impl->triggeredDegree.load() < 0)
        return notes;

    const uint64_t lo = impl->triggerLo.load(), hi = impl->triggerHi.load();

    for (int n = 0; n < 128; ++n)
        if (((n < 64 ? lo >> n : hi >> (n - 64)) & 1u) != 0)
            notes.push_back (n);

    return notes;
}

//==============================================================================
juce::String ChordbloomProcessor::getExportFileName (ExportPart part) const
{
    return progressionFileStem (impl->progression, getKey(), partName (part)) + ".mid";
}

juce::File ChordbloomProcessor::writeMidiToTempFile (ExportPart part) const
{
    if (impl->progression.empty() || (part == ExportPart::Melody && renderMelodyEvents().empty()))
        return {};

    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Chordbloom");
    const auto file = dir.getChildFile (getExportFileName (part));

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

    if (part == ExportPart::Melody)
    {
        const auto melodyEvents = renderMelodyEvents();
        if (melodyEvents.empty())
            return false;

        const auto file = cb::midiexport::buildMelodyMidiFile (melodyEvents, getHostBpm(), 960);
        return cb::midiexport::writeMidiFile (withLoopLength (file, (double) impl->progression.totalBeats()), destination);
    }

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

//==============================================================================
// Single-chord drags: chord notes only (channel 1), with the current pattern / gate / swing / humanize applied.
juce::File ChordbloomProcessor::writeChordMidiToTempFile (const cb::Chord& chordIn) const
{
    auto& i = *impl;
    const auto chord = sanitiseChord (chordIn);
    const auto vs = i.voicingFromParams();

    // The voicing the pad preview just played (the drag starts with a press), otherwise voiced like a new preview.
    std::vector<int> notes;
    if (i.lastPreviewVoicingChord == std::optional<cb::Chord> (chord) && sameVoicingSettings (i.lastPreviewVoicingSettings, vs)
        && ! i.lastPreviewVoicing.empty())
    {
        notes = i.lastPreviewVoicing;
    }
    else
    {
        try
        {
            notes = cb::voiceChord (chord, vs, -1, 0, i.lastPreviewVoicing);
        }
        catch (...)
        {
            notes.clear();
        }
    }

    if (notes.empty())
        return {};

    cb::Progression one;
    cb::ProgressionSlot slot;
    slot.chord = chord;
    slot.lengthBeats = 4;
    one.slots.push_back (slot);

    std::vector<cb::NoteEvent> events;
    try
    {
        events = cb::renderProgression (one, { notes }, i.renderFromParams());
    }
    catch (...)
    {
        events.clear();
    }

    const bool anyChordNote = std::any_of (events.begin(), events.end(),
                                           [] (const cb::NoteEvent& e) { return e.channel != kBassChannel; });
    if (! anyChordNote)
        return {};

    const auto mf = cb::midiexport::buildMidiFile (events, getHostBpm(), true, false, 960);
    return writeTempMidi (chordLabel (chord, getKey()) + " - " + shortKeyName (getKey()) + ".mid",
                          withLoopLength (mf, (double) slot.lengthBeats));
}

juce::File ChordbloomProcessor::writeSlotMidiToTempFile (int slotIndex) const
{
    auto& i = *impl;
    if (slotIndex < 0 || slotIndex >= (int) i.progression.slots.size())
        return {};

    // Rendered as part of the whole progression, so voice leading, bass and humanize match playback exactly.
    const double start = i.progression.slotStartBeat (slotIndex);
    const double end = i.progression.slotStartBeat (slotIndex + 1);
    std::vector<cb::NoteEvent> events;
    for (auto e : renderProgressionEvents())
    {
        if (e.startBeat < start - 1.0e-9 || e.startBeat >= end - 1.0e-9)
            continue;
        e.startBeat = juce::jmax (0.0, e.startBeat - start);
        e.lengthBeats = juce::jmin (e.lengthBeats, (end - start) - e.startBeat);
        if (e.lengthBeats > 0.0)
            events.push_back (e);
    }

    const bool anyChordNote = std::any_of (events.begin(), events.end(),
                                           [] (const cb::NoteEvent& e) { return e.channel != kBassChannel; });
    if (! anyChordNote)
        return {};

    const auto& chord = i.progression.slots[(size_t) slotIndex].chord;
    const auto mf = cb::midiexport::buildMidiFile (events, getHostBpm(), true, false, 960);
    return writeTempMidi (chordLabel (chord, getKey()) + " - " + shortKeyName (getKey()) + ".mid",
                          withLoopLength (mf, end - start));
}

//==============================================================================
void ChordbloomProcessor::transposeProgression (int semitones)
{
    auto p = impl->progression;
    if (p.empty() || cb::pc (semitones) == 0)
        return;

    for (auto& slot : p.slots)
        slot.chord.root = cb::pc (slot.chord.root + semitones);
    impl->commitSong (p, cb::transposeMelody (impl->melody, semitones));   // one undo step, re-renders, sendChangeMessage
}

int ChordbloomProcessor::bestTranspositionForKey() const
{
    const auto& prog = impl->progression;
    if (prog.empty())
        return 0;

    const auto key = getKey();
    auto diatonicCount = [&] (int shift)
    {
        int n = 0;
        for (auto& slot : prog.slots)
        {
            cb::Chord c = slot.chord;
            c.root = cb::pc (c.root + shift);
            try
            {
                if (cb::isDiatonic (c, key))
                    ++n;
            }
            catch (...) {}
        }
        return n;
    };

    // Most diatonic slots wins; ties: the first chord lands on the tonic, then the smaller move, then upwards.
    const int current = diatonicCount (0);
    int best = 0, bestCount = current;
    bool bestTonic = false;
    for (int shift = -5; shift <= 6; ++shift)
    {
        if (shift == 0)
            continue;

        const int count = diatonicCount (shift);
        if (count <= current)
            continue;

        const bool tonic = cb::pc (prog.slots[0].chord.root + shift) == key.tonic;
        bool better = false;
        if (best == 0 || count > bestCount)
            better = true;
        else if (count == bestCount)
        {
            if (tonic != bestTonic)
                better = tonic;
            else if (std::abs (shift) != std::abs (best))
                better = std::abs (shift) < std::abs (best);
            else
                better = shift > best;
        }

        if (better)
        {
            best = shift;
            bestCount = count;
            bestTonic = tonic;
        }
    }
    return best;
}

//==============================================================================
void ChordbloomProcessor::rememberUserDefaults() { impl->requestUserDefaultsSave(); }

void ChordbloomProcessor::setUserDefaultsFile (const juce::File& file)
{
    const std::lock_guard<std::mutex> lock (userDefaultsMutex());
    auto& loc = userDefaultsLocation();
    loc.overridden = true;
    loc.file = file;
}

#if ! CHORDBLOOM_DEV_TOOL
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new ChordbloomProcessor(); }
#endif
