#include "SelfTestUtil.h"

#include <array>
#include <atomic>
#include <iterator>
#include <thread>

// Engine selftests (owner: WP4; final-spec 8.2 / 9 WP4): layers, arp, bass on / off, custom layers, user picks, follow,
// state, export, generator, PreviewSynth, fuzz and load. Registered in runSelfTest (SelfTest.cpp) by WP0.
//
// Checks that need a core function another work package implements (WP1 generator, WP2 arp / bass patterns, WP3
// NoteLimits / layers / follow) go through checkAfter: while that core function is still a WP0 stub (detected at run
// time below) the check is printed as DEFER and not counted; once the code is merged every such check counts like any
// other. After the integration merge the summary line must read "0 deferred".

// Dev tool hook defined in PluginProcessor.cpp (CHORDBLOOM_DEV_TOOL builds only, not in the frozen header): sees every
// MIDI buffer the processor hands to its PreviewSynth; midi == nullptr = the synth released / dropped every voice.
namespace cbdev
{
using SynthStreamObserver = void (*) (void* context, const ChordbloomProcessor& processor, const juce::MidiBuffer* midi);
void setSynthStreamObserver (SynthStreamObserver observer, void* context) noexcept;
}

using namespace cbtest;

namespace
{
namespace id = cb::ParamIDs;
using Layer = cb::Layer;
using Part = ChordbloomProcessor::ExportPart;

constexpr juce::int64 kAnyTime = std::numeric_limits<juce::int64>::max();
constexpr int kChords = ChordbloomProcessor::kChordChannel;
constexpr int kBass = ChordbloomProcessor::kBassChannel;
constexpr int kMel = ChordbloomProcessor::kMelodyChannel;
constexpr int kArp = ChordbloomProcessor::kArpChannel;

//==============================================================================
// Which other work packages' core code is live in this build (a WP0 stub answers these probes differently).
struct Deps
{
    bool arp = false;        // WP2 renderArp
    bool patterns = false;   // WP2 bass pattern modes
    bool layers = false;     // WP3 notesFromEvents (auto display notes)
    bool limits = false;     // WP3 NoteLimits overloads
    bool follow = false;     // WP3 layerNotesFollowing
    bool edit = false;       // WP3 editedSlots / replaceSlotNotes
    bool gen = false;        // WP1 generateMelody
};

Deps detectDeps()
{
    Deps d;
    const auto prog = makeProgression ({ C_, Am_, F_, G_ });
    const auto voicings = cb::voiceProgression (prog, cb::VoicingSettings());

    d.arp = ! cb::renderArp (prog, voicings, cb::ArpSettings(), cb::RenderSettings(), {}, kArp).empty();

    cb::RenderSettings rolling;
    rolling.bass = cb::BassMode::Rolling16;
    int bassEvents = 0;
    for (auto& e : cb::renderProgression (prog, voicings, rolling))
        bassEvents += e.channel == kBass ? 1 : 0;
    d.patterns = bassEvents > 8;

    cb::NoteEvent e;
    e.note = 60;
    e.startBeat = 0.0;
    e.lengthBeats = 1.0;
    e.channel = 1;
    d.layers = ! cb::notesFromEvents ({ e }, cb::kMelodyLimits).empty();

    cb::Melody low;
    low.notes.push_back (melodyNote (28, 0.0, 1.0));
    const auto lowSan = cb::sanitiseMelody (low, cb::layerLimits (Layer::Bass));
    d.limits = lowSan.notes.size() == 1 && lowSan.notes[0].note == 28;

    cb::LayerFollowContext ctx;
    ctx.before = prog;
    ctx.after = makeProgression ({ Am_, F_, G_ });
    ctx.sourceSlot = { 1, 2, 3 };
    ctx.beforeBassPc = { 0, 9, 5, 7 };
    ctx.afterBassPc = { 9, 5, 7 };
    cb::Melody one;
    one.notes.push_back (melodyNote (45, 4.5, 0.5));
    const auto moved = cb::layerNotesFollowing (one, Layer::Bass, ctx);
    d.follow = moved.notes.size() == 1 && std::abs (moved.notes[0].startBeat - 0.5) < 1e-9;
    d.edit = cb::editedSlots (one, cb::Melody(), prog) == std::vector<bool> { false, true, false, false };

    d.gen = ! cb::generateMelody (prog, cb::Key(), cb::MelodyGenContext(), 1, cb::MelodyGenSettings()).empty();
    return d;
}

int numDeferred = 0;

// A check that needs another work package's core code: counted when `available`, otherwise printed as deferred.
void checkAfter (bool available, const char* needs, bool ok, const std::string& name, const std::string& detail = {})
{
    if (available)
    {
        check (ok, name, detail);
        return;
    }
    ++numDeferred;
    std::cout << "DEFER " << name << " (after merge: needs " << needs << "; " << (ok ? "passes" : "fails")
              << " on the stubs)\n";
}

//==============================================================================
// Helpers
void setParamAsUser (ChordbloomProcessor& p, const char* paramId, float plainValue)
{
    if (auto* param = p.getAPVTS().getParameter (paramId))
    {
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
        param->endChangeGesture();
    }
}

// Undo depth, measured by undoing everything and redoing it again (the song ends where it started).
int undoDepth (ChordbloomProcessor& p)
{
    int n = 0;
    while (p.undo())
        ++n;
    for (int i = 0; i < n; ++i)
        p.redo();
    return n;
}

bool sameEvents (const std::vector<cb::NoteEvent>& a, const std::vector<cb::NoteEvent>& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].note != b[i].note || a[i].channel != b[i].channel || a[i].startBeat != b[i].startBeat
            || a[i].lengthBeats != b[i].lengthBeats || a[i].velocity != b[i].velocity)
            return false;
    return true;
}

std::vector<cb::NoteEvent> eventsOnChannel (const std::vector<cb::NoteEvent>& events, int channel)
{
    std::vector<cb::NoteEvent> out;
    for (auto& e : events)
        if (e.channel == channel)
            out.push_back (e);
    return out;
}

cb::Melody layerNotes (Layer layer, std::initializer_list<cb::MelodyNote> notes)
{
    cb::Melody m;
    m.notes = notes;
    return cb::sanitiseMelody (m, cb::layerLimits (layer));
}

cb::Melody notesStartingIn (const cb::Melody& m, double from, double to)
{
    cb::Melody out;
    for (auto& n : m.notes)
        if (n.startBeat >= from - 1e-9 && n.startBeat < to - 1e-9)
            out.notes.push_back (n);
    return out;
}

// Missing note-ons / note-offs of one rendered loop in the recorded output (like checkLoopAgainstRender, as a count).
int loopMismatches (const std::vector<Ev>& evs, const std::vector<cb::NoteEvent>& expected, double spb, double loopStart,
                    std::string* firstMiss = nullptr)
{
    int misses = 0;
    for (auto& e : expected)
    {
        const auto onAt = (juce::int64) std::floor ((loopStart + e.startBeat) * spb);
        const auto offAt = (juce::int64) std::floor ((loopStart + e.startBeat + e.lengthBeats) * spb);
        const auto* on = findEvent (evs, true, e.channel, e.note, onAt);
        const auto* off = findEvent (evs, false, e.channel, e.note, offAt);
        if (on == nullptr || off == nullptr || on->velocity != juce::jlimit (1, 127, juce::roundToInt (e.velocity * 127.0f)))
        {
            if (misses++ == 0 && firstMiss != nullptr)
                *firstMiss = str ("ch", e.channel, " n", e.note, " @", e.startBeat);
        }
    }
    return misses;
}

std::unique_ptr<juce::XmlElement> stateXml (ChordbloomProcessor& p)
{
    juce::MemoryBlock block;
    p.getStateInformation (block);
    return juce::AudioProcessor::getXmlFromBinary (block.getData(), (int) block.getSize());
}

void loadXml (ChordbloomProcessor& p, const juce::XmlElement& xml)
{
    juce::MemoryBlock block;
    juce::AudioProcessor::copyXmlToBinary (xml, block);
    p.setStateInformation (block.getData(), (int) block.getSize());
}

std::vector<const juce::XmlElement*> layerElements (const juce::XmlElement& state)
{
    std::vector<const juce::XmlElement*> out;
    for (auto* e : state.getChildWithTagNameIterator ("Layer"))
        out.push_back (e);
    return out;
}

juce::MidiFile readMidi (const juce::File& f)
{
    juce::MidiFile mf;
    juce::FileInputStream in (f);
    if (in.openedOk())
        mf.readFrom (in);
    return mf;
}

juce::String trackNameOf (const juce::MidiFile& mf, int track)
{
    if (track >= mf.getNumTracks())
        return {};
    for (auto* h : *mf.getTrack (track))
        if (h->message.isTrackNameEvent())
            return h->message.getTextFromTextMetaEvent();
    return {};
}

int noteOnsIn (const juce::MidiFile& mf, int track, int channel = 0)
{
    int n = 0;
    if (track < mf.getNumTracks())
        for (auto* h : *mf.getTrack (track))
            if (h->message.isNoteOn() && (channel == 0 || h->message.getChannel() == channel))
                ++n;
    return n;
}

double endOfTrack (const juce::MidiFile& mf, int track)
{
    if (track < mf.getNumTracks())
        for (auto* h : *mf.getTrack (track))
            if (h->message.isEndOfTrackMetaEvent())
                return h->message.getTimeStamp();
    return -1.0;
}

// Runs the rig until the host position (in beats) reaches `beat` (absolute ppq; the rigs here never jump, so the
// recorded sample of host beat b is b * samplesPerBeat).
std::vector<Ev> runUntil (Rig& rig, double beat)
{
    std::vector<Ev> out;
    for (int guard = 0; rig.head.ppq < beat && guard < 100000; ++guard)
    {
        auto evs = rig.processBlock();
        out.insert (out.end(), evs.begin(), evs.end());
    }
    return out;
}

std::vector<Ev> runBeats (Rig& rig, double beats) { return runUntil (rig, rig.head.ppq + beats); }

// A custom arp line for the own-worktree checks (custom notes play without WP2): 16 short notes plus one long note.
cb::Melody customArpLine()
{
    cb::Melody m;
    for (int i = 0; i < 16; ++i)
        m.notes.push_back (melodyNote (72 + (i % 4) * 3, (double) i, 0.5, 0.7f));
    m.notes.push_back (melodyNote (95, 0.0, 8.0, 0.6f));
    return cb::sanitiseMelody (m, cb::layerLimits (Layer::Arp));
}

cb::Melody customBassLine()   // 36..72 so it survives the melody-range stubs too
{
    return layerNotes (Layer::Bass, { melodyNote (40, 0.0, 2.0), melodyNote (52, 2.0, 1.0), melodyNote (45, 4.0, 2.0),
                                      melodyNote (41, 8.0, 3.0), melodyNote (43, 12.0, 2.0), melodyNote (55, 14.0, 1.5) });
}

//==============================================================================
// WP0 (contract review): getLayerSerial is a real counter from WP0 on. It moves on every re-render: commits, undo /
// redo, state loads and parameter-only changes that the layer UI shows (arpOn here), so the UI never goes stale.
void testLayerSerialContract()
{
    ChordbloomProcessor p;
    pumpMessages (30);
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));

    const auto s0 = p.getLayerSerial();
    setParam (p, cb::ParamIDs::arpOn, 1.0f);
    pumpMessages (80);
    const auto s1 = p.getLayerSerial();
    check (s1 != s0, "layer serial: changes after a parameter-only change (arpOn)");

    p.setMelody (makeMelody ({ melodyNote (72, 0.0, 1.0) }));
    const auto s2 = p.getLayerSerial();
    check (s2 != s1, "layer serial: changes synchronously on a melody commit");

    p.undo();
    check (p.getLayerSerial() != s2, "layer serial: changes on undo");

    const auto s3 = p.getLayerSerial();
    juce::MemoryBlock state;
    p.getStateInformation (state);
    p.setStateInformation (state.getData(), (int) state.getSize());
    check (p.getLayerSerial() != s3, "layer serial: changes on a state load");
}

// getLayerSerial moves on parameter-driven auto re-renders that send no change message, and on bassOn / bassMode.
void testLayerSerialParams()
{
    ChordbloomProcessor p;
    pumpMessages (30);
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
    ChangeCounter changes;
    p.addChangeListener (&changes);

    const std::pair<const char*, float> edits[] = { { id::arpShape, 4.0f }, { id::arpRate, 1.0f }, { id::arpRhythm, 2.0f },
                                                    { id::arpOctave, 1.0f }, { id::arpGate, 0.4f }, { id::bassOn, 0.0f },
                                                    { id::bassMode, 6.0f }, { id::arpOn, 1.0f } };
    bool allMoved = true;
    std::string which;
    for (auto& [paramId, value] : edits)
    {
        const auto before = p.getLayerSerial();
        setParam (p, paramId, value);
        pumpMessages (60);
        if (p.getLayerSerial() == before)
        {
            allMoved = false;
            which += std::string (paramId) + " ";
        }
    }
    check (allMoved, "layer serial: moves after every layer parameter change (arp*, bassOn, bassMode)", which);
    check (changes.count == 0, "layer serial: those parameter re-renders sent no change message (the serial is the signal)",
           str (changes.count, " change messages"));
    p.removeChangeListener (&changes);
}

//==============================================================================
void testParameters()
{
    ChordbloomProcessor p;
    check (p.getParameters().size() == 29, "params: 29 parameters", str (p.getParameters().size()));

    auto* bass = dynamic_cast<juce::AudioParameterChoice*> (p.getAPVTS().getParameter (id::bassMode));
    bool bassNames = bass != nullptr && bass->choices.size() == 15;
    for (int i = 0; bassNames && i < 15; ++i)
        bassNames = bass->choices[i] == cb::bassModeName ((cb::BassMode) i);
    check (bassNames && bass->choices[0] == "Off" && bass->choices[1] == "Root" && bass->choices[2] == "Root + 5th"
               && bass->choices[3] == "Octaves" && bass->choices[4] == "Pulse" && bass->getIndex() == 1,
           "params: bassMode has 15 names (0..4 unchanged), default Root");

    auto* part = dynamic_cast<juce::AudioParameterChoice*> (p.getAPVTS().getParameter (id::midiOutPart));
    check (part != nullptr && part->choices.size() == 6 && part->choices[5] == "Arp only" && part->choices[0] == "All parts"
               && part->choices[4] == "Chords + Bass",
           "params: midiOutPart has 6 choices, \"Arp only\" at 5");

    auto choiceNamesMatch = [&] (const char* paramId, int count, const std::function<juce::String (int)>& nameOf, int def)
    {
        auto* c = dynamic_cast<juce::AudioParameterChoice*> (p.getAPVTS().getParameter (paramId));
        bool ok = c != nullptr && c->choices.size() == count && c->getIndex() == def;
        for (int i = 0; ok && i < count; ++i)
            ok = c->choices[i] == nameOf (i);
        return ok;
    };
    check (choiceNamesMatch (id::arpShape, 17, [] (int i) { return juce::String (cb::arpShapeName ((cb::ArpShape) i)); }, 0),
           "params: arpShape = 17 arpShapeName choices, default Up 3");
    check (choiceNamesMatch (id::arpRhythm, 7, [] (int i) { return juce::String (cb::arpRhythmName ((cb::ArpRhythm) i)); }, 0),
           "params: arpRhythm = 7 arpRhythmName choices, default Every step");
    check (choiceNamesMatch (id::arpRate, 4, [] (int i) { return juce::String (cb::rateDivisionName ((cb::RateDivision) i)); }, 2),
           "params: arpRate = 4 rate choices, default 1/16");

    auto rangeOf = [&] (const char* paramId, const juce::String& name, float lo, float hi, float def)
    {
        auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p.getAPVTS().getParameter (paramId));
        if (rp == nullptr)
            return false;
        const auto& r = rp->getNormalisableRange();
        return rp->getName (64) == name && std::abs (r.start - lo) < 1e-6f && std::abs (r.end - hi) < 1e-6f
            && std::abs (rp->convertFrom0to1 (rp->getDefaultValue()) - def) < 1e-6f;
    };
    check (rangeOf (id::arpOn, "Arp", 0.0f, 1.0f, 0.0f) && rangeOf (id::bassOn, "Bass On", 0.0f, 1.0f, 1.0f),
           "params: arpOn \"Arp\" default off, bassOn \"Bass On\" default on");
    check (rangeOf (id::arpOctave, "Arp Octave", -2.0f, 2.0f, 0.0f) && rangeOf (id::arpGate, "Arp Gate", 0.05f, 1.0f, 0.9f)
               && rangeOf (id::arpShape, "Arp Shape", 0.0f, 16.0f, 0.0f) && rangeOf (id::arpRate, "Arp Rate", 0.0f, 3.0f, 2.0f)
               && rangeOf (id::arpRhythm, "Arp Rhythm", 0.0f, 6.0f, 0.0f),
           "params: arp octave -2..2, gate 0.05..1 (0.9), shape / rate / rhythm ranges and names");
    if (auto* gate = p.getAPVTS().getParameter (id::arpGate))
        check (gate->getText (gate->getValue(), 16) == "90%", "params: arpGate shows a percentage",
               gate->getText (gate->getValue(), 16).toStdString());

    const auto arp = p.getArpSettings();
    check (arp.shape == cb::ArpShape::Up3 && arp.rate == cb::RateDivision::Sixteenth && arp.rhythm == cb::ArpRhythm::EveryStep
               && arp.centreNote == 65 && std::abs (arp.gate - 0.9f) < 1e-6f,
           "params: default arp settings (Up 3, 1/16, every step, centre 65, gate 0.9)");
    setParam (p, id::arpOctave, 2.0f);
    setParam (p, id::octave, 3.0f);
    check (p.getArpSettings().centreNote == 96, "params: arp centre = voicing centre + 5 + 12 * arpOctave, clamped to 96",
           str (p.getArpSettings().centreNote));
}

//==============================================================================
// A hand-written phase-1.5 (version 1) state: loads, sounds identical, nothing on channel 4, re-saved without <Layer>.
void testGoldenV1State()
{
    const char* v1 = R"(<?xml version="1.0" encoding="UTF-8"?>
<ChordbloomState version="1">
  <Params>
    <PARAM id="keyRoot" value="9.0"/>
    <PARAM id="scale" value="1.0"/>
    <PARAM id="octave" value="0.0"/>
    <PARAM id="voicing" value="1.0"/>
    <PARAM id="voiceLeading" value="1.0"/>
    <PARAM id="pattern" value="2.0"/>
    <PARAM id="rate" value="1.0"/>
    <PARAM id="bassMode" value="3.0"/>
    <PARAM id="velocity" value="0.800000011920929"/>
    <PARAM id="gate" value="0.699999988079071"/>
    <PARAM id="swing" value="0.300000011920929"/>
    <PARAM id="humanize" value="0.5"/>
    <PARAM id="previewOn" value="1.0"/>
    <PARAM id="previewTone" value="1.0"/>
    <PARAM id="previewVolume" value="-12.0"/>
    <PARAM id="midiOut" value="1.0"/>
    <PARAM id="syncToHost" value="1.0"/>
    <PARAM id="scaleLock" value="0.0"/>
    <PARAM id="chordTrigger" value="0.0"/>
    <PARAM id="triggerSevenths" value="0.0"/>
    <PARAM id="midiOutPart" value="0.0"/>
    <PARAM id="melodyOn" value="1.0"/>
  </Params>
  <Progression>
    <Slot root="9" quality="1" length="4" inversion="-1" octave="0"/>
    <Slot root="5" quality="10" length="4" inversion="-1" octave="0"/>
    <Slot root="0" quality="0" length="2" inversion="1" octave="0"/>
    <Slot root="7" quality="12" length="6" inversion="-1" octave="-1"/>
  </Progression>
  <Melody ticksPerBeat="48">
    <Note note="76" start="0" length="24" velocity="0.800000011920929"/>
    <Note note="72" start="48" length="48" velocity="0.6000000238418579"/>
    <Note note="74" start="144" length="16" velocity="0.800000011920929"/>
    <Note note="79" start="672" length="96" velocity="0.800000011920929"/>
    <Note note="81" start="816" length="48" velocity="0.800000011920929"/>
  </Melody>
  <Editor width="1100" height="690"/>
</ChordbloomState>)";

    const auto xml = juce::parseXML (juce::String (v1));
    ChordbloomProcessor p;
    if (xml != nullptr)
        loadXml (p, *xml);
    pumpMessages (30);

    const auto& prog = p.getProgression();
    check (xml != nullptr && prog.slots.size() == 4 && prog.slots[2].inversion == 1 && prog.slots[3].octaveShift == -1
               && p.getMelody().notes.size() == 5 && p.getMelody().notes[4].startBeat == 17.0,
           "golden v1: progression and melody (incl. a parked note) load");
    check (getParam (p, id::arpOn) < 0.5f && getParam (p, id::bassOn) > 0.5f && getParam (p, id::arpShape) == 0.0f
               && getParam (p, id::arpRate) == 2.0f && std::abs (getParam (p, id::arpGate) - 0.9f) < 1e-6f,
           "golden v1: new parameters take their defaults (arp off, bass on)");
    check (! p.isLayerCustom (Layer::Arp) && ! p.isLayerCustom (Layer::Bass), "golden v1: both layers auto");

    const auto rs = p.getRenderSettings();
    check (rs.pattern == cb::RhythmPattern::Pulse && rs.bass == cb::BassMode::Octaves && std::abs (rs.gate - 0.7f) < 1e-6f
               && std::abs (rs.humanize - 0.5f) < 1e-6f,
           "golden v1: loaded render settings");
    const auto expected = cb::renderProgression (prog, cb::voiceProgression (prog, p.getVoicingSettings()), rs);
    check (sameEvents (p.renderProgressionEvents(), expected),
           "golden v1: renderProgressionEvents == renderProgression with the loaded parameters (bit-identical)");
    check (sameEvents (p.renderLayerEvents (Layer::Bass), eventsOnChannel (expected, kBass)),
           "golden v1: the bass layer plays renderProgression's bass");

    Rig rig (p);
    rig.head.playing = true;
    const double spb = rig.samplesPerBeat();
    runUntil (rig, 16.5);
    auto loop = p.renderProgressionEvents();
    const auto mel = p.renderMelodyEvents();
    loop.insert (loop.end(), mel.begin(), mel.end());
    checkLoopAgainstRender (rig.all, loop, spb, 0.0, 0, "golden v1 loop (chords + bass + melody)");
    check (countOns (rig.all, 0, (juce::int64) (16.0 * spb) - 2) == (int) loop.size() && countOns (rig.all, 0, kAnyTime, kArp) == 0,
           "golden v1: the loop has exactly the rendered notes, nothing on channel 4",
           str (countOns (rig.all, 0, (juce::int64) (16.0 * spb) - 2), " vs ", loop.size()));
    rig.head.playing = false;
    rig.run (2);
    check (rig.openCount() == 0 && rig.doubleOns == 0, "golden v1: balanced");

    const auto resaved = stateXml (p);
    check (resaved != nullptr && layerElements (*resaved).empty() && resaved->getIntAttribute ("version") == 2
               && resaved->getChildByName ("Melody") != nullptr,
           "golden v1: the re-saved state (version 2) has no <Layer>");
}

//==============================================================================
// Custom bass playback, bass on / off (2.3), reset and bass export.
void testBassLayer (const Deps& d)
{
    ChordbloomProcessor p;
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
    pumpMessages (30);
    const auto autoBassEvents = p.renderLayerEvents (Layer::Bass);
    const auto custom = customBassLine();
    const auto customEvents = cb::renderMelody (custom, 16.0, kBass, cb::layerLimits (Layer::Bass));

    const int depth0 = undoDepth (p);
    p.setLayerNotes (Layer::Bass, custom);
    check (p.isLayerCustom (Layer::Bass) && p.getLayerNotes (Layer::Bass) == custom && undoDepth (p) == depth0 + 1,
           "bass layer: setLayerNotes makes it custom (one undo step)");
    checkAfter (d.edit && d.layers, "WP3 editedSlots", p.getEditedSlots (Layer::Bass) == std::vector<bool> (4, true),
                "bass layer: every slot of a hand-written line counts as edited");
    check (sameEvents (p.renderLayerEvents (Layer::Bass), customEvents), "bass layer: renderLayerEvents = the custom notes on channel 2");
    {
        const auto all = p.renderProgressionEvents();
        check (eventsOnChannel (all, kBass).size() == custom.notes.size() && all.size() > custom.notes.size(),
               "bass layer: renderProgressionEvents = chords + the custom bass");
    }

    Rig rig (p);
    rig.head.playing = true;
    const double spb = rig.samplesPerBeat();
    runUntil (rig, 16.5);
    checkLoopAgainstRender (onChannel (rig.all, kBass), customEvents, spb, 0.0, 0, "custom bass loop");
    check (countOns (rig.all, 0, (juce::int64) (16.0 * spb) - 2, kBass) == (int) custom.notes.size(),
           "bass layer: plays exactly the custom notes");

    // bassMode automation while custom: nothing audible changes (custom wins), the layer stays custom
    setParam (p, id::bassMode, (float) cb::BassMode::Pulse);
    pumpMessages (60);
    {
        const double base = std::ceil (rig.head.ppq / 16.0) * 16.0 + 16.0;
        runUntil (rig, base - 0.1);   // stop before the loop start: the block holding it must be recorded
        rig.all.clear();
        runUntil (rig, base + 16.5);
        check (p.isLayerCustom (Layer::Bass) && loopMismatches (rig.all, customEvents, spb, base) == 0
                   && sameEvents (p.renderLayerEvents (Layer::Bass), customEvents),
               "bass layer: a bassMode change while custom changes nothing audible");
    }

    // bassOn off: no channel 2 in playback, the content (export) stays
    setParam (p, id::bassOn, 0.0f);
    pumpMessages (60);
    rig.all.clear();
    runBeats (rig, 16.5);
    check (countOns (rig.all, 0, kAnyTime, kBass) == 0 && rig.openNotes (kBass).empty() && ! p.isLayerOn (Layer::Bass),
           "bass on/off: bassOn off -> no channel 2 in playback");
    check (p.renderLayerEvents (Layer::Bass).size() == custom.notes.size(), "bass on/off: the content stays while muted");
    {
        const auto bassFile = p.writeMidiToTempFile (Part::Bass);
        const auto mf = readMidi (bassFile);
        check (bassFile.existsAsFile() && noteOnsIn (mf, 1, kBass) == (int) custom.notes.size(),
               "bass on/off: the Bass export ignores bassOn (custom notes)");
        const auto allFile = p.writeMidiToTempFile (Part::All);
        const auto all = readMidi (allFile);
        bool bassTrack = false;
        for (int t = 0; t < all.getNumTracks(); ++t)
            bassTrack = bassTrack || trackNameOf (all, t) == "Chordbloom Bass";
        check (allFile.existsAsFile() && ! bassTrack && ! p.isLayerPlaying (Layer::Bass), "bass on/off: All leaves a muted bass out");
    }

    // bassMode Off also mutes a custom bass
    setParam (p, id::bassOn, 1.0f);
    setParam (p, id::bassMode, 0.0f);
    pumpMessages (60);
    rig.all.clear();
    runBeats (rig, 16.5);
    check (countOns (rig.all, 0, kAnyTime, kBass) == 0 && ! p.isLayerOn (Layer::Bass) && p.isLayerCustom (Layer::Bass),
           "bass on/off: bassMode Off mutes a custom bass (custom kept)");

    // setLayerOn (Bass, true) with bassMode Off -> Root
    p.setLayerOn (Layer::Bass, true);
    pumpMessages (60);
    check (getParam (p, id::bassMode) == 1.0f && getParam (p, id::bassOn) > 0.5f && p.isLayerOn (Layer::Bass),
           "bass on/off: setLayerOn (Bass, true) with bassMode Off sets Root");
    p.setLayerOn (Layer::Bass, false);
    check (getParam (p, id::bassOn) < 0.5f && getParam (p, id::bassMode) == 1.0f, "bass on/off: setLayerOn (Bass, false) only clears bassOn");
    p.setLayerOn (Layer::Bass, true);
    pumpMessages (40);

    // Reset restores the pattern (Root here) and plays it again; undo / redo across custom and reset
    const int depth1 = undoDepth (p);
    p.resetLayer (Layer::Bass);
    pumpMessages (40);
    check (! p.isLayerCustom (Layer::Bass) && undoDepth (p) == depth1 + 1 && sameEvents (p.renderLayerEvents (Layer::Bass), autoBassEvents),
           "bass layer: resetLayer -> auto again (one undo step), the pattern is back");
    {
        const double next = std::ceil (rig.head.ppq / 16.0) * 16.0 + 16.0;
        runUntil (rig, next - 0.1);
        rig.all.clear();
        runUntil (rig, next + 16.5);
        std::string miss;
        check (loopMismatches (rig.all, autoBassEvents, spb, next, &miss) == 0, "bass layer: after reset the auto bass plays", miss);
    }
    check (p.undo() && p.isLayerCustom (Layer::Bass) && p.getLayerNotes (Layer::Bass) == custom,
           "bass layer: undo brings the custom bass back");
    check (p.redo() && ! p.isLayerCustom (Layer::Bass), "bass layer: redo resets it again");

    // Auto with bassMode Off: no content, Bass export refused
    setParam (p, id::bassMode, 0.0f);
    pumpMessages (60);
    check (p.getLayerNotes (Layer::Bass).empty() && p.renderLayerEvents (Layer::Bass).empty()
               && p.writeMidiToTempFile (Part::Bass) == juce::File(),
           "bass layer: auto with bassMode Off has no notes and the Bass export is refused");

    rig.head.playing = false;
    rig.run (2);
    check (rig.openCount() == 0 && rig.doubleOns == 0 && rig.strayOffs == 0, "bass layer: balanced, nothing left on");

    // A custom bass note at MIDI 28 plays at 28 (needs the NoteLimits overloads)
    ChordbloomProcessor q;
    q.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
    cb::Melody low;
    low.notes.push_back (melodyNote (28, 0.0, 1.0));
    low.notes.push_back (melodyNote (24, 4.0, 1.0));
    q.setLayerNotes (Layer::Bass, low);
    const auto lowEvents = q.renderLayerEvents (Layer::Bass);
    checkAfter (d.limits, "WP3 NoteLimits", lowEvents.size() == 2 && lowEvents[0].note == 28 && lowEvents[1].note == 24,
                "bass layer: custom bass notes at MIDI 28 and 24 play there (bass limits 24..72)");
    {
        Rig r2 (q);
        r2.head.playing = true;
        runUntil (r2, 5.5);
        checkAfter (d.limits, "WP3 NoteLimits", findEvent (r2.all, true, kBass, 28, 0) != nullptr,
                    "bass layer: MIDI 28 reaches the MIDI out");
        r2.head.playing = false;
        r2.run (2);
    }
}

//==============================================================================
// Custom arp on channel 4 (works without WP2) and midiOutPart 5.
void testArpChannelAndOutPart()
{
    ChordbloomProcessor p;
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
    const auto arpLine = customArpLine();
    p.setLayerNotes (Layer::Arp, arpLine);
    check (p.isLayerCustom (Layer::Arp) && p.getLayerNotes (Layer::Arp) == arpLine, "arp layer: custom notes stored");

    Rig rig (p);
    rig.head.playing = true;
    const double spb = rig.samplesPerBeat();
    runUntil (rig, 16.5);
    check (countOns (rig.all, 0, kAnyTime, kArp) == 0, "arp layer: arpOn off -> nothing on channel 4");

    setParam (p, id::arpOn, 1.0f);
    pumpMessages (60);
    runUntil (rig, 31.9);
    rig.all.clear();
    runUntil (rig, 48.5);
    std::string miss;
    check (loopMismatches (rig.all, p.renderLayerEvents (Layer::Arp), spb, 32.0, &miss) == 0
               && countOns (rig.all, (juce::int64) (32.0 * spb) - 2, (juce::int64) (48.0 * spb) - 2, kArp) == (int) arpLine.notes.size(),
           "arp layer: arpOn plays renderLayerEvents (Arp) on channel 4 sample-exactly",
           str (miss, " ons ", countOns (rig.all, (juce::int64) (32.0 * spb) - 2, (juce::int64) (48.0 * spb) - 2, kArp), " want ", arpLine.notes.size()));

    // midiOutPart 5: only channel 4, arp auditions included
    setParam (p, id::midiOutPart, 5.0f);
    pumpMessages (40);
    rig.all.clear();
    auto evs = runBeats (rig, 16.0);
    check (countOns (evs, 0, kAnyTime, kArp) > 0 && countOns (evs, 0, kAnyTime, kChords) == 0
               && countOns (evs, 0, kAnyTime, kBass) == 0 && countOns (evs, 0, kAnyTime, kMel) == 0,
           "midiOutPart Arp only: just channel 4 goes out");
    check (rig.openNotes (kChords).empty() && rig.openNotes (kBass).empty(), "midiOutPart -> Arp only: chord and bass notes closed");
    p.startPreviewLayerNote (Layer::Arp, 101);
    evs = rig.processBlock();
    check (findEvent (evs, true, kArp, 101, rig.samplePos - rig.blockSize, 0) != nullptr,
           "midiOutPart Arp only: an arp audition (channel 4) passes");
    p.startPreviewChord (F_);
    evs = rig.processBlock();
    check (countOns (evs, 0, kAnyTime, kChords) == 0, "midiOutPart Arp only: a chord preview does not");
    p.startPreviewLayerNote (Layer::Bass, 41);
    evs = rig.processBlock();
    check (countOns (evs, 0, kAnyTime, kBass) == 0 && p.getSoundingNotes().test (41), "midiOutPart Arp only: a bass audition stays internal");
    p.stopPreview();
    rig.processBlock();

    // Switching mid-note closes channel 4 in the MIDI out while the synth keeps it
    while (std::fmod (rig.head.ppq, 16.0) < 2.0 || std::fmod (rig.head.ppq, 16.0) > 6.0)
        rig.processBlock();
    check (rig.openNotes (kArp).count (95) == 1, "midiOutPart Arp only: the long arp note is sent");
    setParam (p, id::midiOutPart, 1.0f);
    rig.processBlock();
    check (rig.openNotes (kArp).empty() && p.getSoundingNotes().test (95),
           "midiOutPart Arp only -> Chords only: channel 4 closed in the MIDI out, the synth keeps the note");

    setParam (p, id::midiOutPart, 0.0f);
    rig.all.clear();
    evs = runBeats (rig, 16.0);
    check (countOns (evs, 0, kAnyTime, kArp) > 0 && countOns (evs, 0, kAnyTime, kChords) > 0 && countOns (evs, 0, kAnyTime, kBass) > 0,
           "midiOutPart All parts: includes channel 4");
    setParam (p, id::midiOutPart, 4.0f);
    rig.all.clear();
    evs = runBeats (rig, 16.0);
    check (countOns (evs, 0, kAnyTime, kArp) == 0 && countOns (evs, 0, kAnyTime, kChords) > 0 && countOns (evs, 0, kAnyTime, kBass) > 0,
           "midiOutPart Chords + Bass: excludes channel 4");
    check (rig.openNotes (kArp).empty(), "midiOutPart -> Chords + Bass: arp notes closed");

    setParam (p, id::midiOutPart, 0.0f);
    setParam (p, id::arpOn, 0.0f);
    pumpMessages (60);
    rig.all.clear();
    evs = runBeats (rig, 16.0);
    check (countOns (evs, 0, kAnyTime, kArp) == 0 && rig.openNotes (kArp).empty(), "arp layer: arpOn off again -> silent");

    rig.head.playing = false;
    rig.run (2);
    check (rig.openCount() == 0 && rig.doubleOns == 0 && rig.strayOffs == 0 && p.getSoundingNotes().none(),
           "arp layer / out part: balanced, nothing left on");
}

//==============================================================================
// RenderSnapshot: arpGate is timing-only (no re-strike); bassOn / arpOn / arpShape are structural (chase).
void testRenderClasses()
{
    ChordbloomProcessor p;
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
    p.setLayerNotes (Layer::Arp, customArpLine());
    setParam (p, id::arpOn, 1.0f);
    pumpMessages (60);

    Rig rig (p);
    rig.head.playing = true;
    runUntil (rig, 2.1);
    check (rig.openNotes (kArp).count (95) == 1, "render classes: the long custom arp note sounds");

    setParam (p, id::arpGate, 0.3f);
    pumpMessages (60);
    auto evs = rig.processBlock();
    check (countOns (evs, 0, kAnyTime, kArp) == 0 && rig.openNotes (kArp).count (95) == 1,
           "render classes: arpGate is timing-only (no re-strike, the note keeps sounding)");

    setParam (p, id::arpShape, 7.0f);
    pumpMessages (60);
    evs = rig.processBlock();
    check (countOns (evs, 0, kAnyTime, kArp) == 0 && rig.openNotes (kArp).count (95) == 1 && rig.doubleOns == 0,
           "render classes: arpShape on a custom arp re-renders without striking anything again");

    const auto bassBefore = rig.openNotes (kBass);
    setParam (p, id::bassOn, 0.0f);
    pumpMessages (60);
    rig.processBlock();
    check (! bassBefore.empty() && rig.openNotes (kBass).empty(), "render classes: bassOn off closes the sounding bass note");
    setParam (p, id::bassOn, 1.0f);
    pumpMessages (60);
    evs = rig.processBlock();
    check (countOns (evs, 0, kAnyTime, kBass) == 1 && rig.openNotes (kBass) == bassBefore,
           "render classes: bassOn is structural (the bass note is chased at once)");

    setParam (p, id::arpOn, 0.0f);
    pumpMessages (60);
    rig.processBlock();
    check (rig.openNotes (kArp).empty(), "render classes: arpOn off closes the arp");
    setParam (p, id::arpOn, 1.0f);
    pumpMessages (60);
    evs = rig.processBlock();
    check (findEvent (evs, true, kArp, 95, rig.samplePos - rig.blockSize, 0) != nullptr,
           "render classes: arpOn is structural (the long arp note is chased)");

    rig.head.playing = false;
    rig.run (2);
    check (rig.openCount() == 0 && rig.doubleOns == 0 && rig.strayOffs == 0, "render classes: balanced");
}

//==============================================================================
// The auto arp (WP2): playback, timing-only gate, mid-note parameter changes.
void testAutoArp (const Deps& d)
{
    const char* needs = "WP2 renderArp";
    ChordbloomProcessor p;
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
    setParam (p, id::arpOn, 1.0f);
    pumpMessages (60);
    const auto expected = p.renderLayerEvents (Layer::Arp);
    checkAfter (d.arp, needs, ! expected.empty() && ! p.isLayerCustom (Layer::Arp)
                                  && std::all_of (expected.begin(), expected.end(), [] (const cb::NoteEvent& e) { return e.channel == kArp; }),
                "auto arp: renders on channel 4");

    Rig rig (p);
    rig.head.playing = true;
    const double spb = rig.samplesPerBeat();
    runUntil (rig, 16.5);
    std::string miss;
    checkAfter (d.arp, needs, loopMismatches (rig.all, expected, spb, 0.0, &miss) == 0
                                  && countOns (rig.all, 0, (juce::int64) (16.0 * spb) - 2, kArp) == (int) expected.size(),
                "auto arp: arpOn plays renderLayerEvents (Arp) sample-exactly", miss);
    checkAfter (d.arp && d.layers, "WP2 + WP3", p.getLayerNotes (Layer::Arp) == cb::notesFromEvents (expected, cb::layerLimits (Layer::Arp)),
                "auto arp: getLayerNotes = the humanize-free render (humanize 0 here)");

    // arpGate is timing-only: lengthening the gate mid-step never strikes a ghost note before the next step
    setParam (p, id::arpGate, 0.2f);
    pumpMessages (60);
    runUntil (rig, 20.1);   // 1/16 steps: inside the step at 20.0 (its 0.05-beat note already ended)
    setParam (p, id::arpGate, 1.0f);
    pumpMessages (60);
    auto evs = runUntil (rig, 20.24);
    checkAfter (d.arp, needs, countOns (evs, 0, kAnyTime, kArp) == 0, "auto arp: an arpGate change mid-step strikes nothing",
                str (countOns (evs, 0, kAnyTime, kArp), " ons"));
    evs = runUntil (rig, 20.3);
    checkAfter (d.arp, needs, countOns (evs, 0, kAnyTime, kArp) >= 1, "auto arp: the next step plays normally");

    // Mid-note shape / rate / rhythm / octave changes leave no hanging note
    bool cleanSwitches = true;
    std::string why;
    const std::pair<const char*, float> changes[] = { { id::arpShape, 6.0f }, { id::arpRate, 1.0f }, { id::arpRhythm, 5.0f },
                                                      { id::arpOctave, -1.0f }, { id::arpShape, 12.0f }, { id::arpRate, 3.0f } };
    for (auto& [paramId, value] : changes)
    {
        runBeats (rig, 0.37);
        setParam (p, paramId, value);
        pumpMessages (40);
        rig.processBlock();
        const auto now = p.renderLayerEvents (Layer::Arp);
        const double pos = std::fmod (rig.head.ppq, 16.0);
        for (int n : rig.openNotes (kArp))
        {
            bool covered = false;
            for (auto& e : now)
                covered = covered || (e.note == n && e.startBeat <= pos + 0.02 && e.startBeat + e.lengthBeats > pos - 0.05);
            if (! covered)
            {
                cleanSwitches = false;
                why += str (paramId, " left ", n, "; ");
            }
        }
    }
    rig.head.playing = false;
    rig.run (2);
    checkAfter (d.arp, needs, cleanSwitches && rig.openCount() == 0 && rig.doubleOns == 0 && rig.strayOffs == 0,
                "auto arp: shape / rate / rhythm / octave changes mid-note leave no hanging note", why);
}

//==============================================================================
// Custom layers vs auto: equality keeps auto, humanize-free display, 1024-note arp.
void testCustomLayers (const Deps& d)
{
    ChordbloomProcessor p;
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
    setParam (p, id::humanize, 1.0f);
    setParam (p, id::bassMode, (float) cb::BassMode::Pulse);
    pumpMessages (60);

    // Notes equal to the auto notes: the layer stays auto, no undo step (humanize 1: the display is humanize-free)
    for (auto layer : { Layer::Bass, Layer::Arp })
    {
        const int depth = undoDepth (p);
        const auto shown = p.getLayerNotes (layer);
        p.setLayerNotes (layer, shown);
        checkAfter (layer == Layer::Arp ? d.layers && d.arp : d.layers, "WP3 notesFromEvents (+ WP2 for the arp)",
                    ! shown.empty() && ! p.isLayerCustom (layer) && undoDepth (p) == depth,
                    str ("custom layers: setLayerNotes (getLayerNotes()) keeps ", cb::layerName (layer), " auto at humanize 1, no undo step"));
    }
    {
        // The display is the humanize-free render: identical at humanize 0 and 1
        const auto at1 = p.getLayerNotes (Layer::Bass);
        setParam (p, id::humanize, 0.0f);
        pumpMessages (60);
        const auto at0 = p.getLayerNotes (Layer::Bass);
        checkAfter (d.layers, "WP3 notesFromEvents", ! at0.empty() && at0 == at1, "custom layers: auto display notes ignore humanize");
        checkAfter (d.layers, "WP3 notesFromEvents",
                    at0 == cb::notesFromEvents (p.renderLayerEvents (Layer::Bass), cb::layerLimits (Layer::Bass)),
                    "custom layers: auto display = notesFromEvents of the playback render at humanize 0");
    }

    // Editing an auto layer's display notes makes it custom; the auto notes again make it auto (one step each)
    {
        const auto shown = p.getLayerNotes (Layer::Bass);
        auto edited = shown;
        if (! edited.empty())
            edited.notes.erase (edited.notes.begin());
        const int depth = undoDepth (p);
        p.setLayerNotes (Layer::Bass, edited);
        const bool custom = p.isLayerCustom (Layer::Bass);
        p.setLayerNotes (Layer::Bass, shown);
        checkAfter (d.layers, "WP3 notesFromEvents", custom && ! p.isLayerCustom (Layer::Bass) && undoDepth (p) == depth + 2,
                    "custom layers: an edited display note makes the layer custom; the auto notes make it auto again");
    }

    // A 1024-note custom arp plays every note
    {
        ChordbloomProcessor q;
        cb::Progression long64;
        for (int i = 0; i < 16; ++i)
            long64.slots.push_back ({ i % 2 == 0 ? C_ : G_, 16, -1, 0 });
        q.setProgression (long64);
        cb::Melody many;
        for (int i = 0; i < 1024; ++i)
            many.notes.push_back (melodyNote (60 + i % 12, i * 0.25, 0.2));
        q.setLayerNotes (Layer::Arp, many);
        setParam (q, id::arpOn, 1.0f);
        pumpMessages (60);
        const auto events = q.renderLayerEvents (Layer::Arp);
        checkAfter (d.limits, "WP3 NoteLimits", q.getLayerNotes (Layer::Arp).notes.size() == 1024 && events.size() == 1024,
                    "custom layers: a 1024-note custom arp keeps every note", str (events.size()));
        Rig rig (q);
        rig.head.playing = true;
        runUntil (rig, 256.5);
        checkAfter (d.limits, "WP3 NoteLimits", countOns (rig.all, 0, (juce::int64) (256.0 * rig.samplesPerBeat()) - 2, kArp) == 1024,
                    "custom layers: ... and plays all 1024", str (countOns (rig.all, 0, kAnyTime, kArp)));
        rig.head.playing = false;
        rig.run (2);
        check (rig.openCount() == 0 && rig.doubleOns == 0, "custom layers: long arp balanced");
    }
}

//==============================================================================
// Only a commit that can change a layer compares it with auto; resetLayer reserves only -1 for the whole layer.
void testNormaliseScope (const Deps& d)
{
    {
        ChordbloomProcessor p;
        p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
        p.setLayerNotes (Layer::Bass, customBassLine());
        const int depth = undoDepth (p);
        p.resetLayer (Layer::Bass, -2);
        p.resetLayer (Layer::Bass, -7);
        check (p.isLayerCustom (Layer::Bass) && p.getLayerNotes (Layer::Bass) == customBassLine() && undoDepth (p) == depth,
               "resetLayer: a slot index below -1 is a no-op (notes, custom flag and undo depth unchanged)");
        p.resetLayer (Layer::Bass, -1);
        check (! p.isLayerCustom (Layer::Bass) && undoDepth (p) == depth + 1, "resetLayer: -1 resets the whole layer (one step)");
    }

    // A custom bass that equals auto after a parameter change (automation: custom wins) stays custom through a no-op
    // commit, a melody edit and a save / load; only an edit of the layer itself (or of the progression) turns it auto.
    {
        ChordbloomProcessor root;
        root.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
        setParam (root, id::bassMode, (float) cb::BassMode::Root);
        pumpMessages (40);
        const auto rootNotes = root.getLayerNotes (Layer::Bass);

        ChordbloomProcessor p;
        p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
        setParam (p, id::bassMode, (float) cb::BassMode::Pulse);
        pumpMessages (40);
        p.setLayerNotes (Layer::Bass, rootNotes);
        const bool customAtPulse = p.isLayerCustom (Layer::Bass);
        setParam (p, id::bassMode, (float) cb::BassMode::Root);   // automation: the custom notes now equal auto
        pumpMessages (40);
        const bool equalsAuto = p.getLayerNotes (Layer::Bass) == root.getLayerNotes (Layer::Bass);

        const int depth = undoDepth (p);
        p.setMelody (p.getMelody());
        const bool afterNoOp = p.isLayerCustom (Layer::Bass) && undoDepth (p) == depth;
        p.setMelody (makeMelody ({ melodyNote (72, 1.0, 1.0) }));
        const bool afterMelody = p.isLayerCustom (Layer::Bass) && undoDepth (p) == depth + 1;

        ChordbloomProcessor loaded;
        loaded.setProgression (makeProgression ({ G_ }));
        if (auto xml = stateXml (p))
            loadXml (loaded, *xml);
        pumpMessages (40);
        const bool loadedCustom = loaded.isLayerCustom (Layer::Bass) && ! loaded.canUndo();
        loaded.setMelody (loaded.getMelody());
        const bool loadedNoOp = loaded.isLayerCustom (Layer::Bass) && ! loaded.canUndo();
        loaded.setMelody ({});
        const bool loadedMelody = loaded.isLayerCustom (Layer::Bass);

        p.setLayerNotes (Layer::Bass, p.getLayerNotes (Layer::Bass));   // an edit of the layer: equal to auto -> auto
        const bool editTurnsAuto = ! p.isLayerCustom (Layer::Bass) && undoDepth (p) == depth + 2;

        checkAfter (d.layers, "WP3 notesFromEvents",
                    customAtPulse && ! rootNotes.empty() && equalsAuto && afterNoOp && afterMelody && loadedCustom && loadedNoOp
                        && loadedMelody && editTurnsAuto,
                    "normalise: a custom layer equal to auto is only turned auto by an edit of that layer, never by a melody or no-op commit",
                    str (customAtPulse, equalsAuto, afterNoOp, afterMelody, loadedCustom, loadedNoOp, loadedMelody, editTurnsAuto));
    }

    // 2.3: a pick of Off keeps the custom notes; a silenced (custom-empty) bass stays silent through a melody edit and
    // the LED switching it back on (bassMode Root, still no notes).
    {
        ChordbloomProcessor p;
        p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
        pumpMessages (40);
        p.setLayerNotes (Layer::Bass, {});
        const bool silenced = p.isLayerCustom (Layer::Bass);
        setParamAsUser (p, id::bassMode, (float) cb::BassMode::Off);
        pumpMessages (40);
        const bool afterOff = p.isLayerCustom (Layer::Bass);
        p.setMelody (makeMelody ({ melodyNote (72, 1.0, 1.0) }));
        const bool afterMelody = p.isLayerCustom (Layer::Bass);
        p.setLayerOn (Layer::Bass, true);
        pumpMessages (40);
        checkAfter (d.layers, "WP3 notesFromEvents",
                    silenced && afterOff && afterMelody && p.isLayerCustom (Layer::Bass)
                        && juce::roundToInt (getParam (p, id::bassMode)) == (int) cb::BassMode::Root
                        && p.renderLayerEvents (Layer::Bass).empty(),
                    "normalise: a silenced custom bass set to Off survives a melody edit; the LED turns it on silent",
                    str (silenced, afterOff, afterMelody, " events ", p.renderLayerEvents (Layer::Bass).size()));
    }
}

//==============================================================================
// Undo / redo across layers (every push uses current(): nothing drops a custom layer).
void testUndoAcrossLayers()
{
    ChordbloomProcessor p;
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
    const auto m = makeMelody ({ melodyNote (72, 0.0, 1.0), melodyNote (76, 4.0, 2.0) });
    const auto arpLine = customArpLine();
    const auto bassLine = customBassLine();

    p.setMelody (m);                              // 1
    p.setLayerNotes (Layer::Bass, bassLine);      // 2
    p.setLayerNotes (Layer::Arp, arpLine);        // 3
    p.resetLayer (Layer::Bass);                   // 4
    p.addChord (Em_);                             // 5 (append: melody stays, custom arp fills the new slot)
    check (p.getProgression().slots.size() == 5 && p.isLayerCustom (Layer::Arp) && ! p.isLayerCustom (Layer::Bass)
               && p.getMelody() == m,
           "undo across layers: the edit sequence");

    struct Expect { size_t slots; bool arpCustom, bassCustom, melody; };
    const Expect states[] = { { 5, true, false, true }, { 4, true, false, true }, { 4, true, true, true },
                              { 4, false, true, true }, { 4, false, false, true }, { 4, false, false, false } };
    bool undoOk = true, redoOk = true;
    std::string why;
    auto matches = [&] (const Expect& e)
    {
        return p.getProgression().slots.size() == e.slots && p.isLayerCustom (Layer::Arp) == e.arpCustom
            && p.isLayerCustom (Layer::Bass) == e.bassCustom && (p.getMelody() == m) == e.melody
            && (! e.bassCustom || p.getLayerNotes (Layer::Bass) == bassLine)
            && (! e.arpCustom || notesStartingIn (p.getLayerNotes (Layer::Arp), 0.0, 16.0) == arpLine);
    };
    for (int i = 1; i < 6; ++i)
    {
        p.undo();
        if (! matches (states[i]))
        {
            undoOk = false;
            why += str ("undo ", i, "; ");
        }
    }
    for (int i = 4; i >= 0; --i)
    {
        p.redo();
        if (! matches (states[i]))
        {
            redoOk = false;
            why += str ("redo to ", i, "; ");
        }
    }
    check (undoOk, "undo across layers: every undo restores progression, melody, arp and bass together", why);
    check (redoOk, "undo across layers: every redo too", why);

    // A redo after an undo of a custom-layer edit
    p.setLayerNotes (Layer::Bass, bassLine);
    p.undo();
    check (! p.isLayerCustom (Layer::Bass), "undo across layers: undo of a custom bass edit -> auto");
    check (p.redo() && p.isLayerCustom (Layer::Bass) && p.getLayerNotes (Layer::Bass) == bassLine && p.isLayerCustom (Layer::Arp),
           "undo across layers: redo brings it back with the custom arp untouched");

    // Melody API = Melody layer
    p.setLayerNotes (Layer::Melody, makeMelody ({ melodyNote (79, 1.0, 1.0) }));
    check (p.getMelody().notes.size() == 1 && p.getLayerNotes (Layer::Melody) == p.getMelody() && ! p.isLayerCustom (Layer::Melody),
           "layers: setLayerNotes (Melody) = setMelody, never custom");
    const int depth = undoDepth (p);
    p.resetLayer (Layer::Melody);
    p.resetLayer (Layer::Arp, 99);
    check (undoDepth (p) == depth, "layers: resetLayer on the melody or an invalid slot is a no-op");
}

//==============================================================================
// User picks (D13): reset + layer on, automation never resets, undo brings custom back.
void testUserPick (const Deps& d)
{
    ChordbloomProcessor p;
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
    const auto bassLine = customBassLine();
    p.setLayerNotes (Layer::Bass, bassLine);
    setParam (p, id::bassOn, 0.0f);
    pumpMessages (40);

    // Host automation of bassMode while custom resets nothing
    setParam (p, id::bassMode, (float) cb::BassMode::Octaves);
    pumpMessages (60);
    check (p.isLayerCustom (Layer::Bass) && p.getLayerNotes (Layer::Bass) == bassLine, "user pick: bassMode automation resets nothing");

    const int depth = undoDepth (p);
    setParamAsUser (p, id::bassMode, (float) cb::BassMode::Pulse);
    p.beginUserLayerPick (Layer::Bass);
    pumpMessages (40);
    check (! p.isLayerCustom (Layer::Bass) && undoDepth (p) == depth + 1 && getParam (p, id::bassOn) > 0.5f && p.isLayerOn (Layer::Bass),
           "user pick: beginUserLayerPick on a custom bass = one undo step + bass on");
    p.undo();
    pumpMessages (40);
    check (p.isLayerCustom (Layer::Bass) && sameEvents (p.renderLayerEvents (Layer::Bass),
                                                        cb::renderMelody (bassLine, 16.0, kBass, cb::layerLimits (Layer::Bass))),
           "user pick: undo brings the custom notes back and they win over the picked bassMode");

    // Arp: automation of arpShape while custom resets nothing; a pick resets and switches the arp on
    const auto arpLine = customArpLine();
    p.setLayerNotes (Layer::Arp, arpLine);
    setParam (p, id::arpShape, 3.0f);
    pumpMessages (60);
    check (p.isLayerCustom (Layer::Arp) && p.getLayerNotes (Layer::Arp) == arpLine, "user pick: arpShape automation resets nothing");
    setParamAsUser (p, id::arpShape, 5.0f);
    p.beginUserLayerPick (Layer::Arp);
    pumpMessages (40);
    check (! p.isLayerCustom (Layer::Arp) && getParam (p, id::arpOn) > 0.5f, "user pick: beginUserLayerPick (Arp) resets and turns the arp on");
    const int depth2 = undoDepth (p);
    p.beginUserLayerPick (Layer::Arp);
    check (undoDepth (p) == depth2, "user pick: on an auto layer it only switches the layer on (no undo step)");

    // With a custom arp that is on and playing, a pick puts no note of the old (custom) line into the MIDI out
    ChordbloomProcessor q;
    q.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
    cb::Melody oldLine;
    for (int i = 0; i < 64; ++i)
        oldLine.notes.push_back (melodyNote (100, i * 0.25, 0.2));
    q.setLayerNotes (Layer::Arp, oldLine);
    setParam (q, id::arpOn, 1.0f);
    pumpMessages (60);
    Rig rig (q);
    rig.head.playing = true;
    runUntil (rig, 5.1);
    const bool oldPlayed = countOns (rig.all, 0, kAnyTime, kArp) > 0;
    setParamAsUser (q, id::arpShape, (float) cb::ArpShape::UpDown);
    q.beginUserLayerPick (Layer::Arp);
    const auto pickSample = rig.samplePos;
    runUntil (rig, 9.0);
    pumpMessages (40);
    runUntil (rig, 12.0);
    const auto now = q.renderLayerEvents (Layer::Arp);
    const double spb = rig.samplesPerBeat();
    int oldOns = 0, strays = 0;
    for (auto& e : rig.all)
    {
        if (! e.on || e.channel != kArp || e.sample < pickSample)
            continue;
        if (e.note == 100)
            ++oldOns;
        const double beat = std::fmod ((double) e.sample / spb, 16.0);
        bool inNew = false;
        for (auto& n : now)
            inNew = inNew || (n.note == e.note && n.startBeat <= beat + 1e-3 && n.startBeat + n.lengthBeats > beat);
        strays += inNew ? 0 : 1;
    }
    check (oldPlayed && oldOns == 0 && ! q.isLayerCustom (Layer::Arp), "user pick: no note of the old custom arp after the pick");
    checkAfter (d.arp, "WP2 renderArp", strays == 0 && countOns (rig.all, pickSample, kAnyTime, kArp) > 0,
                "user pick: every channel-4 note after the pick belongs to the new pattern", str (strays, " strays"));
    rig.head.playing = false;
    rig.run (2);
    check (rig.openCount() == 0 && rig.doubleOns == 0, "user pick: balanced across the pick");
}

//==============================================================================
// Follow policy (6.2) through the processor (WP3 core).
void testFollow (const Deps& d)
{
    const bool dep = d.follow && d.layers && d.edit;
    const char* needs = "WP3 layers / follow";
    const auto prog = makeProgression ({ C_, Am_, F_, G_ });

    ChordbloomProcessor p;
    p.setProgression (prog);
    pumpMessages (40);
    auto edited = p.getLayerNotes (Layer::Bass);
    edited.notes.push_back (melodyNote (57, 6.0, 0.5));   // an extra A in slot 1 only
    edited = cb::sanitiseMelody (edited, cb::layerLimits (Layer::Bass));
    p.setLayerNotes (Layer::Bass, edited);
    checkAfter (dep, needs, p.isLayerCustom (Layer::Bass) && p.getEditedSlots (Layer::Bass) == std::vector<bool> { false, true, false, false },
                "follow: one edited slot");

    auto hasNote = [] (const cb::Melody& m, int pitch, double start)
    {
        return std::any_of (m.notes.begin(), m.notes.end(), [&] (const cb::MelodyNote& n)
                            { return n.note == pitch && std::abs (n.startBeat - start) < 1e-9; });
    };
    auto stepOnce = [&] (const std::function<void()>& edit)
    {
        const int before = undoDepth (p);
        edit();
        return undoDepth (p) == before + 1;
    };

    // Structural edits move the custom notes, each one undo step
    bool one = stepOnce ([&] { p.removeSlot (0); });
    checkAfter (dep, needs, one && hasNote (p.getLayerNotes (Layer::Bass), 57, 2.0)
                              && p.getEditedSlots (Layer::Bass) == std::vector<bool> { true, false, false },
                "follow: removeSlot moves the edited notes with their slot (one undo step)");
    p.undo();
    one = stepOnce ([&] { p.moveSlot (1, 3); });
    checkAfter (dep, needs, one && hasNote (p.getLayerNotes (Layer::Bass), 57, 14.0)
                              && p.getEditedSlots (Layer::Bass) == std::vector<bool> { false, false, false, true },
                "follow: moveSlot moves them");
    p.undo();
    one = stepOnce ([&] { p.duplicateSlot (1); });
    checkAfter (dep, needs, one && hasNote (p.getLayerNotes (Layer::Bass), 57, 6.0) && hasNote (p.getLayerNotes (Layer::Bass), 57, 10.0),
                "follow: duplicateSlot copies them");
    p.undo();
    one = stepOnce ([&] { p.addChord (Em_, 1); });
    checkAfter (dep, needs, one && hasNote (p.getLayerNotes (Layer::Bass), 57, 10.0), "follow: addChord insert moves later notes");
    p.undo();

    // In-place chord change of the edited slot remaps it by role; of an untouched slot gives it the new auto notes
    {
        auto slot = p.getProgression().slots[1];
        slot.chord = { 2, cb::ChordQuality::Minor };   // Am -> Dm
        one = stepOnce ([&] { p.updateSlot (1, slot); });
        bool remapped = false;
        for (auto& n : p.getLayerNotes (Layer::Bass).notes)
            remapped = remapped || (std::abs (n.startBeat - 6.0) < 1e-9 && n.note % 12 == 2);
        checkAfter (dep, needs, one && remapped && p.getEditedSlots (Layer::Bass)[1], "follow: updateSlot chord change remaps the edited slot");
        p.undo();

        auto untouched = p.getProgression().slots[2];
        untouched.chord = { 2, cb::ChordQuality::Minor };   // F -> Dm
        p.updateSlot (2, untouched);
        ChordbloomProcessor q;
        q.setProgression (p.getProgression());
        pumpMessages (20);
        checkAfter (dep, needs, notesStartingIn (p.getLayerNotes (Layer::Bass), 8.0, 12.0) == notesStartingIn (q.getLayerNotes (Layer::Bass), 8.0, 12.0)
                                  && ! p.getEditedSlots (Layer::Bass)[2] && p.getEditedSlots (Layer::Bass)[1],
                    "follow: an untouched slot's chord change gives it the new auto notes (not edited)");
        p.undo();
    }

    // setProgression: same structure remaps, other structure resets (one step each)
    {
        one = stepOnce ([&] { p.setProgression (makeProgression ({ G_, Em_, C_, { 2, cb::ChordQuality::Major } })); });
        checkAfter (dep, needs, one && p.isLayerCustom (Layer::Bass) && p.getEditedSlots (Layer::Bass)[1],
                    "follow: setProgression with the same structure keeps (remaps) the custom bass");
        p.undo();
        one = stepOnce ([&] { p.setProgression (makeProgression ({ G_, Em_, C_ })); });
        check (one && ! p.isLayerCustom (Layer::Bass), "follow: setProgression with another structure resets the custom bass (one step)");
        p.undo();
        check (p.isLayerCustom (Layer::Bass), "follow: undo brings the custom bass back");
    }

    // addChord append fills the new slot from auto (rendered with the new progression's voicings)
    {
        one = stepOnce ([&] { p.addChord (Em_); });
        ChordbloomProcessor q;
        q.setProgression (p.getProgression());
        pumpMessages (20);
        checkAfter (dep, needs, one && ! notesStartingIn (q.getLayerNotes (Layer::Bass), 16.0, 20.0).empty()
                                  && notesStartingIn (p.getLayerNotes (Layer::Bass), 16.0, 20.0) == notesStartingIn (q.getLayerNotes (Layer::Bass), 16.0, 20.0),
                    "follow: addChord append fills the new slot with the auto notes");
        p.undo();
    }

    // transposeProgression transposes custom layers inside their limits
    {
        const auto before = p.getLayerNotes (Layer::Bass);
        one = stepOnce ([&] { p.transposeProgression (2); });
        checkAfter (dep, needs, one && p.isLayerCustom (Layer::Bass)
                                  && p.getLayerNotes (Layer::Bass) == cb::transposeMelody (before, 2, cb::layerLimits (Layer::Bass)),
                    "follow: transposeProgression transposes the custom bass (one step)");
        p.undo();
    }

    // Lengthening an edited slot fills its new tail with the auto notes (bass and arp)
    {
        setParam (p, id::arpOn, 1.0f);
        pumpMessages (40);
        auto arpEdited = p.getLayerNotes (Layer::Arp);
        arpEdited.notes.push_back (melodyNote (99, 5.0, 0.25));
        p.setLayerNotes (Layer::Arp, arpEdited);
        auto slot = p.getProgression().slots[1];
        slot.lengthBeats = 8;
        p.updateSlot (1, slot);
        ChordbloomProcessor q;
        q.setProgression (p.getProgression());
        setParam (q, id::arpOn, 1.0f);
        pumpMessages (40);
        checkAfter (dep && d.arp, "WP2 + WP3",
                    notesStartingIn (p.getLayerNotes (Layer::Bass), 8.0, 12.0) == notesStartingIn (q.getLayerNotes (Layer::Bass), 8.0, 12.0)
                        && notesStartingIn (p.getLayerNotes (Layer::Arp), 8.0, 12.0) == notesStartingIn (q.getLayerNotes (Layer::Arp), 8.0, 12.0)
                        && ! notesStartingIn (q.getLayerNotes (Layer::Arp), 8.0, 12.0).empty()
                        && hasNote (p.getLayerNotes (Layer::Bass), 57, 6.0) && hasNote (p.getLayerNotes (Layer::Arp), 99, 5.0),
                    "follow: lengthening an edited slot from 4 to 8 beats fills beats 4..8 with the auto notes (bass and arp)");
        p.undo();
        p.undo();
    }

    // Removing the only edited slot makes the layer auto again; a bassMode change then applies
    {
        p.removeSlot (1);
        checkAfter (dep, needs, ! p.isLayerCustom (Layer::Bass) && p.getEditedSlots (Layer::Bass) == std::vector<bool> (3, false),
                    "follow: removing the only edited slot turns the bass back to auto");
        const auto beforeMode = p.getLayerNotes (Layer::Bass);
        setParam (p, id::bassMode, (float) cb::BassMode::Pulse);
        pumpMessages (60);
        checkAfter (dep, needs, p.getLayerNotes (Layer::Bass) != beforeMode, "follow: ... and a bassMode change applies again");
    }
}

//==============================================================================
// State: round trips, malformed layers, off-thread consistency, determinism.
juce::String progressionKey (const cb::Progression& p)
{
    juce::String k;
    for (auto& s : p.slots)
        k << s.chord.root << "," << (int) s.chord.quality << "," << s.lengthBeats << "," << s.inversion << "," << s.octaveShift << ";";
    return k;
}

juce::String progressionKey (const juce::XmlElement* prog)
{
    juce::String k;
    if (prog != nullptr)
        for (auto* s : prog->getChildWithTagNameIterator ("Slot"))
            k << s->getIntAttribute ("root") << "," << s->getIntAttribute ("quality") << "," << s->getIntAttribute ("length") << ","
              << s->getIntAttribute ("inversion") << "," << s->getIntAttribute ("octave") << ";";
    return k;
}

juce::String notesKey (bool custom, const cb::Melody& m)
{
    if (! custom)
        return "auto";
    juce::String k ("custom:");
    for (auto& n : m.notes)
        k << n.note << "," << (int) std::lround (n.startBeat * 48.0) << "," << (int) std::lround (n.lengthBeats * 48.0) << ";";
    return k;
}

juce::String notesKey (const juce::XmlElement& state, const char* layerId)
{
    for (auto* e : state.getChildWithTagNameIterator ("Layer"))
    {
        if (e->getStringAttribute ("id") != layerId)
            continue;
        juce::String k ("custom:");
        for (auto* n : e->getChildWithTagNameIterator ("Note"))
            k << n->getIntAttribute ("note") << "," << n->getIntAttribute ("start") << "," << n->getIntAttribute ("length") << ";";
        return k;
    }
    return "auto";
}

void testLayerState (const Deps& d)
{
    // Round trip: custom arp, custom-empty bass (an empty <Layer>), the new parameters
    ChordbloomProcessor a;
    a.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
    a.setMelody (makeMelody ({ melodyNote (72, 0.0, 1.0) }));
    const auto arpLine = customArpLine();
    a.setLayerNotes (Layer::Arp, arpLine);
    setParam (a, id::arpOn, 1.0f);
    setParam (a, id::arpShape, 5.0f);
    setParam (a, id::arpRate, 1.0f);
    setParam (a, id::arpRhythm, 3.0f);
    setParam (a, id::arpOctave, -1.0f);
    setParam (a, id::arpGate, 0.45f);
    setParam (a, id::bassOn, 0.0f);
    setParam (a, id::bassMode, 9.0f);
    pumpMessages (40);
    auto xml = stateXml (a);
    if (xml == nullptr)
    {
        check (false, "layer state: getStateInformation parses");
        return;
    }
    auto* emptyBass = xml->createNewChildElement ("Layer");
    emptyBass->setAttribute ("id", "bass");
    emptyBass->setAttribute ("ticksPerBeat", 48);

    ChordbloomProcessor b;
    b.addChord (G_);
    loadXml (b, *xml);
    check (b.isLayerCustom (Layer::Arp) && b.getLayerNotes (Layer::Arp) == arpLine && b.isLayerCustom (Layer::Bass)
               && b.getLayerNotes (Layer::Bass).empty() && ! b.canUndo() && ! b.canRedo(),
           "layer state: custom arp and custom-empty bass load; undo cleared");
    bool paramsOk = true;
    for (auto* pid : { id::arpOn, id::arpShape, id::arpRate, id::arpRhythm, id::arpOctave, id::arpGate, id::bassOn, id::bassMode })
        paramsOk = paramsOk && std::abs (getParam (a, pid) - getParam (b, pid)) < 1e-4f;
    check (paramsOk, "layer state: the new parameters round-trip");
    const auto again = stateXml (b);
    const auto layers = again != nullptr ? layerElements (*again) : std::vector<const juce::XmlElement*>();
    check (layers.size() == 2 && layers[0]->getStringAttribute ("id") == "arp" && layers[1]->getStringAttribute ("id") == "bass"
               && layers[1]->getNumChildElements() == 0 && layers[0]->getNumChildElements() == (int) arpLine.notes.size()
               && layers[0]->getIntAttribute ("ticksPerBeat") == 48 && ! layers[1]->hasAttribute ("custom"),
           "layer state: re-saved with <Layer id=arp> (notes) then an empty <Layer id=bass>");
    check (b.renderLayerEvents (Layer::Bass).empty() && ! b.isLayerPlaying (Layer::Bass), "layer state: a custom-empty bass is silent");

    // Malformed layers: NaN, huge ticks, 100k notes, unknown and duplicate ids, a melody id
    {
        juce::XmlElement bad (*xml);
        if (auto* first = bad.getChildByAttribute ("id", "arp"))
            bad.removeChildElement (first, true);   // so the uppercase one below is the first arp element
        {
            // 100k notes, built as text and parsed (appending XmlElement children one by one is quadratic)
            juce::MemoryOutputStream text;
            text << "<Layer id=\"ARP\" ticksPerBeat=\"1e30\">";
            for (int i = 0; i < 100000; ++i)
                text << "<Note note=\"" << (i % 3 == 0 ? juce::String ("nan") : juce::String (20 + i % 100))
                     << "\" start=\"" << (i % 5 == 0 ? juce::String ("nan") : juce::String (i * 9600))
                     << "\" length=\"" << (i % 7 == 0 ? juce::String ("-5") : juce::String (4800))
                     << "\" velocity=\"" << (i % 2 == 0 ? "inf" : "0.5") << "\"/>";
            text << "</Layer>";
            if (auto huge = juce::parseXML (text.toString()))
                bad.addChildElement (huge.release());
        }
        auto* dup = bad.createNewChildElement ("Layer");
        dup->setAttribute ("id", "bass");
        dup->createNewChildElement ("Note")->setAttribute ("note", 50);
        auto* mel = bad.createNewChildElement ("Layer");
        mel->setAttribute ("id", "melody");
        mel->createNewChildElement ("Note")->setAttribute ("note", 60);
        bad.createNewChildElement ("Layer")->setAttribute ("id", "drums");
        bad.createNewChildElement ("Layer");
        auto* nanTicks = bad.createNewChildElement ("Layer");
        nanTicks->setAttribute ("id", "Bass");
        nanTicks->setAttribute ("ticksPerBeat", "nan");

        ChordbloomProcessor c;
        bool threw = false;
        try
        {
            loadXml (c, bad);
            pumpMessages (10);
        }
        catch (...)
        {
            threw = true;
        }
        const auto arpNotes = c.getLayerNotes (Layer::Arp);
        bool sane = arpNotes == cb::sanitiseMelody (arpNotes, cb::layerLimits (Layer::Arp)) && (int) arpNotes.notes.size() <= cb::kMaxLayerNotes;
        for (auto& n : arpNotes.notes)
            sane = sane && n.note >= 28 && n.note <= 108 && std::isfinite (n.startBeat) && std::isfinite (n.velocity);
        check (! threw && c.isLayerCustom (Layer::Arp) && sane && ! arpNotes.empty(),
               "layer state: a hostile <Layer> (NaN, ticks 1e30, 100k notes) is capped and sanitised",
               str (arpNotes.notes.size(), " notes"));
        check (c.isLayerCustom (Layer::Bass) && c.getLayerNotes (Layer::Bass).empty() && c.getMelody().notes.size() == 1,
               "layer state: duplicate / unknown / melody ids are ignored (the first bass element wins)");
    }

    // A well-formed 1500-note arp layer loads whole (the layer cap is 2048, not the melody's 512)
    {
        juce::XmlElement big (*xml);
        if (auto* first = big.getChildByAttribute ("id", "arp"))
            big.removeChildElement (first, true);
        juce::MemoryOutputStream text;
        text << "<Layer id=\"arp\" ticksPerBeat=\"48\">";
        for (int i = 0; i < 1500; ++i)
            text << "<Note note=\"" << (40 + i % 50) << "\" start=\"" << (i * 6) << "\" length=\"5\" velocity=\"0.7\"/>";
        text << "</Layer>";
        if (auto layer = juce::parseXML (text.toString()))
            big.addChildElement (layer.release());
        ChordbloomProcessor c;
        loadXml (c, big);
        checkAfter (d.limits, "WP3 NoteLimits", c.isLayerCustom (Layer::Arp) && c.getLayerNotes (Layer::Arp).notes.size() == 1500,
                    "layer state: a 1500-note arp layer loads whole (cap 2048)", str (c.getLayerNotes (Layer::Arp).notes.size()));
        const auto resaved = stateXml (c);
        const auto els = resaved != nullptr ? layerElements (*resaved) : std::vector<const juce::XmlElement*>();
        checkAfter (d.limits, "WP3 NoteLimits", ! els.empty() && els[0]->getNumChildElements() == 1500,
                    "layer state: ... and saves all 1500 again");
    }

    // Undo is cleared on load (also with layers) and both layers are replaced
    {
        ChordbloomProcessor e;
        e.setProgression (makeProgression ({ C_, G_ }));
        e.setLayerNotes (Layer::Bass, customBassLine());
        e.undo();
        juce::MemoryBlock st;
        a.getStateInformation (st);
        e.setStateInformation (st.getData(), (int) st.getSize());
        check (! e.canUndo() && ! e.canRedo() && e.isLayerCustom (Layer::Arp) && ! e.isLayerCustom (Layer::Bass),
               "layer state: load clears undo and redo and replaces both layers");
    }
}

// getStateInformation from a worker thread while the message thread commits: every saved state pairs a progression
// with the bass notes committed with it.
void testStateConsistencyOffThread()
{
    ChordbloomProcessor p;
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
    p.setLayerNotes (Layer::Bass, customBassLine());

    std::map<juce::String, std::set<juce::String>> committed;
    auto record = [&] { committed[progressionKey (p.getProgression())].insert (notesKey (p.isLayerCustom (Layer::Bass), p.getLayerNotes (Layer::Bass))); };
    record();

    std::atomic<bool> stop { false };
    std::vector<juce::MemoryBlock> saved;
    std::thread saver ([&]
    {
        while (! stop.load() && saved.size() < 4000)
        {
            juce::MemoryBlock b;
            p.getStateInformation (b);
            saved.push_back (std::move (b));
        }
    });

    // Each structural edit is followed by an edit of the bass (one extra note in the first slot, a new pitch each
    // time), so the bass stays custom for the whole run and every saved state has to pair its own notes.
    const cb::Chord chords[] = { C_, Am_, F_, G_, Em_ };
    int customCommits = 0;
    for (int i = 0; i < 300; ++i)
    {
        switch (i % 3)
        {
            case 0:  p.transposeProgression (i % 2 == 0 ? 1 : -2); break;
            case 1:  p.removeSlot (0); break;
            default: p.addChord (chords[i % 5]); break;
        }
        record();
        auto bass = p.getLayerNotes (Layer::Bass);
        bass.notes.push_back (melodyNote (30 + i % 40, 0.5, 0.25));
        p.setLayerNotes (Layer::Bass, bass);
        record();
        customCommits += p.isLayerCustom (Layer::Bass) ? 1 : 0;
    }
    stop.store (true);
    saver.join();

    int bad = 0, parsed = 0, withBass = 0;
    for (auto& b : saved)
    {
        const auto xml = juce::AudioProcessor::getXmlFromBinary (b.getData(), (int) b.getSize());
        if (xml == nullptr)
        {
            ++bad;
            continue;
        }
        ++parsed;
        const auto key = progressionKey (xml->getChildByName ("Progression"));
        const auto bassKey = notesKey (*xml, "bass");
        withBass += bassKey != "auto" ? 1 : 0;
        const auto it = committed.find (key);
        if (it == committed.end() || it->second.count (bassKey) == 0)
            ++bad;
    }
    check (customCommits == 300 && parsed > 100 && withBass > 100 && bad == 0,
           "state off-thread: every state saved during commits on a custom bass pairs a progression with its own bass notes",
           str (bad, " inconsistent of ", saved.size(), ", ", withBass, " with a custom bass, custom after ", customCommits, " of 300"));

    // setStateInformation off the message thread, then getStateInformation from that thread before the async update
    ChordbloomProcessor src;
    src.setProgression (makeProgression ({ F_, G_, Em_, Am_ }));
    src.setLayerNotes (Layer::Arp, customArpLine());
    src.setLayerNotes (Layer::Bass, customBassLine());
    juce::MemoryBlock state;
    src.getStateInformation (state);

    ChordbloomProcessor c;
    c.setProgression (makeProgression ({ C_ }));
    juce::MemoryBlock echoed;
    std::thread loader ([&]
    {
        c.setStateInformation (state.getData(), (int) state.getSize());
        c.getStateInformation (echoed);
    });
    loader.join();
    const auto echo = juce::AudioProcessor::getXmlFromBinary (echoed.getData(), (int) echoed.getSize());
    const auto orig = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
    check (echo != nullptr && orig != nullptr
               && progressionKey (echo->getChildByName ("Progression")) == progressionKey (src.getProgression())
               && notesKey (*echo, "arp") == notesKey (*orig, "arp") && notesKey (*echo, "bass") == notesKey (*orig, "bass")
               && notesKey (*echo, "arp") != "auto",
           "state off-thread: a load on a worker thread echoes the new progression with the new layers before the async apply");
    pumpMessages (60);
    check (c.getProgression() == src.getProgression() && c.isLayerCustom (Layer::Arp) && c.isLayerCustom (Layer::Bass)
               && c.getLayerNotes (Layer::Arp) == src.getLayerNotes (Layer::Arp) && ! c.canUndo(),
           "state off-thread: applied on the message thread afterwards");

    // An edit, undo or redo on the message thread before the async apply works on the loaded song: the load is applied
    // first, so a save in that window holds the new song plus the edit and the edit is not lost afterwards.
    ChordbloomProcessor e;
    e.setProgression (makeProgression ({ C_ }));
    std::thread worker ([&] { e.setStateInformation (state.getData(), (int) state.getSize()); });
    worker.join();
    e.addChord (G_);
    const auto afterEdit = stateXml (e);
    auto expected = src.getProgression();
    cb::ProgressionSlot appended;
    appended.chord = G_;
    expected.slots.push_back (appended);
    const bool savedOk = afterEdit != nullptr && progressionKey (afterEdit->getChildByName ("Progression")) == progressionKey (expected)
                         && notesKey (*afterEdit, "arp") == notesKey (e.isLayerCustom (Layer::Arp), e.getLayerNotes (Layer::Arp))
                         && notesKey (*afterEdit, "arp") != "auto";
    pumpMessages (60);
    const bool kept = e.getProgression() == expected && e.isLayerCustom (Layer::Arp) && e.canUndo();
    e.undo();
    const bool undone = e.getProgression() == src.getProgression() && ! e.canUndo();
    check (savedOk && kept && undone,
           "state off-thread: an edit before the async apply lands on the loaded song (saved with it, kept, one undo step)",
           str (savedOk, kept, undone));

    ChordbloomProcessor u;
    u.setProgression (makeProgression ({ C_ }));
    u.addChord (G_);
    std::thread worker2 ([&] { u.setStateInformation (state.getData(), (int) state.getSize()); });
    worker2.join();
    const bool undid = u.undo();
    const auto afterUndo = stateXml (u);
    check (! undid && u.getProgression() == src.getProgression() && afterUndo != nullptr
               && progressionKey (afterUndo->getChildByName ("Progression")) == progressionKey (src.getProgression()),
           "state off-thread: an undo before the async apply finds the loaded song (cleared stacks), nothing reverts it");
}

void scriptedSession (ChordbloomProcessor& p)
{
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
    setParam (p, id::arpOn, 1.0f);
    setParam (p, id::arpShape, 4.0f);
    setParam (p, id::arpRate, 1.0f);
    setParam (p, id::arpRhythm, 2.0f);
    setParam (p, id::arpOctave, 1.0f);
    setParam (p, id::arpGate, 0.6f);
    setParam (p, id::bassMode, 6.0f);
    setParam (p, id::swing, 0.3f);
    setParam (p, id::humanize, 0.7f);
    pumpMessages (40);
    p.setMelody (makeMelody ({ melodyNote (72, 0.0, 1.0), melodyNote (74, 2.5, 0.5), melodyNote (79, 17.0, 1.0) }));
    auto arp = p.getLayerNotes (Layer::Arp);
    arp.notes.push_back (melodyNote (90, 3.0, 0.25));
    p.setLayerNotes (Layer::Arp, arp);
    auto bass = p.getLayerNotes (Layer::Bass);
    bass.notes.push_back (melodyNote (50, 9.0, 0.5));
    p.setLayerNotes (Layer::Bass, bass);
    p.addChord (Em_, 1);
    auto slot = p.getProgression().slots[2];
    slot.inversion = 1;
    p.updateSlot (2, slot);
    p.transposeProgression (3);
    p.removeSlot (0);
    p.undo();
    p.redo();
    p.resetLayer (Layer::Arp, 1);
    p.generateMelody (cb::MelodyGenSettings(), 12345u);
    pumpMessages (20);
}

void testDeterminism()
{
    ChordbloomProcessor a, b;
    scriptedSession (a);
    scriptedSession (b);
    auto strip = [] (ChordbloomProcessor& p)
    {
        auto xml = stateXml (p);
        if (xml == nullptr)
            return juce::String();
        if (auto* e = xml->getChildByName ("Editor"))
            xml->removeChildElement (e, true);
        return xml->toString();
    };
    const auto sa = strip (a), sb = strip (b);
    check (sa.isNotEmpty() && sa == sb, "determinism: two processors driven by the same calls save byte-identical state XML");
    check (sameEvents (a.renderProgressionEvents(), b.renderProgressionEvents())
               && sameEvents (a.renderLayerEvents (Layer::Arp), b.renderLayerEvents (Layer::Arp))
               && sameEvents (a.renderLayerEvents (Layer::Bass), b.renderLayerEvents (Layer::Bass)) && a.getMelody() == b.getMelody()
               && a.getEditedSlots (Layer::Bass) == b.getEditedSlots (Layer::Bass),
           "determinism: ... and identical renders, layers and melody");
}

//==============================================================================
// Generator (WP1) through the processor.
void testGenerate (const Deps& d)
{
    const char* needs = "WP1 generateMelody";
    ChordbloomProcessor p;
    check (! p.generateMelody (cb::MelodyGenSettings(), 7u) && p.getMelody().empty() && ! p.canUndo(),
           "generate: refused without chords");

    p.setProgression (makeProgression ({ Am_, F_, C_, G_ }));
    const auto parked = melodyNote (84, 17.0, 1.0);
    p.setMelody (makeMelody ({ melodyNote (60, 1.0, 1.0), parked }));
    const int depth = undoDepth (p);
    cb::MelodyGenSettings settings;
    settings.style = cb::MelodyStyle::Uplifting;
    const bool ok = p.generateMelody (settings, 7u);
    const auto generated = p.getMelody();
    const auto direct = cb::generateMelody (p.getProgression(), p.getKey(), p.getMelodyGenContext(), 7u, settings);
    checkAfter (d.gen, needs, ok && undoDepth (p) == depth + 1 && p.getLastGeneratorSeed() == 7u,
                "generate: one undo step, seed kept");
    checkAfter (d.gen, needs, notesStartingIn (generated, 0.0, 16.0) == direct && ! direct.empty(),
                "generate: the loop's notes are cb::generateMelody with getMelodyGenContext()");
    checkAfter (d.gen, needs, std::count (generated.notes.begin(), generated.notes.end(), parked) == 1,
                "generate: parked notes (past the loop end) stay");

    ChordbloomProcessor q;
    q.setProgression (makeProgression ({ Am_, F_, C_, G_ }));
    q.setMelody (makeMelody ({ melodyNote (60, 1.0, 1.0), parked }));
    const bool qOk = q.generateMelody (settings, 7u);
    checkAfter (d.gen, needs, qOk && q.isMelodyFromGenerator(), "generate: remembered as generated");
    const int qDepth = undoDepth (q);   // (undo / redo forget the generator result)
    checkAfter (d.gen, needs, q.getMelody() == generated, "generate: deterministic per seed (two processors)");
    q.generateMelody (settings, 7u);
    checkAfter (d.gen, needs, q.getMelody() == generated, "generate: the same seed again reproduces it");
    checkAfter (d.gen, needs, ! q.generateMelody (settings, 7u) && undoDepth (q) == qDepth,
                "generate: the same result again -> false, no new undo step");
    q.generateMelody (settings, 8u);
    checkAfter (d.gen, needs, q.getMelody() != generated, "generate: another seed gives another melody");

    // isMelodyFromGenerator: false after an edit, an undo, a redo and a state load
    ChordbloomProcessor g;
    g.setProgression (makeProgression ({ Am_, F_, C_, G_ }));
    g.generateMelody (settings, 3u);
    const bool fresh = g.isMelodyFromGenerator();
    auto edited = g.getMelody();
    edited.notes.push_back (melodyNote (96, 15.0, 0.5));
    g.setMelody (edited);
    const bool afterEdit = g.isMelodyFromGenerator();
    g.undo();
    const bool afterUndo = g.isMelodyFromGenerator();
    g.generateMelody (settings, 9u);
    const bool regenerated = g.isMelodyFromGenerator();
    g.undo();
    g.redo();
    const bool afterRedo = g.isMelodyFromGenerator();
    g.generateMelody (settings, 10u);
    juce::MemoryBlock st;
    g.getStateInformation (st);
    g.setStateInformation (st.getData(), (int) st.getSize());
    const bool afterLoad = g.isMelodyFromGenerator();
    checkAfter (d.gen, needs, fresh && ! afterEdit && ! afterUndo && regenerated && ! afterRedo && ! afterLoad && g.getLastGeneratorSeed() == 10u,
                "generate: isMelodyFromGenerator false after an edit, an undo, a redo and a state load (even of the same melody)",
                str (fresh, afterEdit, afterUndo, regenerated, afterRedo, afterLoad));

    // Parked notes keep their share of the 512-note cap: the generated notes fill only what is left, and with no room
    // left the generator changes nothing.
    {
        auto parkedNotes = [] (int count)
        {
            cb::Melody m;
            for (int i = 0; i < count; ++i)
                m.notes.push_back (melodyNote (48 + i % 12, 16.0 + (i / 12) * 0.5, 0.25));
            return cb::sanitiseMelody (m);
        };
        auto countFrom = [] (const cb::Melody& m, double from)
        {
            return (int) std::count_if (m.notes.begin(), m.notes.end(), [from] (const cb::MelodyNote& n) { return n.startBeat >= from; });
        };

        ChordbloomProcessor full;
        full.setProgression (makeProgression ({ Am_, F_, C_, G_ }));
        const auto all512 = parkedNotes (512);
        full.setMelody (all512);
        const int fullDepth = undoDepth (full);
        bool anyTrue = false;
        for (std::uint32_t seed = 1; seed <= 4; ++seed)
            anyTrue = full.generateMelody (settings, seed) || anyTrue;
        check (all512.notes.size() == 512 && ! anyTrue && full.getMelody() == all512 && undoDepth (full) == fullDepth,
               "generate: 512 parked notes leave no room -> false, melody and undo depth unchanged");

        ChordbloomProcessor some;
        some.setProgression (makeProgression ({ Am_, F_, C_, G_ }));
        const auto parked500 = parkedNotes (500);
        some.setMelody (parked500);
        bool done = false;
        for (std::uint32_t seed = 1; seed <= 8 && ! done; ++seed)
            done = some.generateMelody (settings, seed);
        const auto& out = some.getMelody();
        bool allKept = true;
        for (auto& n : parked500.notes)
            allKept = allKept && std::count (out.notes.begin(), out.notes.end(), n) == 1;
        check (allKept && countFrom (out, 16.0) == 500 && out.notes.size() <= 512,
               "generate: every parked note stays when the generated notes would exceed the cap");
        checkAfter (d.gen, needs, done && out.notes.size() > 500,
                    "generate: ... and the generated notes fill the room that is left", str (out.notes.size()));
    }

    // The generator's context (own worktree): chords, the audible bass, the arp only when on
    ChordbloomProcessor c;
    c.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
    c.setLayerNotes (Layer::Arp, customArpLine());
    auto channels = [] (const cb::MelodyGenContext& ctx)
    {
        std::set<int> s;
        for (auto& e : ctx.accompaniment)
            s.insert (e.channel);
        return s;
    };
    const auto ctx0 = c.getMelodyGenContext();
    check (ctx0.voicings == c.getProgressionVoicings() && channels (ctx0) == std::set<int> { kChords, kBass },
           "generate context: voicings + chords + bass, no arp while it is off");
    setParam (c, id::arpOn, 1.0f);
    setParam (c, id::bassOn, 0.0f);
    pumpMessages (40);
    const auto ctx1 = c.getMelodyGenContext();
    const bool sorted = std::is_sorted (ctx1.accompaniment.begin(), ctx1.accompaniment.end(),
                                        [] (const cb::NoteEvent& x, const cb::NoteEvent& y) { return x.startBeat < y.startBeat; });
    check (channels (ctx1) == std::set<int> { kChords, kArp } && sorted, "generate context: the arp when on, no muted bass, sorted");
    c.setLayerNotes (Layer::Bass, customBassLine());
    setParam (c, id::bassOn, 1.0f);
    pumpMessages (40);
    check (eventsOnChannel (c.getMelodyGenContext().accompaniment, kBass).size() == customBassLine().notes.size(),
           "generate context: a custom bass counts as the audible bass");
}

//==============================================================================
// Export (6.4).
juce::MidiFile loopLengthCopy (const juce::MidiFile& in, double loopBeats)   // = the processor's withLoopLength
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

juce::MemoryBlock bytesOf (const juce::MidiFile& mf)
{
    juce::MemoryOutputStream os;
    mf.writeTo (os, 1);
    return os.getMemoryBlock();
}

void testExport (const Deps& d)
{
    ChordbloomProcessor p;
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
    pumpMessages (20);
    const double loopTicks = 16.0 * 960.0;

    // Both / Bass are byte-identical to the phase-1.5 output for auto layers
    {
        const auto events = cb::renderProgression (p.getProgression(), p.getProgressionVoicings(), p.getRenderSettings());
        const auto bothWant = bytesOf (loopLengthCopy (cb::midiexport::buildMidiFile (events, 120.0, true, true), 16.0));
        const auto bassWant = bytesOf (loopLengthCopy (cb::midiexport::buildMidiFile (events, 120.0, false, true), 16.0));
        juce::MemoryBlock bothGot, bassGot;
        p.writeMidiToTempFile (Part::Both).loadFileAsData (bothGot);
        p.writeMidiToTempFile (Part::Bass).loadFileAsData (bassGot);
        check (bothGot == bothWant && bothGot.getSize() > 0, "export: Both is byte-identical to the phase-1.5 output (auto bass)");
        check (bassGot == bassWant && bassGot.getSize() > 0, "export: Bass is byte-identical to the phase-1.5 output (auto bass)");
    }

    check (p.getExportFileName (Part::Arp) == "C Am F G - C major - Arp.mid"
               && p.getExportFileName (Part::All) == "C Am F G - C major - All parts.mid",
           "export: file names \"... - Arp.mid\" / \"... - All parts.mid\"");

    // The auto arp exports while muted (WP2); without arp notes the Arp export is refused
    {
        const auto file = p.writeMidiToTempFile (Part::Arp);
        checkAfter (d.arp, "WP2 renderArp", file.existsAsFile() && readMidi (file).getNumTracks() == 2,
                    "export: the auto arp exports while the arp is off");
        if (! d.arp)
            check (file == juce::File(), "export: no arp notes -> the Arp export is refused");
    }

    // Custom bass and custom arp
    const auto bassLine = customBassLine();
    p.setLayerNotes (Layer::Bass, bassLine);
    cb::Melody lateArp;   // a "Late start"-like arp that ends early (beat 10)
    for (int i = 1; i < 10; ++i)
        lateArp.notes.push_back (melodyNote (70 + i, i, 0.5));
    lateArp = cb::sanitiseMelody (lateArp, cb::layerLimits (Layer::Arp));
    p.setLayerNotes (Layer::Arp, lateArp);
    {
        const auto bassMf = readMidi (p.writeMidiToTempFile (Part::Bass));
        const auto bothMf = readMidi (p.writeMidiToTempFile (Part::Both));
        check (noteOnsIn (bassMf, 1, kBass) == (int) bassLine.notes.size() && noteOnsIn (bothMf, 2, kBass) == (int) bassLine.notes.size()
                   && trackNameOf (bothMf, 2) == "Chordbloom Bass" && noteOnsIn (bothMf, 1, kChords) > 0,
               "export: Bass and Both use the custom bass");

        const auto arpFile = p.writeMidiToTempFile (Part::Arp);
        const auto arpMf = readMidi (arpFile);
        check (arpFile.getFileName() == p.getExportFileName (Part::Arp) && arpMf.getNumTracks() == 2
                   && trackNameOf (arpMf, 1) == "Chordbloom Arp" && noteOnsIn (arpMf, 1, kArp) == (int) lateArp.notes.size()
                   && noteOnsIn (arpMf, 1) == noteOnsIn (arpMf, 1, kArp),
               "export: Arp = one track \"Chordbloom Arp\" on channel 4 (exported while the arp is off)");
        check (std::abs (endOfTrack (arpMf, 1) - loopTicks) < 0.5 && std::abs (endOfTrack (arpMf, 0) - loopTicks) < 0.5,
               "export: the Arp track ends at the loop length although its last note ends at beat 10",
               str (endOfTrack (arpMf, 1)));
    }

    // All = the playing parts, in order, each one loop long
    p.setMelody (makeMelody ({ melodyNote (72, 0.0, 1.0), melodyNote (76, 8.0, 2.0) }));   // final gap after beat 10
    setParam (p, id::arpOn, 1.0f);
    pumpMessages (40);
    {
        const auto allFile = p.writeMidiToTempFile (Part::All);
        const auto all = readMidi (allFile);
        bool loopLong = all.getNumTracks() == 5;
        for (int t = 0; t < all.getNumTracks(); ++t)
            loopLong = loopLong && std::abs (endOfTrack (all, t) - loopTicks) < 0.5;
        check (allFile.getFileName() == p.getExportFileName (Part::All) && all.getNumTracks() == 5
                   && trackNameOf (all, 1) == "Chordbloom Chords" && trackNameOf (all, 2) == "Chordbloom Bass"
                   && trackNameOf (all, 3) == "Chordbloom Melody" && trackNameOf (all, 4) == "Chordbloom Arp"
                   && noteOnsIn (all, 1, kChords) == noteOnsIn (all, 1) && noteOnsIn (all, 2, kBass) == (int) bassLine.notes.size()
                   && noteOnsIn (all, 3, kMel) == 2 && noteOnsIn (all, 4, kArp) == (int) lateArp.notes.size(),
               "export: All = Chords, Bass, Melody, Arp tracks in order on their channels");
        check (loopLong, "export: every All track ends at the loop length (melody with a final gap, early-ending arp)");
    }
    setParam (p, id::arpOn, 0.0f);
    p.clearMelody();
    pumpMessages (40);
    {
        const auto all = readMidi (p.writeMidiToTempFile (Part::All));
        check (all.getNumTracks() == 3 && trackNameOf (all, 2) == "Chordbloom Bass", "export: All leaves out parts that do not play");
    }

    // Slot drags stay chords only
    setParam (p, id::arpOn, 1.0f);
    pumpMessages (40);
    {
        const auto slotMf = readMidi (p.writeSlotMidiToTempFile (1));
        int other = 0, chords = 0;
        for (int t = 0; t < slotMf.getNumTracks(); ++t)
            for (auto* h : *slotMf.getTrack (t))
                if (h->message.isNoteOn())
                    (h->message.getChannel() == kChords ? chords : other)++;
        check (chords > 0 && other == 0, "export: slot drags stay chords only (custom bass and arp left out)");
    }

    // buildTracksMidiFile: empty entries left out, no padding at loopBeats <= 0, offs before ons on a tick
    {
        std::vector<cb::midiexport::ExportTrack> tracks;
        tracks.push_back ({ "A", { { 60, 0.8f, 0.0, 1.0, 1 }, { 60, 0.8f, 1.0, 1.0, 1 } } });
        tracks.push_back ({ "Empty", {} });
        tracks.push_back ({ "B", { { 40, 0.8f, 0.5, 0.25, 2 } } });
        const auto mf = cb::midiexport::buildTracksMidiFile (tracks, 100.0, 0.0);
        bool offFirst = false;
        if (mf.getNumTracks() == 3)
        {
            const juce::MidiMessage* prev = nullptr;
            for (auto* h : *mf.getTrack (1))
            {
                if (prev != nullptr && prev->isNoteOff() && h->message.isNoteOn()
                    && std::abs (prev->getTimeStamp() - h->message.getTimeStamp()) < 0.5)
                    offFirst = true;
                prev = &h->message;
            }
        }
        check (mf.getNumTracks() == 3 && trackNameOf (mf, 1) == "A" && trackNameOf (mf, 2) == "B" && offFirst
                   && std::abs (endOfTrack (mf, 1) - 1920.0) < 0.5 && std::abs (endOfTrack (mf, 2) - 720.0) < 0.5,
               "export: buildTracksMidiFile (empty entries out, offs before ons, no padding without a loop length)");
        const auto padded = cb::midiexport::buildTracksMidiFile (tracks, 100.0, 8.0);
        check (padded.getNumTracks() == 3 && std::abs (endOfTrack (padded, 2) - 8.0 * 960.0) < 0.5
                   && std::abs (endOfTrack (padded, 0) - 8.0 * 960.0) < 0.5,
               "export: buildTracksMidiFile pads every track to loopBeats");
    }

    ChordbloomProcessor empty;
    check (empty.writeMidiToTempFile (Part::Arp) == juce::File() && empty.writeMidiToTempFile (Part::All) == juce::File()
               && ! empty.saveMidiFile (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("cb-none.mid"), Part::All),
           "export: Arp / All refused without chords");
}

//==============================================================================
// PreviewSynth: 48 slots, released voices stolen first, re-strike reuses the voice.
void testPreviewSynthStealing()
{
    {
        PreviewSynth synth;
        synth.prepare (48000.0, 512);
        synth.setTone (PreviewSynth::Tone::WarmPad);
        synth.setGainDecibels (-9.0f);
        juce::AudioBuffer<float> buf (2, 512);

        struct Timed { juce::int64 sample; juce::MidiMessage msg; };
        std::vector<Timed> events;
        for (int n : { 48, 55, 60, 64 })
            events.push_back ({ 0, juce::MidiMessage::noteOn (1, n, (juce::uint8) 90) });
        const double spb = 24000.0, step = 0.25 * spb;
        for (int k = 0; k < 128; ++k)   // 8 bars of 1/16 at 120 bpm
        {
            const auto at = (juce::int64) (k * step);
            const int arpNote = 72 + (k % 3) * 4 + (k / 16 % 2) * 12;
            const int bassNote = 36 + (k % 2) * 12;
            events.push_back ({ at, juce::MidiMessage::noteOn (4, arpNote, (juce::uint8) 96) });
            events.push_back ({ at + (juce::int64) (0.9 * step), juce::MidiMessage::noteOff (4, arpNote) });
            events.push_back ({ at, juce::MidiMessage::noteOn (2, bassNote, (juce::uint8) 96) });
            events.push_back ({ at + (juce::int64) (0.96 * step), juce::MidiMessage::noteOff (2, bassNote) });
        }
        std::stable_sort (events.begin(), events.end(), [] (const Timed& a, const Timed& b)
        {
            if (a.sample != b.sample)
                return a.sample < b.sample;
            return a.msg.isNoteOff() && ! b.msg.isNoteOff();
        });

        size_t next = 0;
        const auto total = (juce::int64) (32.0 * spb);
        bool finite = true;
        for (juce::int64 pos = 0; pos < total; pos += 512)
        {
            juce::MidiBuffer midi;
            while (next < events.size() && events[next].sample < pos + 512)
            {
                midi.addEvent (events[next].msg, (int) (events[next].sample - pos));
                ++next;
            }
            buf.clear();
            synth.render (buf, midi, 512);
            finite = finite && std::isfinite (buf.getMagnitude (0, 0, 512));
        }
        check (synth.getHeldVoiceSteals() == 0 && finite,
               "synth: a held Warm Pad chord survives 8 bars of 1/16 arp + rolling bass (no held voice stolen)",
               str (synth.getHeldVoiceSteals(), " held steals"));
    }

    {
        PreviewSynth synth;
        synth.prepare (48000.0, 256);
        juce::AudioBuffer<float> buf (2, 256);
        auto renderWith = [&] (const juce::MidiBuffer& m)
        {
            buf.clear();
            synth.render (buf, m, 256);
        };
        juce::MidiBuffer fill;
        for (int i = 0; i < 48; ++i)
            fill.addEvent (juce::MidiMessage::noteOn (1, 30 + i, (juce::uint8) 100), 0);
        renderWith (fill);
        juce::MidiBuffer again;
        again.addEvent (juce::MidiMessage::noteOn (1, 40, (juce::uint8) 100), 0);
        renderWith (again);
        check (synth.getHeldVoiceSteals() == 0, "synth: with 48 held notes a re-struck note reuses its own voice");
        juce::MidiBuffer extra;
        extra.addEvent (juce::MidiMessage::noteOn (1, 100, (juce::uint8) 100), 0);
        renderWith (extra);
        check (synth.getHeldVoiceSteals() == 1, "synth: a 49th held note steals one held voice (counted)");
        juce::MidiBuffer release;
        release.addEvent (juce::MidiMessage::noteOff (1, 31), 0);
        renderWith (release);
        juce::MidiBuffer another;
        another.addEvent (juce::MidiMessage::noteOn (1, 101, (juce::uint8) 100), 0);
        renderWith (another);
        check (synth.getHeldVoiceSteals() == 1, "synth: a released voice is stolen before any held one");
        synth.prepare (48000.0, 256);
        check (synth.getHeldVoiceSteals() == 0, "synth: prepare() resets the steal counter");
    }
}

//==============================================================================
// 2000 random operations while playing: balanced MIDI out and synth stream, no exception.
struct XorShift
{
    std::uint32_t s = 0x9e3779b9u;
    std::uint32_t next() noexcept
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    int below (int n) noexcept { return n <= 0 ? 0 : (int) (next() % (std::uint32_t) n); }
};

cb::Melody randomNotes (XorShift& rng, int low, int high, int count)
{
    cb::Melody m;
    for (int i = 0; i < count; ++i)
        m.notes.push_back (melodyNote (low + rng.below (high - low + 1), rng.below (96) * 0.25, 0.25 + rng.below (12) * 0.25,
                                       0.1f + 0.08f * (float) rng.below (11)));
    return m;
}

// What the processor hands to its PreviewSynth, checked like the MIDI out: per (channel, note) on / off pairing.
// Fed on the audio thread through cbdev::setSynthStreamObserver; fixed tables, no allocation.
struct SynthStreamCheck
{
    const ChordbloomProcessor* processor = nullptr;
    std::array<std::array<std::uint8_t, 128>, 16> open {};
    int ons = 0, offs = 0, doubleOns = 0, strayOffs = 0, clears = 0;

    static void observe (void* context, const ChordbloomProcessor& from, const juce::MidiBuffer* midi)
    {
        auto& self = *static_cast<SynthStreamCheck*> (context);
        if (&from != self.processor)
            return;
        if (midi == nullptr)
        {
            ++self.clears;
            for (auto& ch : self.open)
                ch.fill (0);
            return;
        }
        for (const auto meta : *midi)
        {
            if (meta.numBytes < 3)
                continue;
            const int status = meta.data[0] & 0xf0;
            const int ch = meta.data[0] & 0x0f;
            const int note = meta.data[1] & 0x7f;
            auto& slot = self.open[(size_t) ch][(size_t) note];
            if (status == 0x90 && meta.data[2] > 0)
            {
                ++self.ons;
                self.doubleOns += slot != 0 ? 1 : 0;
                slot = 1;
            }
            else if (status == 0x80 || status == 0x90)
            {
                ++self.offs;
                self.strayOffs += slot == 0 ? 1 : 0;
                slot = 0;
            }
        }
    }

    int openCount() const
    {
        int n = 0;
        for (auto& ch : open)
            for (auto v : ch)
                n += v != 0 ? 1 : 0;
        return n;
    }
};

void testEngineFuzz()
{
    ChordbloomProcessor p;
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }, 2));
    SynthStreamCheck synthStream;
    synthStream.processor = &p;
    cbdev::setSynthStreamObserver (&SynthStreamCheck::observe, &synthStream);
    Rig rig (p, 44100.0, 256);
    rig.head.playing = true;
    XorShift rng;
    const cb::Chord chords[] = { C_, Am_, F_, G_, Em_, { 2, cb::ChordQuality::Dominant13 }, { 7, cb::ChordQuality::Sus4 } };
    const char* params[] = { id::arpOn, id::arpShape, id::arpRate, id::arpRhythm, id::arpOctave, id::arpGate, id::bassOn,
                             id::bassMode, id::midiOutPart, id::melodyOn, id::swing, id::humanize, id::pattern, id::rate };
    const int maxOf[] = { 1, 16, 3, 6, 4, 1, 1, 14, 5, 1, 1, 1, 7, 3 };

    bool threw = false;
    try
    {
        for (int step = 0; step < 2000; ++step)
        {
            const auto& prog = p.getProgression();
            const int n = (int) prog.slots.size();
            const auto layer = (Layer) rng.below (3);
            switch (rng.below (17))
            {
                case 0:  p.setLayerNotes (Layer::Arp, randomNotes (rng, 30, 105, rng.below (24))); break;
                case 1:  p.setLayerNotes (Layer::Bass, randomNotes (rng, 24, 72, rng.below (12))); break;
                case 2:  p.resetLayer (layer, rng.below (6) - 1); break;
                case 3:
                    switch (rng.below (5))
                    {
                        case 0:  p.addChord (chords[rng.below (7)], rng.below (n + 2) - 1); break;
                        case 1:  if (n > 1) p.removeSlot (rng.below (n)); break;
                        case 2:  p.moveSlot (rng.below (juce::jmax (1, n)), rng.below (juce::jmax (1, n))); break;
                        case 3:  p.duplicateSlot (rng.below (juce::jmax (1, n))); break;
                        default:
                            if (n > 0)
                            {
                                auto s = prog.slots[(size_t) rng.below (n)];
                                s.lengthBeats = 1 + rng.below (6);
                                p.updateSlot (rng.below (n), s);
                            }
                            break;
                    }
                    break;
                case 4:
                    if (n > 0)
                    {
                        auto s = prog.slots[(size_t) rng.below (n)];
                        s.chord = chords[rng.below (7)];
                        s.inversion = rng.below (4) - 1;
                        p.updateSlot (rng.below (n), s);
                    }
                    break;
                case 5:  p.transposeProgression (rng.below (13) - 6); break;
                case 6:  p.generateMelody (cb::MelodyGenSettings(), rng.next()); break;
                case 7:  if (rng.below (2) == 0) p.undo(); else p.redo(); break;
                case 8:
                case 9:
                {
                    const int k = rng.below ((int) std::size (params));
                    const bool continuous = k == 5 || k == 10 || k == 11;
                    const float v = continuous ? (float) rng.below (101) / 100.0f : (float) rng.below (maxOf[k] + 1);
                    setParam (p, params[k], k == 4 ? v - 2.0f : v);
                    break;
                }
                case 10:
                {
                    juce::MemoryBlock st;
                    p.getStateInformation (st);
                    p.setStateInformation (st.getData(), (int) st.getSize());
                    break;
                }
                case 11: p.startPreviewLayerNote (layer, 30 + rng.below (70)); break;
                case 12: p.stopPreview(); break;
                case 13: rig.head.ppq = rng.below (64) * 0.5; break;
                case 14: p.setMelody (randomNotes (rng, 40, 90, rng.below (16))); break;
                case 15:
                    setParamAsUser (p, id::arpShape, (float) rng.below (17));
                    p.beginUserLayerPick (Layer::Arp);
                    break;
                default: p.setLayerOn (layer, rng.below (2) == 1); break;
            }

            if (p.getProgression().empty())
                p.setProgression (makeProgression ({ C_, G_ }, 2));
            if (step % 8 == 0)
                pumpMessages (1);
            rig.run (1 + rng.below (4));
        }
    }
    catch (...)
    {
        threw = true;
    }

    p.stopPreview();
    rig.head.playing = false;
    rig.run (3);
    cbdev::setSynthStreamObserver (nullptr, nullptr);
    check (! threw && rig.doubleOns == 0 && rig.strayOffs == 0 && rig.openCount() == 0 && p.getSoundingNotes().none() && rig.audioFinite,
           "engine fuzz: 2000 random layer / edit / parameter / state operations while playing stay balanced",
           str (threw ? "threw, " : "", rig.doubleOns, " double, ", rig.strayOffs, " stray, ", rig.openCount(), " open"));
    check (synthStream.ons > 1000 && synthStream.ons == synthStream.offs && synthStream.doubleOns == 0 && synthStream.strayOffs == 0
               && synthStream.openCount() == 0 && synthStream.clears <= 1,
           "engine fuzz: the stream handed to the PreviewSynth stays balanced per (channel, note) too",
           str (synthStream.ons, " on, ", synthStream.offs, " off, ", synthStream.doubleOns, " double, ", synthStream.strayOffs,
                " stray, ", synthStream.openCount(), " open, ", synthStream.clears, " clears"));
}

//==============================================================================
// Worst case: 16 x 16-beat slots, 6-note 1/16 Pulse chords, Rolling 16ths, 1/16 arp, 512-note melody.
void testWorstCaseLoad (const Deps& d)
{
    ChordbloomProcessor p;
    cb::Progression big;
    for (int i = 0; i < 16; ++i)
        big.slots.push_back ({ { (i * 5) % 12, i % 2 == 0 ? cb::ChordQuality::Dominant13 : cb::ChordQuality::Minor11 }, 16, -1, 0 });
    p.setProgression (big);
    setParam (p, id::voicing, (float) cb::VoicingStyle::Spread);
    setParam (p, id::pattern, (float) cb::RhythmPattern::Pulse);
    setParam (p, id::rate, (float) cb::RateDivision::Sixteenth);
    setParam (p, id::bassMode, (float) cb::BassMode::Rolling16);
    setParam (p, id::arpOn, 1.0f);
    pumpMessages (60);
    cb::Melody m;
    for (int i = 0; i < 512; ++i)
        m.notes.push_back (melodyNote (60 + i % 12, i * 0.5, 0.4));
    p.setMelody (m);

    const size_t events = p.renderProgressionEvents().size() + p.renderLayerEvents (Layer::Arp).size() + p.renderMelodyEvents().size();
    check (events <= 12000 && events > 4000, "load: worst-case EngineData <= 12k events", str (events, " events"));
    checkAfter (d.arp && d.patterns, "WP2 arp + patterns", events > 7000, "load: the worst case really is ~9k events", str (events));

    Rig rig (p);
    rig.head.playing = true;
    rig.head.ppq = 250.0;
    rig.run (4);
    std::vector<double> ms;
    for (int trial = 0; trial < 7; ++trial)
    {
        auto variant = m;
        variant.notes[(size_t) trial].note += 1;
        p.setMelody (variant);   // publishes a new EngineData
        const auto t0 = juce::Time::getHighResolutionTicks();
        rig.processBlock();
        const auto t1 = juce::Time::getHighResolutionTicks();
        ms.push_back (juce::Time::highResolutionTicksToSeconds (t1 - t0) * 1000.0);
        rig.run (2);
    }
    std::sort (ms.begin(), ms.end());
    check (ms[ms.size() / 2] < 2.0, "load: a processBlock (512 @ 48 kHz) that picks up a new EngineData takes < 2 ms",
           str ("median ", ms[ms.size() / 2], " ms, max ", ms.back(), " ms"));

    // 2 s of arpGate automation at 20 Hz from a host thread (no async update: only the processor's 20 Hz timer picks
    // it up), while the rig plays in real time and the message loop runs.
    rig.head.ppq = 0.0;
    pumpMessages (20);
    std::atomic<bool> automationDone { false };
    std::thread host ([&]
    {
        for (int i = 0; i < 40; ++i)
        {
            setParam (p, id::arpGate, 0.05f + 0.095f * (float) (i % 11));
            juce::Thread::sleep (50);
        }
        automationDone.store (true);
    });

    std::vector<Ev> played;
    int publications = 0;
    auto lastSerial = p.getLayerSerial();
    const double blockMs = 1000.0 * rig.blockSize / rig.sr;
    auto due = juce::Time::getMillisecondCounterHiRes();
    for (int guard = 0; ! automationDone.load() && guard < 2000; ++guard)
    {
        auto evs = rig.processBlock();
        played.insert (played.end(), evs.begin(), evs.end());
        due += blockMs;
        pumpMessages (juce::jmax (0, (int) (due - juce::Time::getMillisecondCounterHiRes())));
        if (const auto serial = p.getLayerSerial(); serial != lastSerial)
        {
            ++publications;
            lastSerial = serial;
        }
    }
    host.join();

    // Note lengths of the arp (quantised to 64 samples): the gate changes must reach playback.
    std::map<int, juce::int64> arpOnAt;
    std::set<juce::int64> arpLengths;
    for (auto& e : played)
    {
        if (e.channel != kArp)
            continue;
        if (e.on)
            arpOnAt[e.note] = e.sample;
        else if (auto it = arpOnAt.find (e.note); it != arpOnAt.end())
        {
            arpLengths.insert ((e.sample - it->second) / 64);
            arpOnAt.erase (it);
        }
    }

    rig.head.playing = false;
    rig.run (2);
    check (publications >= 10, "load: arpGate automation from a host thread is published by the 20 Hz timer",
           str (publications, " publications"));
    checkAfter (d.arp, "WP2 renderArp", arpLengths.size() >= 3, "load: ... and the arp's note-off timing follows the gate",
                str (arpLengths.size(), " distinct arp note lengths"));
    check (rig.openCount() == 0 && rig.doubleOns == 0 && rig.strayOffs == 0 && p.getSoundingNotes().none(),
           "load: 2 s of arpGate automation leaves no hanging note");
}
} // namespace

void cbtest::runEngineSelfTests()
{
    const auto deps = detectDeps();
    std::cout << "engine selftests: core available: arp " << deps.arp << ", bass patterns " << deps.patterns << ", layers "
              << deps.layers << ", limits " << deps.limits << ", follow " << deps.follow << ", edit " << deps.edit
              << ", generator " << deps.gen << "\n";

    auto timed = [] (const char* name, const std::function<void()>& test)
    {
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        test();
        std::cout << "engine selftests: " << name << " took "
                  << juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - t0) << " ms\n";
    };

    timed ("testLayerSerialContract", [&] { testLayerSerialContract(); });
    timed ("testLayerSerialParams", [&] { testLayerSerialParams(); });
    timed ("testParameters", [&] { testParameters(); });
    timed ("testGoldenV1State", [&] { testGoldenV1State(); });
    timed ("testBassLayer", [&] { testBassLayer (deps); });
    timed ("testArpChannelAndOutPart", [&] { testArpChannelAndOutPart(); });
    timed ("testRenderClasses", [&] { testRenderClasses(); });
    timed ("testAutoArp", [&] { testAutoArp (deps); });
    timed ("testCustomLayers", [&] { testCustomLayers (deps); });
    timed ("testNormaliseScope", [&] { testNormaliseScope (deps); });
    timed ("testUndoAcrossLayers", [&] { testUndoAcrossLayers(); });
    timed ("testUserPick", [&] { testUserPick (deps); });
    timed ("testFollow", [&] { testFollow (deps); });
    timed ("testLayerState", [&] { testLayerState (deps); });
    timed ("testStateConsistencyOffThread", [&] { testStateConsistencyOffThread(); });
    timed ("testDeterminism", [&] { testDeterminism(); });
    timed ("testGenerate", [&] { testGenerate (deps); });
    timed ("testExport", [&] { testExport (deps); });
    timed ("testPreviewSynthStealing", [&] { testPreviewSynthStealing(); });
    timed ("testEngineFuzz", [&] { testEngineFuzz(); });
    timed ("testWorstCaseLoad", [&] { testWorstCaseLoad (deps); });
    std::cout << "engine selftests: " << numDeferred << " deferred (must be 0 after the integration merge)\n";
}
