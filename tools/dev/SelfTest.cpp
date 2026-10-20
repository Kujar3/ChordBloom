#include "plugin/MidiExport.h"
#include "plugin/PluginProcessor.h"
#include "plugin/PreviewSynth.h"
#include "ui/ChordPaletteView.h"
#include "ui/KeyboardGuide.h"
#include "ui/MelodyRoll.h"
#include "ui/PluginEditor.h"
#include "ui/ProgressionLane.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

#include "SelfTestUtil.h"

using namespace cbtest;

// Processor integration tests (owner: processor agent). Print one line per check; return 0 when all pass.
//
// The tests drive ChordbloomProcessor exactly like a host: prepareToPlay, a fake AudioPlayHead, processBlock with
// real MidiBuffers, getState/setState. The dev tool is a console app, so the JUCE message queue is pumped by hand to
// let the processor's AsyncUpdater / Timer (parameter-change re-render) run.

namespace
{

//==============================================================================
void testBasicsAndDefaults()
{
    ChordbloomProcessor p;
    namespace id = cb::ParamIDs;

    check (p.getName() == "Chordbloom" && p.acceptsMidi() && p.producesMidi() && ! p.isMidiEffect(), "basics: name and MIDI flags");
    check (p.getTotalNumOutputChannels() == 2 && p.getTotalNumInputChannels() == 0, "basics: stereo out, no audio in");

    struct Expect { const char* id; float value; };
    const Expect defaults[] = {
        { id::keyRoot, 0 }, { id::scale, 0 }, { id::octave, 0 }, { id::voicing, 0 }, { id::voiceLeading, 1 },
        { id::pattern, 0 }, { id::rate, 1 }, { id::bassMode, 1 }, { id::velocity, 0.75f }, { id::gate, 0.9f },
        { id::swing, 0 }, { id::humanize, 0 }, { id::previewOn, 1 }, { id::previewTone, 0 }, { id::previewVolume, -9 },
        { id::midiOut, 1 }, { id::syncToHost, 1 }, { id::scaleLock, 0 }, { id::chordTrigger, 0 }, { id::triggerSevenths, 0 },
        { id::midiOutPart, 0 },
    };

    for (auto& d : defaults)
    {
        const float v = getParam (p, d.id);
        check (std::abs (v - d.value) < 1.0e-4f, str ("default ", d.id, " == ", d.value), str ("got ", v));
    }

    check (p.getParameters().size() == 29, "parameter count == 29", str ("got ", p.getParameters().size()));
    {
        auto* sync = p.getAPVTS().getParameter (id::syncToHost);
        check (sync != nullptr && sync->getName (64) == "Play With Host", "syncToHost is shown as \"Play With Host\"",
               sync != nullptr ? sync->getName (64).toStdString() : std::string ("missing"));
        auto* part = dynamic_cast<juce::AudioParameterChoice*> (p.getAPVTS().getParameter (id::midiOutPart));
        check (part != nullptr && part->choices.size() == 6 && part->choices[0] == "All parts"
                   && part->choices[1] == "Chords only" && part->choices[2] == "Bass only"
                   && part->choices[3] == "Melody only" && part->choices[4] == "Chords + Bass"
                   && part->choices[5] == "Arp only",
               "midiOutPart choices (old indices keep their meaning; melody and arp parts appended)");
        auto* melodyOn = dynamic_cast<juce::AudioParameterBool*> (p.getAPVTS().getParameter (id::melodyOn));
        check (melodyOn != nullptr && melodyOn->get() && melodyOn->getName (64) == "Melody", "melodyOn defaults to on");
    }

    const auto key = p.getKey();
    check (key.tonic == 0 && key.scale == cb::ScaleType::Major, "getKey default C Major");

    const auto vs = p.getVoicingSettings();
    check (vs.centerNote == 60 && vs.style == cb::VoicingStyle::Close && vs.voiceLeading, "getVoicingSettings defaults",
           str ("center ", vs.centerNote, " style ", (int) vs.style, " vl ", vs.voiceLeading));

    const auto rs = p.getRenderSettings();
    check (rs.pattern == cb::RhythmPattern::Block && rs.rate == cb::RateDivision::Eighth && rs.bass == cb::BassMode::Root
               && std::abs (rs.velocity - 0.75f) < 1e-4f && std::abs (rs.gate - 0.9f) < 1e-4f && rs.swing == 0.0f
               && rs.humanize == 0.0f,
           "getRenderSettings defaults");

    setParam (p, id::octave, 1.0f);
    check (p.getVoicingSettings().centerNote == 72, "octave +1 -> centerNote 72", str ("got ", p.getVoicingSettings().centerNote));
    setParam (p, id::octave, -3.0f);
    check (p.getVoicingSettings().centerNote == 24, "octave -3 -> centerNote 24");

    setParam (p, id::keyRoot, 9.0f);
    setParam (p, id::scale, 1.0f);
    check (p.getKey().tonic == 9 && p.getKey().scale == cb::ScaleType::NaturalMinor, "getKey follows parameters (A Natural Minor)");
    const auto& pal = p.getPalette();
    check (pal.size() == (size_t) cb::numPaletteSections, "palette has all sections");
    check (! pal.empty() && ! pal[0].chords.empty() && pal[0].chords[0].chord.root == 9, "palette rebuilt for new key (Triads[0] root == A)");

    check (p.getSavedEditorSize() == juce::Point<int>(), "editor size {0,0} before saved");
    check (p.getPlayingSlot() == -1 && ! p.isHostPlaying() && ! p.isLocalPlaybackOn() && p.getHostBpm() == 120.0,
           "transport getters idle defaults");
    check (p.getSoundingNotes().none() && p.getTriggeredDegree() == -1 && ! p.getPreviewChord().has_value(), "live getters idle defaults");
}

//==============================================================================
void testEditingUndoRedo()
{
    ChordbloomProcessor p;
    ChangeCounter counter;
    p.addChangeListener (&counter);

    check (! p.canUndo() && ! p.canRedo(), "edit: fresh processor has no undo/redo");

    std::vector<cb::Progression> history { p.getProgression() };
    auto snap = [&] { history.push_back (p.getProgression()); };

    p.addChord (C_);   snap();
    p.addChord (Am_);  snap();
    p.addChord (F_);   snap();
    p.addChord (G_);   snap();

    const auto& prog = p.getProgression();
    check (prog.slots.size() == 4 && prog.slots[0].chord == C_ && prog.slots[1].chord == Am_ && prog.slots[2].chord == F_
               && prog.slots[3].chord == G_,
           "edit: addChord appends");
    check (prog.slots[0].lengthBeats == 4 && prog.slots[0].inversion == -1 && prog.slots[0].octaveShift == 0,
           "edit: new slot defaults (4 beats, auto inversion, octave 0)");

    p.addChord (Em_, 1); snap();
    check (p.getProgression().slots.size() == 5 && p.getProgression().slots[1].chord == Em_, "edit: addChord inserts at index");

    p.removeSlot (1); snap();
    check (p.getProgression() == history[4], "edit: removeSlot");

    p.moveSlot (0, 3); snap();
    {
        const auto& s = p.getProgression().slots;
        check (s.size() == 4 && s[0].chord == Am_ && s[1].chord == F_ && s[2].chord == G_ && s[3].chord == C_,
               "edit: moveSlot(0,3) -> final index 3");
    }
    p.moveSlot (3, 0); snap();
    check (p.getProgression() == history[4], "edit: moveSlot(3,0) restores order");

    cb::ProgressionSlot wild;
    wild.chord = { 17, cb::ChordQuality::Major };   // root out of range
    wild.lengthBeats = 99;
    wild.inversion = 9;
    wild.octaveShift = 5;
    p.updateSlot (2, wild); snap();
    {
        const auto& s = p.getProgression().slots[2];
        check (s.chord.root == 5 && s.lengthBeats == cb::kMaxSlotBeats && s.inversion == 2 && s.octaveShift == 2,
               "edit: updateSlot clamps (root 17->5, length 99->16, inversion 9->2, octave 5->2)",
               str ("root ", s.chord.root, " len ", s.lengthBeats, " inv ", s.inversion, " oct ", s.octaveShift));
    }

    check (p.duplicateSlot (0), "edit: duplicateSlot returns true");
    snap();
    check (p.getProgression().slots.size() == 5 && p.getProgression().slots[1] == p.getProgression().slots[0],
           "edit: duplicateSlot inserts copy after index");

    // invalid edits are no-ops and create no undo entries
    const auto before = p.getProgression();
    p.removeSlot (-1);
    p.removeSlot (99);
    p.moveSlot (42, 0);
    p.updateSlot (77, wild);
    check (! p.duplicateSlot (-3), "edit: duplicateSlot invalid index returns false");
    check (p.getProgression() == before, "edit: invalid indices are ignored");

    // Undo all the way back, checking every snapshot
    bool undoOk = true;
    for (int i = (int) history.size() - 2; i >= 0; --i)
    {
        if (! p.undo() || p.getProgression() != history[(size_t) i])
        {
            undoOk = false;
            break;
        }
    }
    check (undoOk, "undo: walks back through every snapshot");
    check (! p.canUndo() && p.canRedo() && ! p.undo(), "undo: stack exhausted at the start");

    bool redoOk = true;
    for (size_t i = 1; i < history.size(); ++i)
    {
        if (! p.redo() || p.getProgression() != history[i])
        {
            redoOk = false;
            break;
        }
    }
    check (redoOk, "redo: walks forward through every snapshot");
    check (! p.canRedo() && ! p.redo(), "redo: stack exhausted at the end");

    p.undo();
    check (p.canRedo(), "redo available after undo");
    p.addChord (G_);
    check (! p.canRedo(), "new edit clears redo");

    // kMaxSlots
    p.clearProgression();
    check (p.getProgression().empty(), "edit: clearProgression");
    for (int i = 0; i < cb::kMaxSlots; ++i)
        p.addChord ({ i % 12, cb::ChordQuality::Minor });
    check ((int) p.getProgression().slots.size() == cb::kMaxSlots, "edit: can hold kMaxSlots slots");
    check (! p.addChord (C_) && (int) p.getProgression().slots.size() == cb::kMaxSlots, "edit: addChord refuses beyond kMaxSlots");
    check (! p.duplicateSlot (0), "edit: duplicateSlot refuses when full");

    cb::Progression tooLong;
    for (int i = 0; i < 40; ++i)
        tooLong.slots.push_back ({ C_, 4, -1, 0 });
    p.setProgression (tooLong);
    check ((int) p.getProgression().slots.size() == cb::kMaxSlots, "edit: setProgression truncates to kMaxSlots");

    // undo depth limit
    ChordbloomProcessor q;
    q.addChord (C_);
    for (int i = 0; i < 130; ++i)
    {
        auto s = q.getProgression().slots[0];
        s.lengthBeats = 1 + (i % 16);
        if (s.lengthBeats == q.getProgression().slots[0].lengthBeats)
            s.lengthBeats = s.lengthBeats == 1 ? 2 : 1;
        q.updateSlot (0, s);
    }
    int undos = 0;
    while (q.undo())
        ++undos;
    check (undos == 100, "undo: depth limited to 100", str ("got ", undos));

    pumpMessages (30);
    check (counter.count > 0, "edits send change messages", str ("count ", counter.count));
    p.removeChangeListener (&counter);
}

//==============================================================================
void testStateRoundTrip()
{
    namespace id = cb::ParamIDs;
    juce::MemoryBlock block;

    cb::Progression prog;
    prog.slots.push_back ({ { 9, cb::ChordQuality::Minor7 }, 2, 1, -1 });
    prog.slots.push_back ({ { 5, cb::ChordQuality::Major7 }, 6, -1, 0 });
    prog.slots.push_back ({ { 2, cb::ChordQuality::Sus4 }, 3, 2, 2 });
    prog.slots.push_back ({ { 7, cb::ChordQuality::Dominant9 }, 16, 4, -2 });

    std::map<juce::String, float> saved;
    {
        ChordbloomProcessor p;
        setParam (p, id::keyRoot, 9);
        setParam (p, id::scale, 1);
        setParam (p, id::octave, -1);
        setParam (p, id::voicing, 2);
        setParam (p, id::voiceLeading, 0);
        setParam (p, id::pattern, 4);
        setParam (p, id::rate, 2);
        setParam (p, id::bassMode, 3);
        setParam (p, id::velocity, 0.5f);
        setParam (p, id::gate, 0.6f);
        setParam (p, id::swing, 0.3f);
        setParam (p, id::humanize, 0.2f);
        setParam (p, id::previewTone, 2);
        setParam (p, id::previewVolume, -20.0f);
        setParam (p, id::midiOut, 0);
        setParam (p, id::syncToHost, 0);
        setParam (p, id::scaleLock, 1);
        setParam (p, id::triggerSevenths, 1);
        p.setProgression (prog);
        p.setSavedEditorSize ({ 1200, 750 });

        for (auto* param : p.getParameters())
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (param))
                saved[rp->paramID] = rp->convertFrom0to1 (rp->getValue());

        p.getStateInformation (block);
    }

    check (block.getSize() > 0, "state: getStateInformation produces data");

    {
        const auto xml = juce::AudioProcessor::getXmlFromBinary (block.getData(), (int) block.getSize());
        check (xml != nullptr && xml->hasTagName ("ChordbloomState") && xml->getIntAttribute ("version", 0) >= 1
                   && xml->getChildByName ("Progression") != nullptr,
               "state: versioned XML with <Progression> child");
    }

    ChordbloomProcessor q;
    q.addChord (C_);   // something to be replaced, plus an undo entry that must be dropped
    q.setStateInformation (block.getData(), (int) block.getSize());

    bool paramsOk = true;
    std::string mismatch;
    for (auto* param : q.getParameters())
    {
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (param))
        {
            const float v = rp->convertFrom0to1 (rp->getValue());
            if (std::abs (v - saved[rp->paramID]) > 1.0e-3f)
            {
                paramsOk = false;
                mismatch += str (rp->paramID, " ", v, " != ", saved[rp->paramID], "; ");
            }
        }
    }
    check (paramsOk, "state: every parameter restored", mismatch);
    check (q.getProgression() == prog, "state: progression restored (root, quality, length, inversion, octave)");
    check (q.getSavedEditorSize() == juce::Point<int> (1200, 750), "state: editor size restored");
    check (q.getKey().tonic == 9 && q.getKey().scale == cb::ScaleType::NaturalMinor, "state: key restored");
    check (! q.canUndo() && ! q.canRedo(), "state: loading clears undo history");

    // Second round trip is stable
    juce::MemoryBlock block2;
    q.getStateInformation (block2);
    ChordbloomProcessor r;
    r.setStateInformation (block2.getData(), (int) block2.getSize());
    check (r.getProgression() == prog && r.getSavedEditorSize() == juce::Point<int> (1200, 750), "state: second round trip stable");

    // Robustness
    ChordbloomProcessor z;
    z.addChord (G_);
    z.setStateInformation (nullptr, 0);
    const char garbage[] = "this is not a chordbloom state \x01\x02\x03";
    z.setStateInformation (garbage, (int) sizeof (garbage));
    juce::MemoryBlock random;
    for (int i = 0; i < 300; ++i)
        random.append (&i, 1);
    z.setStateInformation (random.getData(), (int) random.getSize());
    check (z.getProgression().slots.size() == 1 && z.getProgression().slots[0].chord == G_, "state: garbage is ignored safely");

    juce::XmlElement bad ("ChordbloomState");
    bad.setAttribute ("version", 99);
    auto* params = bad.createNewChildElement ("Params");
    auto addParam = [params] (const char* pid, const juce::String& value)
    {
        auto* e = params->createNewChildElement ("PARAM");
        e->setAttribute ("id", pid);
        e->setAttribute ("value", value);
    };
    addParam (id::keyRoot, "57");
    addParam (id::velocity, "nan");
    addParam (id::octave, "-12");
    addParam (id::gate, "banana");
    auto* badProg = bad.createNewChildElement ("Progression");
    for (int i = 0; i < 25; ++i)
    {
        auto* s = badProg->createNewChildElement ("Slot");
        s->setAttribute ("root", -7 + i * 13);
        s->setAttribute ("quality", i % 2 == 0 ? 999 : -4);
        s->setAttribute ("length", i % 2 == 0 ? 1000 : -3);
        s->setAttribute ("inversion", i % 3 == 0 ? 77 : -9);
        s->setAttribute ("octave", i % 2 == 0 ? 9 : -9);
    }
    badProg->createNewChildElement ("Slot");   // no attributes at all
    auto* editor = bad.createNewChildElement ("Editor");
    editor->setAttribute ("width", 99999);
    editor->setAttribute ("height", 5);

    juce::MemoryBlock badBlock;
    juce::AudioProcessor::copyXmlToBinary (bad, badBlock);
    z.setStateInformation (badBlock.getData(), (int) badBlock.getSize());

    check (getParam (z, id::keyRoot) == 11.0f, "state: out-of-range keyRoot clamped to 11", str ("got ", getParam (z, id::keyRoot)));
    check (std::abs (getParam (z, id::velocity) - 0.75f) < 1e-4f, "state: NaN velocity falls back to default", str ("got ", getParam (z, id::velocity)));
    check (getParam (z, id::octave) == -3.0f, "state: octave clamped to -3", str ("got ", getParam (z, id::octave)));
    check (std::isfinite (getParam (z, id::gate)) && getParam (z, id::gate) >= 0.05f && getParam (z, id::gate) <= 1.0f,
           "state: non-numeric gate stays in range");
    check (std::abs (getParam (z, id::swing)) < 1e-6f, "state: missing parameters reset to defaults");

    bool slotsOk = (int) z.getProgression().slots.size() == cb::kMaxSlots;
    for (auto& s : z.getProgression().slots)
    {
        const int tones = s.chord.numTones();
        slotsOk = slotsOk && s.chord.root >= 0 && s.chord.root < 12 && (int) s.chord.quality >= 0
               && (int) s.chord.quality < cb::numChordQualities && s.lengthBeats >= 1 && s.lengthBeats <= 16
               && s.inversion >= -1 && s.inversion < tones && s.octaveShift >= -2 && s.octaveShift <= 2;
    }
    check (slotsOk, "state: invalid slots clamped and truncated to kMaxSlots");
    const auto ed = z.getSavedEditorSize();
    check (ed.x >= 960 && ed.x <= 1600 && ed.y >= 560 && ed.y <= 1000, "state: editor size clamped to the resizable range",
           str (ed.x, "x", ed.y));

    juce::MemoryBlock afterBad;
    z.getStateInformation (afterBad);
    check (afterBad.getSize() > 0, "state: still serialisable after bad input");
}

void testHostSyncedPlayback()
{
    ChordbloomProcessor p;
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
    p.setLocalPlayback (false);

    Rig rig (p);
    rig.head.playing = true;
    rig.head.bpm = 120.0;
    rig.head.ppq = 0.0;

    const auto expected = p.renderProgressionEvents();
    const auto voicings = p.getProgressionVoicings();
    check (! expected.empty(), "render: progression renders events", "renderProgression returned nothing (core stub?)");
    check (voicings.size() == 4, "render: one voicing per slot");

    const double spb = rig.samplesPerBeat();   // 24000
    const int loopBlocks = (int) std::ceil (33.0 * spb / rig.blockSize);

    int slotAt5 = -2;
    double beatAt5 = -1.0;
    for (int b = 0; b < loopBlocks; ++b)
    {
        const double ppqBefore = rig.head.ppq;
        rig.processBlock();
        if (slotAt5 == -2 && ppqBefore >= 5.0)
        {
            slotAt5 = p.getPlayingSlot();
            beatAt5 = p.getPlaybackBeat();
        }
    }

    check (p.isHostPlaying() && std::abs (p.getHostBpm() - 120.0) < 1e-9, "host: isHostPlaying / getHostBpm");
    check (slotAt5 == 1 && std::abs (beatAt5 - 5.0) < 0.05, "host: getPlayingSlot/getPlaybackBeat at beat 5",
           str ("slot ", slotAt5, " beat ", beatAt5));

    checkLoopAgainstRender (rig.all, expected, spb, 0.0, 0, "host loop 1");
    checkLoopAgainstRender (rig.all, expected, spb, 16.0, 0, "host loop 2 (wrap)");

    const auto loopSamples = (juce::int64) (16.0 * spb);
    check (countOns (rig.all, 0, loopSamples) == (int) expected.size(), "host: note-on count per loop == rendered events",
           str (countOns (rig.all, 0, loopSamples), " vs ", expected.size()));

    // Contract-level checks of what the defaults (Block + Root bass) should sound like
    if (voicings.size() == 4)
    {
        bool chordsOk = true, bassOk = true;
        std::string why;
        for (int i = 0; i < 4; ++i)
        {
            const auto start = (juce::int64) std::floor (i * 4.0 * spb);
            const auto chordNotes = onNotesAt (rig.all, ChordbloomProcessor::kChordChannel, start);
            const std::set<int> want (voicings[(size_t) i].begin(), voicings[(size_t) i].end());
            if (chordNotes != want)
            {
                chordsOk = false;
                why += str ("slot ", i, " got ", notesToString ({ chordNotes.begin(), chordNotes.end() }), " want ",
                            notesToString (voicings[(size_t) i]), "; ");
            }

            const auto bass = onNotesAt (rig.all, ChordbloomProcessor::kBassChannel, start);
            const int root = p.getProgression().slots[(size_t) i].chord.root;
            if (bass.size() != 1 || *bass.begin() % 12 != root || *bass.begin() < 28 || *bass.begin() > 55
                || (! want.empty() && *bass.begin() >= *want.begin()))
                bassOk = false;
        }
        check (chordsOk, "host: chord channel plays each slot's voicing at the slot start", why);
        check (bassOk, "host: bass channel plays the slot root (28..55, below the chord) at the slot start");
    }

    check (rig.doubleOns == 0, "host: no double note-ons in the output stream", str (rig.doubleOns));

    // Transport stop -> everything off
    rig.head.playing = false;
    rig.run (2);
    check (rig.openCount() == 0, "host stop: no hanging notes", str (rig.openCount(), " open"));
    check (p.getPlayingSlot() == -1 && ! p.isHostPlaying(), "host stop: playing slot -1");
    check (p.getSoundingNotes().none(), "host stop: getSoundingNotes empty");
    check (rig.audioFinite, "host: audio finite");
}

void testJumpsLoopsAndTempo()
{
    ChordbloomProcessor p;
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
    const auto voicings = p.getProgressionVoicings();

    Rig rig (p);
    rig.head.playing = true;
    const double spb = rig.samplesPerBeat();

    rig.run ((int) (6.0 * spb / rig.blockSize));   // into slot 1
    check (! rig.openNotes (1).empty(), "jump: notes sounding before the jump");

    // Host loops back to 0
    rig.head.ppq = 0.0;
    const auto jumpStart = rig.samplePos;
    const auto evs = rig.processBlock();
    bool offsBeforeOns = true;
    bool sawOn = false;
    for (auto& e : evs)
    {
        if (e.on)
            sawOn = true;
        else if (sawOn && e.sample == jumpStart)
            offsBeforeOns = false;
    }
    const auto slot0 = onNotesAt (evs, 1, jumpStart, 0);
    check (offsBeforeOns, "jump back: note-offs precede the new note-ons");
    check (! voicings.empty() && slot0 == std::set<int> (voicings[0].begin(), voicings[0].end()),
           "jump back: slot 0 chord starts at the jump sample");
    check (rig.doubleOns == 0, "jump back: no double note-ons");

    // Jump forward into the middle of slot 2 -> notes in progress are chased
    rig.head.ppq = 9.0;
    rig.processBlock();
    const auto open = rig.openNotes (1);
    check (voicings.size() > 2 && open == std::set<int> (voicings[2].begin(), voicings[2].end()),
           "jump forward: slot 2 chord chased mid-slot", notesToString ({ open.begin(), open.end() }));
    check (p.getPlayingSlot() == 2, "jump forward: playing slot 2");

    // Tempo change 120 -> 90 at beat ~10: the downbeat of the next loop lands where ppq crosses 16
    rig.run (8);
    rig.head.bpm = 90.0;
    const double ppqAtChange = rig.head.ppq;
    const auto sampleAtChange = rig.samplePos;
    const double spb90 = rig.samplesPerBeat();
    const int blocks = (int) std::ceil ((16.5 - ppqAtChange) * spb90 / rig.blockSize);
    const auto evs90 = rig.run (blocks);
    const auto expectedDownbeat = sampleAtChange + (juce::int64) std::floor ((16.0 - ppqAtChange) * spb90);
    const auto down = onNotesAt (evs90, 1, expectedDownbeat, 2);
    check (! voicings.empty() && down == std::set<int> (voicings[0].begin(), voicings[0].end()),
           "tempo change: loop wraps on the host's beat 16", str ("expected @", expectedDownbeat));
    check (rig.doubleOns == 0, "tempo change: no double note-ons");

    // Host stops while notes sound
    rig.head.playing = false;
    rig.run (1);
    check (rig.openCount() == 0, "jump/tempo: stop leaves no hanging notes", str (rig.openCount(), " open"));

    // syncToHost off while host plays -> nothing from playback, and turning it off mid-play closes notes
    rig.head.playing = true;
    rig.head.ppq = 0.0;
    rig.run (4);
    check (rig.openCount() > 0, "sync: playing with syncToHost on");
    setParam (p, cb::ParamIDs::syncToHost, 0.0f);
    rig.run (2);
    check (rig.openCount() == 0 && p.getPlayingSlot() == -1, "sync off: notes released, no playback");
    const auto silent = rig.run ((int) (5.0 * spb / rig.blockSize));
    check (countOns (silent) == 0, "sync off: host playing produces no playback notes");
    setParam (p, cb::ParamIDs::syncToHost, 1.0f);
    rig.head.playing = false;
    rig.run (1);
}

void testLocalPlayback()
{
    ChordbloomProcessor p;
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
    const auto expected = p.renderProgressionEvents();

    {
        Rig rig (p, 48000.0, 512, false);   // no play head at all (standalone without transport)
        p.setLocalPlayback (true);
        check (p.isLocalPlaybackOn(), "local: isLocalPlaybackOn");
        const double spb = 48000.0 * 60.0 / 120.0;
        rig.run ((int) std::ceil (17.0 * spb / rig.blockSize));
        checkLoopAgainstRender (rig.all, expected, spb, 0.0, 0, "local 120 bpm (no play head)");
        check (p.getHostBpm() == 120.0, "local: tempo defaults to 120 without host");
        check (p.getPlayingSlot() == 0, "local: playing slot after wrap", str (p.getPlayingSlot()));

        p.setLocalPlayback (false);
        rig.run (1);
        check (rig.openCount() == 0 && p.getPlayingSlot() == -1, "local stop: no hanging notes");
        check (rig.doubleOns == 0, "local: no double note-ons");
    }

    {
        Rig rig (p);
        rig.head.playing = false;
        rig.head.bpm = 100.0;
        p.setLocalPlayback (true);
        const double spb = rig.samplesPerBeat();
        rig.run ((int) std::ceil (5.0 * spb / rig.blockSize));
        check (std::abs (p.getHostBpm() - 100.0) < 1e-9, "local: follows host tempo while host stopped");
        checkLoopAgainstRender (rig.all, { expected.begin(), std::find_if (expected.begin(), expected.end(),
                                                                          [] (const cb::NoteEvent& e) { return e.startBeat >= 4.0; }) },
                                spb, 0.0, 0, "local 100 bpm, slot 0");
        check (onNotesAt (rig.all, 1, (juce::int64) std::floor (4.0 * spb)).size() >= 3, "local 100 bpm: slot 1 at 4 beats");

        // Host starts -> local yields to host sync; host stops -> Chordbloom's own Play is switched off as well (B5,
        // behaviour changed in round 2: it used to restart its own loop, which kept playing after FL stopped).
        rig.head.playing = true;
        rig.head.ppq = 8.0;
        rig.run (2);
        check (p.getPlayingSlot() == 2, "local->host: host transport takes over", str (p.getPlayingSlot()));
        rig.head.playing = false;
        auto evs = rig.run (4);
        check (! p.isLocalPlaybackOn() && p.getPlayingSlot() == -1 && countOns (evs) == 0 && rig.openCount() == 0,
               "host stop: Chordbloom's own loop stops too (Play switched off, nothing keeps playing)",
               str ("local ", p.isLocalPlaybackOn(), " slot ", p.getPlayingSlot(), " ons ", countOns (evs)));

        // Play pressed while the host plays (follows the host), then the host stops -> silence as well
        rig.head.playing = true;
        rig.head.ppq = 0.0;
        rig.run (2);
        p.setLocalPlayback (true);
        rig.run (2);
        rig.head.playing = false;
        evs = rig.run (3);
        check (! p.isLocalPlaybackOn() && p.getPlayingSlot() == -1 && countOns (evs) == 0 && rig.openCount() == 0,
               "Play pressed while the host plays: host stop silences it");

        // Local playback while the host is stopped still works
        p.setLocalPlayback (true);
        evs = rig.run (3);
        check (p.isLocalPlaybackOn() && countOns (evs, 0, std::numeric_limits<juce::int64>::max(), 1) >= 3,
               "local playback still starts while the host is stopped");
        p.setLocalPlayback (false);
        rig.run (1);
        check (rig.openCount() == 0, "local: no hanging notes after mode changes");
    }
}

void testEditWhilePlaying()
{
    ChordbloomProcessor p;
    p.setProgression (makeProgression ({ C_, F_ }));
    Rig rig (p);
    p.setLocalPlayback (true);
    const double spb = rig.samplesPerBeat();
    rig.run ((int) (1.0 * spb / rig.blockSize));
    const auto before = rig.openNotes (1);

    auto slot = p.getProgression().slots[0];
    slot.chord = Am_;
    p.updateSlot (0, slot);
    rig.processBlock();
    const auto after = rig.openNotes (1);
    const auto vAm = p.getProgressionVoicings()[0];
    check (after == std::set<int> (vAm.begin(), vAm.end()), "loop edit: new chord replaces the sounding one immediately",
           notesToString ({ before.begin(), before.end() }) + " -> " + notesToString ({ after.begin(), after.end() }));
    check (rig.doubleOns == 0, "loop edit: stream balanced");

    // A render-parameter tweak (velocity) must not retrigger the held chord
    setParam (p, cb::ParamIDs::velocity, 0.6f);
    pumpMessages (80);
    const auto evs = rig.run (3);
    check (countOns (evs) == 0, "param tweak: held chord is not retriggered", str (countOns (evs), " note-ons"));
    check (p.renderProgressionEvents().empty() || p.renderProgressionEvents()[0].velocity < 0.7f, "param tweak: render uses new velocity");

    // Clearing while playing silences everything
    p.clearProgression();
    rig.run (2);
    check (rig.openCount() == 0 && p.getPlayingSlot() == -1, "loop edit: clearing stops all notes");
    p.undo();
    rig.run ((int) (0.5 * spb / rig.blockSize));
    check (rig.openCount() > 0, "loop edit: undo brings playback back");
    p.setLocalPlayback (false);
    rig.run (1);
    check (rig.openCount() == 0, "loop edit: no hanging notes");
}

void testParameterRerender()
{
    ChordbloomProcessor p;
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
    ChangeCounter counter;
    p.addChangeListener (&counter);
    pumpMessages (20);
    counter.count = 0;

    setParam (p, cb::ParamIDs::pattern, (float) cb::RhythmPattern::ArpUp);
    setParam (p, cb::ParamIDs::bassMode, (float) cb::BassMode::Off);
    pumpMessages (100);

    const auto expected = p.renderProgressionEvents();
    Rig rig (p);
    p.setLocalPlayback (true);
    const double spb = rig.samplesPerBeat();
    rig.run ((int) std::ceil (16.5 * spb / rig.blockSize));
    checkLoopAgainstRender (rig.all, expected, spb, 0.0, 0, "re-render after pattern change (Arp Up)");
    check (countOns (rig.all, 0, (juce::int64) (16 * spb), 2) == 0, "re-render: bass off -> no channel 2 notes");
    p.setLocalPlayback (false);
    rig.run (1);

    setParam (p, cb::ParamIDs::keyRoot, 2.0f);
    pumpMessages (100);
    check (counter.count > 0, "key change sends a change message");
    const auto& pal = p.getPalette();
    check (! pal.empty() && ! pal[0].chords.empty() && pal[0].chords[0].chord.root == 2, "key change rebuilds the palette (D)");
    p.removeChangeListener (&counter);
}

//==============================================================================
void testPreview()
{
    ChordbloomProcessor p;
    Rig rig (p);
    const auto vs = p.getVoicingSettings();

    p.startPreviewChord (C_);
    auto evs = rig.processBlock();
    const auto expectedC = cb::voiceChord (C_, vs, -1, 0, {});
    const auto gotC = onNotesAt (evs, 1, rig.samplePos - rig.blockSize, 0);
    check (! expectedC.empty() && gotC == std::set<int> (expectedC.begin(), expectedC.end()), "preview: chord notes on channel 1 at sample 0",
           notesToString ({ gotC.begin(), gotC.end() }) + " vs " + notesToString (expectedC));
    check (p.getPreviewChord().has_value() && *p.getPreviewChord() == C_, "preview: getPreviewChord");
    {
        const auto sounding = p.getSoundingNotes();
        bool all = true;
        for (int n : expectedC)
            all = all && sounding.test ((size_t) n);
        check (all && (int) sounding.count() == (int) expectedC.size(), "preview: getSoundingNotes shows the chord");
    }

    p.startPreviewChord (F_);
    evs = rig.processBlock();
    const auto expectedF = cb::voiceChord (F_, vs, -1, 0, expectedC);
    const auto openNow = rig.openNotes (1);
    check (openNow == std::set<int> (expectedF.begin(), expectedF.end()), "preview: next chord voice-led from the previous preview",
           notesToString ({ openNow.begin(), openNow.end() }) + " vs " + notesToString (expectedF));
    check (rig.doubleOns == 0, "preview: stream balanced when switching chords");

    p.stopPreview();
    rig.processBlock();
    check (rig.openCount() == 0 && ! p.getPreviewChord().has_value() && p.getSoundingNotes().none(), "preview: stopPreview releases everything");

    p.startPreviewNote (64);
    evs = rig.processBlock();
    check (onNotesAt (evs, 1, rig.samplePos - rig.blockSize, 0) == std::set<int> { 64 } && ! p.getPreviewChord().has_value(),
           "preview: single note");
    p.stopPreview();
    evs = rig.processBlock();
    check (findEvent (evs, false, 1, 64, rig.samplePos - rig.blockSize, 0) != nullptr, "preview: single note off");

    p.setProgression (makeProgression ({ C_, Am_, F_ }));
    p.startPreviewSlot (1);
    evs = rig.processBlock();
    const auto v1 = p.getProgressionVoicings()[1];
    check (rig.openNotes (1) == std::set<int> (v1.begin(), v1.end()) && p.getPreviewChord() == std::optional<cb::Chord> (Am_),
           "preview: slot preview uses the slot voicing");
    p.startPreviewSlot (99);   // invalid -> just stops
    rig.processBlock();
    check (rig.openCount() == 0, "preview: invalid slot preview stops the old one");

    // Many previews without the audio thread running must not leave anything stuck once it runs again
    for (int i = 0; i < 600; ++i)
    {
        p.startPreviewChord ({ i % 12, cb::ChordQuality::Major7 });
        if (i % 3 == 0)
            p.stopPreview();
    }
    p.stopPreview();
    rig.run (2);
    check (rig.openCount() == 0 && p.getSoundingNotes().none(), "preview: FIFO overflow never leaves notes on", str (rig.openCount()));
}

//==============================================================================
void testScaleLock()
{
    ChordbloomProcessor p;
    Rig rig (p);
    setParam (p, cb::ParamIDs::scaleLock, 1.0f);
    pumpMessages (10);

    auto evs = rig.processBlock (noteOn (61, 10));
    auto base = rig.samplePos - rig.blockSize;
    check (findEvent (evs, true, 1, 60, base + 10, 0) != nullptr && countOns (evs) == 1, "scaleLock: C#5 -> C5 (tie snaps down), sample-accurate");
    evs = rig.processBlock (noteOff (61, 20));
    base = rig.samplePos - rig.blockSize;
    check (findEvent (evs, false, 1, 60, base + 20, 0) != nullptr, "scaleLock: note-off follows the mapping");

    evs = rig.processBlock (noteOn (66));
    check (findEvent (evs, true, 1, 65, rig.samplePos - rig.blockSize, 0) != nullptr, "scaleLock: F# -> F (tie down)");
    rig.processBlock (noteOff (66));
    evs = rig.processBlock (noteOn (64));
    check (findEvent (evs, true, 1, 64, rig.samplePos - rig.blockSize, 0) != nullptr, "scaleLock: scale tones pass unchanged");
    rig.processBlock (noteOff (64));

    // Mapping survives a mode change while the key is held
    rig.processBlock (noteOn (68));   // G# -> G
    setParam (p, cb::ParamIDs::scaleLock, 0.0f);
    evs = rig.processBlock (noteOff (68));
    check (findEvent (evs, false, 1, 67, rig.samplePos - rig.blockSize, 0) != nullptr && rig.openCount() == 0,
           "scaleLock: note-off matches even after scaleLock was switched off");

    // Different key: D major, F -> E (tie down between E and F#)
    setParam (p, cb::ParamIDs::scaleLock, 1.0f);
    setParam (p, cb::ParamIDs::keyRoot, 2.0f);
    pumpMessages (100);
    evs = rig.processBlock (noteOn (65));
    check (findEvent (evs, true, 1, 64, rig.samplePos - rig.blockSize, 0) != nullptr, "scaleLock: follows key change (D major: F -> E)");
    rig.processBlock (noteOff (65));

    // Non-note messages pass through
    juce::MidiBuffer cc;
    cc.addEvent (juce::MidiMessage::controllerEvent (1, 1, 64), 5);
    cc.addEvent (juce::MidiMessage::pitchWheel (1, 9000), 6);
    const int othersBefore = rig.numOtherEvents;
    rig.processBlock (cc);
    check (rig.numOtherEvents - othersBefore == 2, "live input: non-note MIDI passes through");

    setParam (p, cb::ParamIDs::scaleLock, 0.0f);
    evs = rig.processBlock (noteOn (61));
    check (findEvent (evs, true, 1, 61, rig.samplePos - rig.blockSize, 0) != nullptr, "live input: plain pass-through when no mode is on");
    rig.processBlock (noteOff (61));
    check (rig.openCount() == 0, "live input: balanced");
}

void testChordTrigger()
{
    ChordbloomProcessor p;
    Rig rig (p);
    setParam (p, cb::ParamIDs::chordTrigger, 1.0f);
    pumpMessages (10);
    const auto key = p.getKey();

    auto evs = rig.processBlock (noteOn (62, 7));   // D5 -> ii
    std::set<int> d5;
    for (auto& e : evs)
        if (e.on)
            d5.insert (e.note);
    const auto ii = cb::diatonicChord (key, 1, false);
    check (pitchClassesOf (d5) == pitchClassesOf (ii) && (int) d5.size() == ii.numTones(), "chordTrigger: D plays degree ii triad",
           notesToString ({ d5.begin(), d5.end() }));
    check (! evs.empty() && evs.front().sample == rig.samplePos - rig.blockSize + 7, "chordTrigger: sample-accurate");
    double mean = 0.0;
    for (int n : d5)
        mean += n;
    mean /= juce::jmax ((size_t) 1, d5.size());
    check (std::abs (mean - 60.0) <= 9.0, "chordTrigger: C5 octave plays around the voicing centre", str ("mean ", mean));
    check (p.getTriggeredDegree() == 1, "chordTrigger: getTriggeredDegree while held", str (p.getTriggeredDegree()));
    {
        const auto shown = p.getTriggeredNotes();   // B26: the keyboard guide shows exactly what is played
        check (std::set<int> (shown.begin(), shown.end()) == d5, "chordTrigger: getTriggeredNotes == the notes sent",
               notesToString (shown));
    }

    evs = rig.processBlock (noteOff (62));
    check (rig.openCount() == 0, "chordTrigger: release sends note-offs for the whole chord");
    check (p.getTriggeredDegree() == -1, "chordTrigger: degree -1 after release");
    check (p.getTriggeredNotes().empty(), "chordTrigger: no triggered notes after release");

    evs = rig.processBlock (noteOn (50));   // D4
    std::set<int> d4;
    for (auto& e : evs)
        if (e.on)
            d4.insert (e.note);
    std::set<int> d5down;
    for (int n : d5)
        d5down.insert (n - 12);
    check (d4 == d5down, "chordTrigger: one octave lower input -> chord one octave lower");
    {
        const auto shown = p.getTriggeredNotes();
        check (std::set<int> (shown.begin(), shown.end()) == d4, "chordTrigger: getTriggeredNotes follows the input octave",
               notesToString (shown));
    }
    rig.processBlock (noteOff (50));

    evs = rig.processBlock (noteOn (61));   // black key
    check (countOns (evs) == 1 && findEvent (evs, true, 1, 61, rig.samplePos - rig.blockSize, 0) != nullptr,
           "chordTrigger: black keys pass through");
    rig.processBlock (noteOff (61));

    setParam (p, cb::ParamIDs::triggerSevenths, 1.0f);
    evs = rig.processBlock (noteOn (67));   // G5 -> V7
    std::set<int> g7;
    for (auto& e : evs)
        if (e.on)
            g7.insert (e.note);
    const auto v7 = cb::diatonicChord (key, 4, true);
    check (pitchClassesOf (g7) == pitchClassesOf (v7) && g7.size() == 4, "chordTrigger: sevenths (G -> V7)", notesToString ({ g7.begin(), g7.end() }));
    check (p.getTriggeredDegree() == 4, "chordTrigger: degree 4 for G");

    // Switch the mode off while held: note-off must still release the chord
    setParam (p, cb::ParamIDs::chordTrigger, 0.0f);
    rig.processBlock (noteOff (67));
    check (rig.openCount() == 0 && p.getTriggeredDegree() == -1, "chordTrigger: release after mode switch still matches");

    // Two keys held, overlapping chords share notes: ref counting keeps the shared note until both are released
    setParam (p, cb::ParamIDs::chordTrigger, 1.0f);
    setParam (p, cb::ParamIDs::triggerSevenths, 0.0f);
    rig.processBlock (noteOn (60));   // I
    rig.processBlock (noteOn (64));   // iii (shares two notes with I in C major)
    rig.processBlock (noteOff (60));
    const auto iii = cb::diatonicChord (key, 2, false);
    check (pitchClassesOf (rig.openNotes (1)) == pitchClassesOf (iii), "chordTrigger: overlapping chords keep shared notes");
    rig.processBlock (noteOff (64));
    check (rig.openCount() == 0 && p.getSoundingNotes().none(), "chordTrigger: all released");
}

//==============================================================================
void testMidiOutAndAudio()
{
    ChordbloomProcessor p;
    p.setProgression (makeProgression ({ C_, Am_ }));
    Rig rig (p);

    // Audio: preview through the built-in synth
    p.startPreviewChord (C_);
    rig.run (200);
    check (rig.audioFinite, "audio: output finite (no NaN/inf)");
    check (rig.peak <= 2.0f, "audio: output bounded", str ("peak ", rig.peak));
    check (rig.peak > 1.0e-4f && rig.peak != 0.5f, "audio: preview synth is audible", str ("peak ", rig.peak));
    p.stopPreview();
    rig.run (400);   // ~4 s of release tail
    check (rig.blockPeak() < 1.0e-3f, "audio: silent after preview released (buffer cleared every block)", str ("peak ", rig.blockPeak()));

    setParam (p, cb::ParamIDs::previewOn, 0.0f);
    p.startPreviewChord (F_);
    rig.run (50);
    check (rig.blockPeak() < 1.0e-6f, "audio: previewOn off -> silent", str ("peak ", rig.blockPeak()));
    check (! rig.openNotes (1).empty(), "audio: previewOn off still sends MIDI");
    p.stopPreview();
    rig.run (1);
    setParam (p, cb::ParamIDs::previewOn, 1.0f);

    // midiOut off -> empty MIDI output, even with preview + playback + live input
    setParam (p, cb::ParamIDs::midiOut, 0.0f);
    p.startPreviewChord (G_);
    p.setLocalPlayback (true);
    int outCount = 0;
    for (int i = 0; i < 100; ++i)
    {
        const auto evs = rig.processBlock (i == 5 ? noteOn (70) : (i == 9 ? noteOff (70) : juce::MidiBuffer()));
        outCount += (int) evs.size();
    }
    check (outCount == 0 && rig.numOtherEvents == 0, "midiOut off: MIDI output empty", str (outCount, " events"));
    check (p.getSoundingNotes().any(), "midiOut off: notes still sound internally");
    check (rig.audioFinite, "midiOut off: audio finite");
    p.stopPreview();
    p.setLocalPlayback (false);
    rig.run (1);

    // midiOut switched off while notes are on: the host receives note-offs for them
    setParam (p, cb::ParamIDs::midiOut, 1.0f);
    p.startPreviewChord (C_);
    rig.run (2);
    const int openBefore = rig.openCount();
    setParam (p, cb::ParamIDs::midiOut, 0.0f);
    rig.run (1);
    check (openBefore > 0 && rig.openCount() == 0, "midiOut off mid-note: host notes are closed");
    p.stopPreview();
    rig.run (1);
    setParam (p, cb::ParamIDs::midiOut, 1.0f);
    rig.run (1);
    check (rig.openCount() == 0 && p.getSoundingNotes().none(), "midiOut: nothing left on");
}

void testReleaseResources()
{
    ChordbloomProcessor p;
    Rig rig (p);
    p.startPreviewChord (C_);
    rig.processBlock (noteOn (40, 0, 3));
    check (rig.openCount() > 3, "releaseResources: notes on before");

    p.releaseResources();
    p.prepareToPlay (48000.0, 512);
    rig.processBlock();
    check (rig.openCount() == 0 && p.getSoundingNotes().none(), "releaseResources/prepareToPlay: first block closes every note",
           str (rig.openCount(), " open"));
    p.stopPreview();
    rig.processBlock (noteOff (40, 0, 3));   // late note-off for a closed note: harmless
    check (rig.doubleOns == 0, "releaseResources: stream balanced");
}

//==============================================================================
// Host bypass: JUCE calls processBlockBypassed instead of processBlock. Everything Chordbloom holds must be closed,
// input MIDI passes through, nothing new is generated, and playback picks up again when the bypass ends.
void testBypass()
{
    ChordbloomProcessor p;
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
    Rig rig (p);
    rig.head.playing = true;
    rig.run (10);
    p.startPreviewNote (96);
    rig.processBlock (noteOn (40, 0, 3));
    rig.run (2);
    check (rig.openNotes (1).size() >= 3 && rig.openNotes (2).size() == 1 && rig.openNotes (3).size() == 1,
           "bypass: playback, preview and live notes on before", str (rig.openCount(), " open"));

    rig.bypassed = true;
    rig.processBlock (noteOn (72, 10, 4));
    check (rig.openCount() == 1 && rig.openNotes (4) == std::set<int> { 72 },
           "bypass: every held note closed, input passes through", str (rig.openCount(), " open"));
    check (p.getSoundingNotes().none() && p.getPlayingSlot() == -1, "bypass: nothing reported as sounding / playing");

    p.stopPreview();
    p.startPreviewChord (F_);
    const auto during = rig.run (300);   // ~3 s
    check (countOns (during) == 0, "bypass: no notes generated while bypassed", str (countOns (during), " note-ons"));
    check (rig.blockPeak() < 1.0e-3f, "bypass: preview sound fades out", str ("peak ", rig.blockPeak()));
    rig.processBlock (noteOff (72, 0, 4));
    p.stopPreview();

    rig.bypassed = false;
    const auto after = rig.run (8);
    check (countOns (after, 0, std::numeric_limits<juce::int64>::max(), 1) >= 3, "bypass: playback resumes after bypass");
    rig.head.playing = false;
    rig.processBlock (noteOff (40, 0, 3));   // key released after the bypass already closed it: harmless
    rig.run (2);
    check (rig.openCount() == 0 && rig.doubleOns == 0, "bypass: stream balanced afterwards",
           str (rig.openCount(), " open, ", rig.doubleOns, " double note-ons"));
}

//==============================================================================
// The preview synth on its own: every tone must be audible, bounded, click-free when notes are released or cut
// (allNotesOff without tail) and silent again after the release; 40 overlapping notes exercise voice stealing.
void testPreviewSynth()
{
    const char* toneNames[] = { "Soft Keys", "Warm Pad", "Glass Pluck" };
    constexpr double sr = 48000.0;
    constexpr int block = 480;

    for (int t = 0; t < (int) PreviewSynth::Tone::NumTones; ++t)
    {
        PreviewSynth synth;
        synth.prepare (sr, block);
        synth.setTone ((PreviewSynth::Tone) t);
        synth.setGainDecibels (-9.0f);

        std::vector<float> out;
        juce::AudioBuffer<float> buf (2, block);
        bool finite = true;

        auto renderBlocks = [&] (int count, const juce::MidiBuffer& first)
        {
            for (int i = 0; i < count; ++i)
            {
                buf.clear();
                synth.render (buf, i == 0 ? first : juce::MidiBuffer(), block);
                for (int n = 0; n < block; ++n)
                {
                    const float l = buf.getSample (0, n), r = buf.getSample (1, n);
                    finite = finite && std::isfinite (l) && std::isfinite (r);
                    out.push_back (std::abs (l) > std::abs (r) ? l : r);
                }
            }
        };

        auto chord = [] (bool on, std::initializer_list<int> notes)
        {
            juce::MidiBuffer m;
            for (int n : notes)
                m.addEvent (on ? juce::MidiMessage::noteOn (1, n, (juce::uint8) 100) : juce::MidiMessage::noteOff (1, n), 37);
            return m;
        };

        // Largest sample-to-sample step after an event, relative to the steady signal just before it.
        auto stepRatio = [&] (size_t eventIndex)
        {
            float before = 1.0e-9f, after = 0.0f;
            for (size_t i = eventIndex - 2400; i < eventIndex; ++i)
                before = std::max (before, std::abs (out[i] - out[i - 1]));
            for (size_t i = eventIndex; i < eventIndex + 2400; ++i)
                after = std::max (after, std::abs (out[i] - out[i - 1]));
            return after / before;
        };

        auto peakOf = [&] (size_t from, size_t to)
        {
            float m = 0.0f;
            for (size_t i = from; i < to && i < out.size(); ++i)
                m = std::max (m, std::abs (out[i]));
            return m;
        };

        renderBlocks (100, chord (true, { 48, 60, 64, 67 }));            // 1 s chord
        const size_t releaseAt = out.size() + 37;
        renderBlocks (300, chord (false, { 48, 60, 64, 67 }));           // 3 s release
        const float releaseTail = peakOf (out.size() - 4800, out.size());

        renderBlocks (100, chord (true, { 50, 62, 65, 69 }));
        const size_t cutAt = out.size();
        synth.allNotesOff (false);                                        // hard stop -> 3 ms fade, no click
        renderBlocks (300, {});

        juce::MidiBuffer many, manyOff;
        for (int i = 0; i < 40; ++i)
        {
            many.addEvent (juce::MidiMessage::noteOn (1 + i % 2, 40 + i, (juce::uint8) 110), i * 11);
            manyOff.addEvent (juce::MidiMessage::noteOff (1 + i % 2, 40 + i), 0);
        }
        renderBlocks (100, many);
        renderBlocks (400, manyOff);

        const float peak = peakOf (0, out.size());
        const float end = peakOf (out.size() - 4800, out.size());
        const std::string name = std::string ("synth ") + toneNames[t] + ": ";
        check (finite && peak > 0.01f && peak < 1.0f, name + "audible, finite and bounded", str ("peak ", peak));
        check (stepRatio (releaseAt) < 1.5f, name + "note-off is click-free", str ("step ratio ", stepRatio (releaseAt)));
        check (stepRatio (cutAt) < 1.5f, name + "allNotesOff (no tail) is click-free", str ("step ratio ", stepRatio (cutAt)));
        check (releaseTail < 2.0e-3f && end < 2.0e-3f, name + "silent after release", str (releaseTail, " / ", end));
    }
}

//==============================================================================
void testMidiExport()
{
    // buildMidiFile, independent of the core renderer
    std::vector<cb::NoteEvent> events;
    events.push_back ({ 60, 0.8f, 0.0, 1.0, 1 });
    events.push_back ({ 64, 0.8f, 1.5, 0.5, 1 });
    events.push_back ({ 36, 0.6f, 0.0, 4.0, 2 });

    auto parse = [] (const juce::MidiFile& mf, juce::MidiFile& out)
    {
        juce::MemoryOutputStream os;
        if (! mf.writeTo (os, 1))
            return false;
        juce::MemoryInputStream is (os.getData(), os.getDataSize(), false);
        int type = -1;
        return out.readFrom (is, true, &type) && type == 1;
    };

    auto countNoteOns = [] (const juce::MidiFile& mf, int channel = 0)
    {
        int n = 0;
        for (int t = 0; t < mf.getNumTracks(); ++t)
            for (auto* h : *mf.getTrack (t))
                if (h->message.isNoteOn() && (channel == 0 || h->message.getChannel() == channel))
                    ++n;
        return n;
    };

    auto tempoOf = [] (const juce::MidiFile& mf)
    {
        if (mf.getNumTracks() < 1)
            return -1.0;
        for (auto* h : *mf.getTrack (0))
            if (h->message.isTempoMetaEvent())
                return 60.0 / h->message.getTempoSecondsPerQuarterNote();
        return -1.0;
    };

    auto trackName = [] (const juce::MidiFile& mf, int track)
    {
        if (track >= mf.getNumTracks())
            return juce::String();
        for (auto* h : *mf.getTrack (track))
            if (h->message.isTrackNameEvent())
                return h->message.getTextFromTextMetaEvent();
        return juce::String();
    };

    auto onlyChannel = [] (const juce::MidiFile& mf, int track, int channel)
    {
        if (track >= mf.getNumTracks())
            return false;
        for (auto* h : *mf.getTrack (track))
            if (h->message.isNoteOnOrOff() && h->message.getChannel() != channel)
                return false;
        return true;
    };

    // Both parts: one named track per part (round 2: used to be one track with two channels)
    juce::MidiFile parsed;
    const bool ok = parse (cb::midiexport::buildMidiFile (events, 90.0, true, true), parsed);
    check (ok && parsed.getNumTracks() == 3 && parsed.getTimeFormat() == 960, "export: type-1, tempo + chords + bass tracks, 960 ppq",
           str (parsed.getNumTracks(), " tracks"));
    check (trackName (parsed, 1) == "Chordbloom Chords" && trackName (parsed, 2) == "Chordbloom Bass"
               && onlyChannel (parsed, 1, 1) && onlyChannel (parsed, 2, 2),
           "export: Both = two named tracks (chords ch1, bass ch2)",
           (trackName (parsed, 1) + " / " + trackName (parsed, 2)).toStdString());
    check (std::abs (tempoOf (parsed) - 90.0) < 0.01, "export: tempo track (90 bpm)", str (tempoOf (parsed)));
    check (countNoteOns (parsed) == 3 && countNoteOns (parsed, 1) == 2 && countNoteOns (parsed, 2) == 1, "export: both parts, channels kept");
    {
        juce::MidiFile noBass;
        std::vector<cb::NoteEvent> chordsOnlyEvents (events.begin(), events.begin() + 2);
        parse (cb::midiexport::buildMidiFile (chordsOnlyEvents, 120.0, true, true), noBass);
        check (noBass.getNumTracks() == 2 && trackName (noBass, 1) == "Chordbloom Chords", "export: Both without bass notes has no empty bass track");
    }

    bool tickOk = false;
    if (parsed.getNumTracks() == 3)
        for (auto* h : *parsed.getTrack (1))
            if (h->message.isNoteOn() && h->message.getNoteNumber() == 64)
                tickOk = std::abs (h->message.getTimeStamp() - 1440.0) < 0.5 && h->noteOffObject != nullptr
                      && std::abs (h->noteOffObject->message.getTimeStamp() - 1920.0) < 0.5;
    check (tickOk, "export: note at beat 1.5 -> tick 1440, off at 1920");

    juce::MidiFile chordsOnly, bassOnly;
    parse (cb::midiexport::buildMidiFile (events, 120.0, true, false), chordsOnly);
    parse (cb::midiexport::buildMidiFile (events, 120.0, false, true), bassOnly);
    check (countNoteOns (chordsOnly) == 2 && countNoteOns (bassOnly) == 1, "export: part filtering by channel");

    // Through the processor
    ChordbloomProcessor p;
    check (p.writeMidiToTempFile (ChordbloomProcessor::ExportPart::Chords) == juce::File(), "export: empty progression -> File()");
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
    const auto rendered = p.renderProgressionEvents();
    int nChords = 0, nBass = 0;
    for (auto& e : rendered)
        (e.channel == 2 ? nBass : nChords)++;

    const auto file = p.writeMidiToTempFile (ChordbloomProcessor::ExportPart::Chords);
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Chordbloom");
    check (file.existsAsFile(), "export: temp file written", file.getFullPathName().toStdString());
    check (file.getFileName() == "C Am F G - C major - Chords.mid" && file.getParentDirectory() == dir
               && file.getFileName() == p.getExportFileName (ChordbloomProcessor::ExportPart::Chords),
           "export: temp file named after the progression, in <temp>/Chordbloom", file.getFullPathName().toStdString());

    auto readFile = [] (const juce::File& f, juce::MidiFile& out)
    {
        juce::FileInputStream in (f);
        return in.openedOk() && out.readFrom (in);
    };

    juce::MidiFile fromDisk;
    check (readFile (file, fromDisk) && countNoteOns (fromDisk) == nChords && nChords > 0, "export: chords file parses back with every chord note",
           str (countNoteOns (fromDisk), " vs ", nChords));
    check (std::abs (tempoOf (fromDisk) - 120.0) < 0.01, "export: tempo = host tempo (120 when unknown)");
    double eot = -1.0;
    if (fromDisk.getNumTracks() == 2)
        for (auto* h : *fromDisk.getTrack (1))
            if (h->message.isEndOfTrackMetaEvent())
                eot = h->message.getTimeStamp();
    check (std::abs (eot - 16.0 * 960.0) < 0.5, "export: clip is exactly one loop long", str ("end tick ", eot));

    const auto bassFile = p.writeMidiToTempFile (ChordbloomProcessor::ExportPart::Bass);
    juce::MidiFile bassParsed;
    check (bassFile.getFileName() == "C Am F G - C major - Bass.mid" && readFile (bassFile, bassParsed)
               && countNoteOns (bassParsed) == nBass && countNoteOns (bassParsed, 1) == 0,
           "export: bass file", str (countNoteOns (bassParsed), " vs ", nBass));

    const auto bothFile = p.writeMidiToTempFile (ChordbloomProcessor::ExportPart::Both);
    juce::MidiFile bothParsed;
    check (readFile (bothFile, bothParsed) && countNoteOns (bothParsed) == (int) rendered.size() && bothParsed.getNumTracks() == 3
               && bothFile.getFileName() == "C Am F G - C major - Chords + Bass.mid",
           "export: both file (two named tracks)", bothFile.getFileName().toStdString());
    {
        bool loopLong = bothParsed.getNumTracks() == 3;
        for (int t = 1; t < bothParsed.getNumTracks(); ++t)
            for (auto* h : *bothParsed.getTrack (t))
                if (h->message.isEndOfTrackMetaEvent())
                    loopLong = loopLong && std::abs (h->message.getTimeStamp() - 16.0 * 960.0) < 0.5;
        check (loopLong, "export: both tracks are exactly one loop long");
    }

    const auto custom = dir.getChildFile ("selftest-save.mid");
    check (p.saveMidiFile (custom, ChordbloomProcessor::ExportPart::Both) && custom.existsAsFile(), "export: saveMidiFile");
    custom.deleteFile();

    // Overwriting works repeatedly (drag the same chip twice)
    check (p.writeMidiToTempFile (ChordbloomProcessor::ExportPart::Chords) == file, "export: same chip twice reuses the file name");

    ChordbloomProcessor empty;
    check (! empty.saveMidiFile (custom, ChordbloomProcessor::ExportPart::Both), "export: saveMidiFile false for empty progression");
}

//==============================================================================
// Round 2 (processor review fixes)
//==============================================================================

// B1: VST3 hosts call processBlock with 0 samples (parameter flush). Note-offs drained in such a call must reach the
// built-in synth (Warm Pad used to drone forever), and a zeroed play head in it must not read as a transport stop.
void testZeroSampleBlocks()
{
    {
        ChordbloomProcessor p;
        setParam (p, cb::ParamIDs::previewTone, 1.0f);   // Warm Pad (infinite sustain before round 2)
        pumpMessages (20);
        Rig rig (p);
        p.startPreviewChord (C_);
        rig.run (50);
        p.stopPreview();
        const auto evs = rig.processZero();
        int offs = 0;
        for (auto& e : evs)
            offs += e.on ? 0 : 1;
        check (offs >= 3 && rig.openCount() == 0, "0-sample block: preview note-offs go out as MIDI", str (offs, " offs"));
        rig.run (600);   // 6.4 s
        check (rig.blockPeak() < 1.0e-4f, "0-sample block: the synth gets the note-offs too (no Warm Pad drone)",
               str ("peak ", rig.blockPeak()));

        // live input inside a 0-sample call: passes through and sounds
        rig.processZero (noteOn (67));
        rig.run (5);
        check (rig.openNotes (1).count (67) == 1 && rig.blockPeak() > 1.0e-4f, "0-sample block: live note-on handled");
        rig.processZero (noteOff (67));
        rig.run (600);
        check (rig.openCount() == 0 && rig.blockPeak() < 1.0e-4f, "0-sample block: live note-off reaches synth and MIDI out");
    }

    {
        ChordbloomProcessor p;
        p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
        const auto voicings = p.getProgressionVoicings();
        Rig rig (p);
        rig.head.playing = true;
        rig.run (60);   // ~1.3 beats into slot 0
        const auto before = rig.openNotes (1);

        // VST3 flush call with a zeroed play head (not playing, ppq 0)
        const double ppq = rig.head.ppq;
        rig.head.playing = false;
        rig.head.ppq = 0.0;
        const auto flush = rig.processZero();
        rig.head.playing = true;
        rig.head.ppq = ppq;
        check (flush.empty() && rig.openNotes (1) == before && ! before.empty(),
               "0-sample block with a zeroed play head: playback is not stopped", str (flush.size(), " events"));

        const auto next = rig.run (10);
        check (countOns (next) == 0 && rig.openNotes (1) == before && rig.doubleOns == 0,
               "0-sample block: playback continues seamlessly afterwards (no re-strike)", str (countOns (next), " note-ons"));
        check (p.getPlayingSlot() == 0, "0-sample block: still in slot 0");

        rig.head.playing = false;
        rig.run (2);
        check (rig.openCount() == 0, "0-sample block: real stop still closes everything");
    }
}

// B4: FL Studio's pattern mode loops ppq over the pattern length (1 bar for an empty pattern). The progression must
// keep running across those loops instead of replaying its first chord(s).
void testHostPatternLoop()
{
    for (int reportLoop = 0; reportLoop < 2; ++reportLoop)
    {
        for (int bars : { 1, 2 })
        {
            ChordbloomProcessor p;
            p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));   // 16 beats
            const auto voicings = p.getProgressionVoicings();
            Rig rig (p, 48000.0, 441);   // the pattern end falls inside a block
            rig.hostLoopLen = 4.0 * bars;
            rig.head.reportLoop = reportLoop == 1;
            rig.head.loopStart = 0.0;
            rig.head.loopEnd = rig.hostLoopLen;
            rig.head.playing = true;

            const double spb = rig.samplesPerBeat();
            const auto evs = rig.run ((int) std::ceil (33.0 * spb / rig.blockSize));

            const std::string label = str ("pattern loop ", bars, " bar", bars > 1 ? "s" : "", reportLoop ? " (loop points)" : " (no loop points)");
            bool sequenceOk = voicings.size() == 4;
            std::string why;
            for (int i = 0; i < 8 && sequenceOk; ++i)
            {
                const auto at = (juce::int64) std::floor (i * 4.0 * spb);
                const auto got = onNotesAt (evs, 1, at, 2);
                const auto& v = voicings[(size_t) (i % 4)];
                if (got != std::set<int> (v.begin(), v.end()))
                {
                    sequenceOk = false;
                    why = str ("chord ", i, " got ", notesToString ({ got.begin(), got.end() }), " want ", notesToString (v));
                }
            }
            check (sequenceOk, label + ": C Am F G keep coming across the host loops", why);
            check (rig.doubleOns == 0, label + ": no double note-ons");
            check (std::abs (p.getHostLoopBeats() - 4.0 * bars) < 1e-6, label + ": getHostLoopBeats reports the pattern length",
                   str (p.getHostLoopBeats()));

            if (bars == 1 && reportLoop == 0)
            {
                // A genuine relocation (mid-bar click) still follows the host's position
                rig.hostLoopLen = 0.0;
                rig.head.ppq = 9.37;
                rig.processBlock();
                const auto open = rig.openNotes (1);
                check (p.getPlayingSlot() == 2 && open == std::set<int> (voicings[2].begin(), voicings[2].end()),
                       "pattern loop: a mid-bar relocation still jumps to the host position", str ("slot ", p.getPlayingSlot()));
            }

            rig.head.playing = false;
            rig.run (2);
            check (rig.openCount() == 0, label + ": stop leaves nothing on");
        }
    }
}

// B6: a host loop wrapping inside a block (hosts that do not split the block at the loop end) must not flam the
// downbeat chord; loops that are relocations must not blip the chord after the loop end.
void testHostLoopWrapNoFlam()
{
    for (int reportLoop = 0; reportLoop < 2; ++reportLoop)
    {
        ChordbloomProcessor p;
        p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
        const auto voicings = p.getProgressionVoicings();
        const auto expected = p.renderProgressionEvents();
        Rig rig (p, 48000.0, 441);
        rig.hostLoopLen = 16.0;
        rig.head.reportLoop = reportLoop == 1;
        rig.head.loopEnd = 16.0;
        rig.head.playing = true;
        const double spb = rig.samplesPerBeat();
        rig.run ((int) std::ceil (49.0 * spb / rig.blockSize));

        const std::string label = reportLoop ? "loop wrap (loop points)" : "loop wrap (no loop points)";
        const int window = (int) (0.05 * rig.sr);
        bool once = true;
        std::string why;
        for (int k = 1; k <= 2; ++k)
        {
            const auto wrap = (juce::int64) std::floor (16.0 * k * spb);
            for (int n : voicings[0])
            {
                int ons = 0, offsAfter = 0;
                for (auto& e : rig.all)
                {
                    if (e.channel != 1 || e.note != n || e.sample < wrap - window || e.sample > wrap + window)
                        continue;
                    if (e.on)
                        ++ons;
                    else if (e.sample > wrap)
                        ++offsAfter;
                }
                if (ons != 1 || offsAfter != 0)
                {
                    once = false;
                    why = str ("loop ", k, " note ", n, ": ", ons, " ons, ", offsAfter, " offs after the downbeat");
                }
            }
        }
        check (once, label + ": downbeat chord struck exactly once per loop (no flam)", why);
        checkLoopAgainstRender (rig.all, expected, spb, 16.0, 0, label + " loop 2");
        check (rig.doubleOns == 0, label + ": no double note-ons");
        rig.head.playing = false;
        rig.run (2);
        check (rig.openCount() == 0, label + ": nothing left on");
    }

    // 24-beat host loop over a 16-beat progression (a relocation every loop) with loop points: no blip of slot 2
    {
        ChordbloomProcessor p;
        p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
        const auto voicings = p.getProgressionVoicings();
        Rig rig (p, 48000.0, 441);
        rig.hostLoopLen = 24.0;
        rig.head.reportLoop = true;
        rig.head.loopEnd = 24.0;
        rig.head.playing = true;
        const double spb = rig.samplesPerBeat();
        rig.run ((int) std::ceil (25.0 * spb / rig.blockSize));

        const auto wrap = (juce::int64) std::floor (24.0 * spb);
        const int window = (int) (0.05 * rig.sr);
        std::set<int> struck;
        for (auto& e : rig.all)
            if (e.on && e.channel == 1 && e.sample >= wrap - window && e.sample <= wrap + window)
                struck.insert (e.note);
        check (struck == std::set<int> (voicings[0].begin(), voicings[0].end()),
               "24-beat host loop: only the slot-0 chord is struck at the wrap (no blip of the next slot)",
               notesToString ({ struck.begin(), struck.end() }));
        check (p.getHostLoopBeats() == 0.0 && rig.doubleOns == 0, "24-beat host loop: treated as relocation, balanced");
        rig.head.playing = false;
        rig.run (2);
        check (rig.openCount() == 0, "24-beat host loop: nothing left on");
    }
}

// B7: velocity / gate / swing / humanize tweaks while the loop plays re-time notes but never strike mid-step.
void testTimingTweaksDoNotRestrike()
{
    ChordbloomProcessor p;
    namespace id = cb::ParamIDs;
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
    setParam (p, id::pattern, (float) cb::RhythmPattern::Pulse);
    setParam (p, id::rate, (float) cb::RateDivision::Eighth);
    setParam (p, id::bassMode, (float) cb::BassMode::Off);
    setParam (p, id::gate, 0.2f);
    pumpMessages (100);

    Rig rig (p);
    p.setLocalPlayback (true);
    const double spb = rig.samplesPerBeat();
    auto runTo = [&] (double beat)
    {
        std::vector<Ev> evs;
        while ((double) (rig.samplePos + rig.blockSize) <= beat * spb)
        {
            auto e = rig.processBlock();
            evs.insert (evs.end(), e.begin(), e.end());
        }
        return evs;
    };
    auto sampleOf = [&] (double beat) { return (juce::int64) std::floor (beat * spb); };

    runTo (1.24);
    setParam (p, id::gate, 1.0f);
    pumpMessages (80);
    auto evs = runTo (1.49);
    check (countOns (evs) == 0, "gate 0.2 -> 1.0 mid-step: no ghost hit before the next step", str (countOns (evs), " note-ons"));
    evs = runTo (1.62);
    check (countOns (evs, sampleOf (1.5) - 2, sampleOf (1.5) + 2) >= 3, "gate change: the next step plays normally");

    // Swing 0 -> 1 while the off-beat chord at 2.5 sounds: it is neither cut nor struck again at its swung position
    setParam (p, id::gate, 0.9f);
    pumpMessages (80);
    runTo (2.52);
    const auto heldBefore = rig.openNotes (1);
    setParam (p, id::swing, 1.0f);
    pumpMessages (80);
    evs = runTo (2.98);
    int offsEarly = 0;
    for (auto& e : evs)
        if (! e.on && e.sample < sampleOf (2.66))
            ++offsEarly;
    check (! heldBefore.empty() && countOns (evs) == 0 && offsEarly == 0,
           "swing change mid-step: sounding off-beat chord not cut, not re-struck",
           str (countOns (evs), " note-ons, ", offsEarly, " early note-offs"));
    evs = runTo (3.1);
    check (countOns (evs, sampleOf (3.0) - 2, sampleOf (3.0) + 2) >= 3, "swing change: the downbeat at 3.0 plays normally");

    // Humanize mid-step
    runTo (3.2);
    setParam (p, id::humanize, 1.0f);
    pumpMessages (80);
    evs = runTo (3.45);
    check (countOns (evs) == 0, "humanize change mid-step: no extra strike", str (countOns (evs), " note-ons"));

    // A structural change (pattern) still chases the new notes immediately
    runTo (5.0);
    setParam (p, id::pattern, (float) cb::RhythmPattern::Block);
    pumpMessages (80);
    rig.processBlock();
    const auto vAm = p.getProgressionVoicings()[1];
    check (rig.openNotes (1) == std::set<int> (vAm.begin(), vAm.end()), "pattern change mid-slot: the held chord is chased");

    p.setLocalPlayback (false);
    rig.run (2);
    check (rig.openCount() == 0 && rig.doubleOns == 0, "timing tweaks: balanced, nothing left on");
}

// I21 + B8: the built-in synth honours CC64; switching MIDI Out off releases a forwarded pedal / bend downstream.
void testSustainPedal()
{
    auto cc = [] (int controller, int value, int channel = 1)
    {
        juce::MidiBuffer b;
        b.addEvent (juce::MidiMessage::controllerEvent (channel, controller, value), 0);
        return b;
    };

    {
        ChordbloomProcessor p;
        Rig rig (p);
        rig.processBlock (noteOn (72));
        rig.run (20);
        rig.processBlock (noteOff (72));
        rig.run (60);   // ~0.64 s
        check (rig.blockPeak() < 5.0e-4f, "synth: without the pedal a released note fades", str ("peak ", rig.blockPeak()));

        rig.processBlock (cc (64, 127));
        rig.processBlock (noteOn (72));
        rig.run (20);
        rig.processBlock (noteOff (72));
        rig.run (60);
        check (rig.blockPeak() > 1.0e-3f, "synth: sustain pedal keeps the released note ringing", str ("peak ", rig.blockPeak()));
        check (rig.openCount() == 0, "sustain: the note-off itself is still sent as MIDI out");
        rig.processBlock (cc (64, 0));
        rig.run (100);
        check (rig.blockPeak() < 1.0e-3f, "synth: pedal up releases the sustained note", str ("peak ", rig.blockPeak()));
    }

    {
        ChordbloomProcessor p;
        Rig rig (p);
        rig.processBlock (cc (64, 127));
        juce::MidiBuffer bend;
        bend.addEvent (juce::MidiMessage::pitchWheel (1, 12000), 0);
        rig.processBlock (bend);
        rig.processBlock (noteOn (60));
        rig.processBlock (noteOff (60));
        setParam (p, cb::ParamIDs::midiOut, 0.0f);
        rig.processBlock();
        bool pedalUp = false, bendCentred = false, otherChannels = false;
        for (auto& m : rig.lastOthers)
        {
            if (m.isSustainPedalOff() && m.getChannel() == 1)
                pedalUp = true;
            else if (m.isPitchWheel() && m.getChannel() == 1 && m.getPitchWheelValue() == 8192)
                bendCentred = true;
            else
                otherChannels = true;
        }
        check (pedalUp && bendCentred, "MIDI Out off: a forwarded sustain pedal is released and the bend centred",
               str (rig.lastOthers.size(), " messages"));
        check (! otherChannels, "MIDI Out off: only the channels that need it get a reset");

        rig.processBlock (cc (64, 127));
        check (rig.lastOthers.empty(), "MIDI Out off: live controllers are not forwarded");
        setParam (p, cb::ParamIDs::midiOut, 1.0f);
        rig.processBlock (cc (64, 0));
        check (rig.lastOthers.size() == 1, "MIDI Out on again: controllers forwarded");
    }
}

// B15: an input note-off without a matching note-on must not cut a preview / playback note of the same pitch.
void testUnmatchedNoteOff()
{
    ChordbloomProcessor p;
    Rig rig (p);
    p.startPreviewNote (60);
    rig.processBlock();
    rig.processBlock (noteOff (60));
    check (rig.openNotes (1).count (60) == 1 && p.getSoundingNotes().test (60),
           "unmatched live note-off does not cut the preview note of the same pitch");
    p.stopPreview();
    rig.processBlock();
    check (rig.openCount() == 0, "unmatched note-off: the preview's own note-off closes it");

    const auto evs = rig.processBlock (noteOff (62, 5));
    check (findEvent (evs, false, 1, 62, rig.samplePos - rig.blockSize + 5, 0) != nullptr,
           "unmatched note-off passes through when nothing holds the note");

    // Same with playback on channel 1
    p.setProgression (makeProgression ({ C_ }));
    p.setLocalPlayback (true);
    rig.run (4);
    const auto open = rig.openNotes (1);
    const int n = open.empty() ? 60 : *open.begin();
    rig.processBlock (noteOff (n));
    check (! open.empty() && rig.openNotes (1) == open, "unmatched note-off does not cut a playback note");
    p.setLocalPlayback (false);
    rig.run (2);
    check (rig.openCount() == 0, "unmatched note-off: balanced afterwards");
}

// B17: the first bypassed block's cleanup note-offs come before the block's input (a new note-on is not cancelled).
void testBypassOrdering()
{
    ChordbloomProcessor p;
    Rig rig (p);
    p.startPreviewNote (60);
    rig.processBlock();
    check (rig.openNotes (1).count (60) == 1, "bypass order: preview note on before");

    rig.bypassed = true;
    const auto evs = rig.processBlock (noteOn (60, 0, 1));
    check (evs.size() == 2 && ! evs[0].on && evs[1].on && rig.openNotes (1).count (60) == 1,
           "bypass: cleanup note-off precedes a new input note-on of the same pitch", str (evs.size(), " events"));
    rig.processBlock (noteOff (60, 0, 1));
    rig.bypassed = false;
    p.stopPreview();
    rig.run (2);
    check (rig.openCount() == 0, "bypass order: balanced afterwards");
}

// I13: midiOutPart filters playback MIDI out (previews and live input count as chords); the synth plays everything.
void testMidiOutPart()
{
    namespace id = cb::ParamIDs;
    ChordbloomProcessor p;
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
    Rig rig (p);
    rig.head.playing = true;
    const double spb = rig.samplesPerBeat();

    setParam (p, id::midiOutPart, 1.0f);   // Chords only
    auto evs = rig.run ((int) std::ceil (6.0 * spb / rig.blockSize));   // mid slot 1
    bool bassSounds = false;
    for (int n = 28; n <= 55; ++n)
        bassSounds = bassSounds || p.getSoundingNotes().test ((size_t) n);
    check (countOns (evs, 0, std::numeric_limits<juce::int64>::max(), 2) == 0 && countOns (evs, 0, std::numeric_limits<juce::int64>::max(), 1) >= 6,
           "midiOutPart Chords only: no bass notes on MIDI out");
    check (bassSounds, "midiOutPart: the built-in sound still plays the bass");

    setParam (p, id::midiOutPart, 2.0f);   // Bass only
    evs = rig.run (2);
    check (rig.openNotes (1).empty(), "midiOutPart -> Bass only: the sent chord notes are closed at once");
    p.startPreviewChord (F_);
    evs = rig.processBlock (noteOn (90, 3, 1));
    auto more = rig.run ((int) std::ceil (8.0 * spb / rig.blockSize));
    evs.insert (evs.end(), more.begin(), more.end());
    check (countOns (evs, 0, std::numeric_limits<juce::int64>::max(), 1) == 0 && countOns (evs, 0, std::numeric_limits<juce::int64>::max(), 2) >= 1,
           "midiOutPart Bass only: only playback bass goes out (no chords, previews or live input)");
    check (p.getSoundingNotes().test (90), "midiOutPart Bass only: live input still sounds internally");
    rig.processBlock (noteOff (90, 0, 1));
    p.stopPreview();

    setParam (p, id::midiOutPart, 1.0f);   // switch while a bass note sounds
    rig.run (2);
    check (rig.openNotes (2).empty(), "midiOutPart -> Chords only: the sent bass note is closed at once");

    setParam (p, id::midiOutPart, 0.0f);
    rig.run ((int) std::ceil (4.0 * spb / rig.blockSize));
    check (! rig.openNotes (1).empty() && ! rig.openNotes (2).empty(), "midiOutPart Chords + Bass: both parts again");
    p.startPreviewChord (C_);
    rig.run (2);
    p.stopPreview();
    rig.head.playing = false;
    rig.run (2);
    check (rig.openCount() == 0 && rig.doubleOns == 0, "midiOutPart: nothing left on, never a double note-on",
           str (rig.openCount(), " open, ", rig.doubleOns, " double"));
}

// B14 + I25: export names come from the progression, keep '#', contain nothing Windows forbids, <= 60 characters.
void testExportNames()
{
    namespace id = cb::ParamIDs;
    using Part = ChordbloomProcessor::ExportPart;
    auto legal = [] (const juce::String& n)
    {
        return n.length() <= 60 && ! n.containsAnyOf ("<>:\"/\\|?*") && n.endsWith (".mid") && ! n.dropLastCharacters (4).endsWith (" ")
            && ! n.dropLastCharacters (4).endsWith (".");
    };

    ChordbloomProcessor p;
    setParam (p, id::keyRoot, 6.0f);
    setParam (p, id::scale, 1.0f);   // F# Natural Minor
    pumpMessages (30);
    p.setProgression (makeProgression ({ { 6, cb::ChordQuality::Minor }, { 2, cb::ChordQuality::Major },
                                         { 9, cb::ChordQuality::Major }, { 4, cb::ChordQuality::Major } }));
    const auto name = p.getExportFileName (Part::Chords);
    check (name == "F#m D A E - F# minor - Chords.mid", "export name: from the progression, '#' kept", name.toStdString());
    const auto file = p.writeMidiToTempFile (Part::Chords);
    check (file.getFileName() == name && file.existsAsFile(), "export name: temp file uses it", file.getFullPathName().toStdString());
    check (p.getExportFileName (Part::Bass) == "F#m D A E - F# minor - Bass.mid"
               && p.getExportFileName (Part::Both) == "F#m D A E - F# minor - Chords + Bass.mid",
           "export name: part labels");

    ChordbloomProcessor fMajor, fSharpMajor;
    setParam (fMajor, id::keyRoot, 5.0f);
    setParam (fSharpMajor, id::keyRoot, 6.0f);
    pumpMessages (30);
    fMajor.setProgression (makeProgression ({ F_, C_ }));
    fSharpMajor.setProgression (makeProgression ({ { 6, cb::ChordQuality::Major }, { 1, cb::ChordQuality::Major } }));
    const auto nf = fMajor.getExportFileName (Part::Chords), nfs = fSharpMajor.getExportFileName (Part::Chords);
    check (nf != nfs && nfs.contains ("F#") && nfs.contains ("F# major"), "export name: F# major differs from F major",
           (nf + " / " + nfs).toStdString());

    ChordbloomProcessor longOne;
    cb::Progression big;
    for (int i = 0; i < cb::kMaxSlots; ++i)
        big.slots.push_back ({ { (i * 7) % 12, (cb::ChordQuality) (i % cb::numChordQualities) }, 2, -1, 0 });
    longOne.setProgression (big);
    bool allLegal = true;
    std::string bad;
    for (auto part : { Part::Chords, Part::Bass, Part::Both })
    {
        const auto n = longOne.getExportFileName (part);
        if (! legal (n))
        {
            allLegal = false;
            bad += n.toStdString() + " | ";
        }
    }
    const auto longName = longOne.getExportFileName (Part::Both);
    check (allLegal && longName.contains (" ...") && longName.endsWith (" - C major - Chords + Bass.mid"),
           "export name: 16 chords of every quality -> shortened, legal, <= 60 chars", bad + longName.toStdString());
    check (longOne.writeMidiToTempFile (Part::Both).existsAsFile(), "export name: long progression writes a file");

    cb::Progression sixNine;
    sixNine.slots.push_back ({ { 0, cb::ChordQuality::Major69 }, 4, -1, 0 });
    longOne.setProgression (sixNine);
    check (legal (longOne.getExportFileName (Part::Chords)), "export name: 6/9 chord has no '/'",
           longOne.getExportFileName (Part::Chords).toStdString());

    ChordbloomProcessor empty;
    check (empty.getExportFileName (Part::Chords) == "Chordbloom - C major - Chords.mid", "export name: empty progression",
           empty.getExportFileName (Part::Chords).toStdString());
}

// I3: a single chord / a single slot as a MIDI file (for drags straight into the DAW).
void testSingleChordExport()
{
    auto readFile = [] (const juce::File& f, juce::MidiFile& out)
    {
        juce::FileInputStream in (f);
        return in.openedOk() && out.readFrom (in);
    };
    auto notesOf = [] (const juce::MidiFile& mf, int& ons, int& firstTick, int& channelMask, double& eot)
    {
        std::set<int> notes;
        ons = 0;
        firstTick = -1;
        channelMask = 0;
        eot = -1.0;
        for (int t = 1; t < mf.getNumTracks(); ++t)
            for (auto* h : *mf.getTrack (t))
            {
                if (h->message.isNoteOn())
                {
                    ++ons;
                    notes.insert (h->message.getNoteNumber());
                    channelMask |= 1 << h->message.getChannel();
                    if (firstTick < 0 || h->message.getTimeStamp() < firstTick)
                        firstTick = (int) h->message.getTimeStamp();
                }
                if (h->message.isEndOfTrackMetaEvent())
                    eot = h->message.getTimeStamp();
            }
        return notes;
    };

    const cb::Chord Am7 { 9, cb::ChordQuality::Minor7 };
    ChordbloomProcessor p;
    p.startPreviewChord (Am7);   // the drag starts with a press: the file uses what was heard
    const auto heard = p.getPreviewNotes();
    p.stopPreview();
    const auto f = p.writeChordMidiToTempFile (Am7);
    juce::MidiFile mf;
    int ons = 0, first = 0, channels = 0;
    double eot = 0.0;
    const bool parsed = f.existsAsFile() && readFile (f, mf);
    const auto notes = parsed ? notesOf (mf, ons, first, channels, eot) : std::set<int>();
    check (parsed && f.getFileName() == "Am7 - C major.mid", "single chord export: file written and parses back", f.getFullPathName().toStdString());
    check (notes == std::set<int> (heard.begin(), heard.end()) && ons == (int) heard.size() && first == 0 && channels == (1 << 1),
           "single chord export: the previewed Am7 voicing, chord channel only, at tick 0",
           notesToString ({ notes.begin(), notes.end() }) + " vs " + notesToString (heard));
    check (std::abs (eot - 4.0 * 960.0) < 0.5, "single chord export: 4 beats long", str ("eot ", eot));

    setParam (p, cb::ParamIDs::pattern, (float) cb::RhythmPattern::Pulse);
    pumpMessages (50);
    const auto pulse = p.writeChordMidiToTempFile (G_);
    juce::MidiFile pm;
    const bool pulseOk = readFile (pulse, pm);
    notesOf (pm, ons, first, channels, eot);
    check (pulseOk && ons == 8 * 3, "single chord export: current pattern applies (Pulse 1/8 -> 8 strikes)", str (ons, " note-ons"));
    setParam (p, cb::ParamIDs::pattern, 0.0f);
    pumpMessages (50);

    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
    const auto voicings = p.getProgressionVoicings();
    const auto s = p.writeSlotMidiToTempFile (1);
    juce::MidiFile sm;
    const bool slotOk = s.existsAsFile() && readFile (s, sm);
    const auto slotNotes = slotOk ? notesOf (sm, ons, first, channels, eot) : std::set<int>();
    check (slotOk && slotNotes == std::set<int> (voicings[1].begin(), voicings[1].end()) && first == 0 && channels == (1 << 1)
               && std::abs (eot - 4.0 * 960.0) < 0.5,
           "slot export: that slot's voicing, shifted to beat 0, one slot long", notesToString ({ slotNotes.begin(), slotNotes.end() }));
    check (p.writeSlotMidiToTempFile (7) == juce::File() && p.writeSlotMidiToTempFile (-1) == juce::File(), "slot export: invalid slot -> File()");
}

// I10: transpose (undoable) and the "move chords to key" suggestion.
void testTranspose()
{
    namespace id = cb::ParamIDs;
    ChordbloomProcessor p;
    check (p.bestTranspositionForKey() == 0, "transpose: empty progression -> 0");
    const auto original = makeProgression ({ C_, G_, Am_, F_ });
    p.setProgression (original);
    check (p.bestTranspositionForKey() == 0, "best transposition: already diatonic -> 0");

    setParam (p, id::keyRoot, 6.0f);
    setParam (p, id::scale, 1.0f);   // F# minor
    pumpMessages (50);
    const int best = p.bestTranspositionForKey();
    check (best == -3, "best transposition: C G Am F into F# minor -> -3 (A E F#m D)", str ("got ", best));

    ChangeCounter counter;
    p.addChangeListener (&counter);
    p.transposeProgression (best);
    pumpMessages (30);
    const auto& s = p.getProgression().slots;
    bool allDiatonic = s.size() == 4;
    for (auto& slot : s)
        allDiatonic = allDiatonic && cb::isDiatonic (slot.chord, p.getKey());
    check (s.size() == 4 && s[0].chord.root == 9 && s[1].chord.root == 4 && s[2].chord.root == 6 && s[3].chord.root == 2
               && s[2].chord.quality == cb::ChordQuality::Minor && allDiatonic,
           "transpose: every root moved, qualities kept, all diatonic now");
    check (counter.count > 0, "transpose: sends a change message");
    check (p.bestTranspositionForKey() == 0, "best transposition after moving: 0");
    check (p.canUndo() && p.undo() && p.getProgression() == original, "transpose: undo restores the original");
    check (p.redo() && p.getProgression().slots[0].chord.root == 9, "transpose: redo");

    const auto before = p.getProgression();
    p.undo();
    p.redo();
    const bool couldUndo = p.canUndo();
    p.transposeProgression (12);
    p.transposeProgression (0);
    check (p.getProgression() == before && couldUndo == p.canUndo(), "transpose by 0 / 12: no change");
    p.transposeProgression (-1);
    check (p.getProgression().slots[0].chord.root == 8, "transpose -1 semitone");
    p.removeChangeListener (&counter);

    const int range = p.bestTranspositionForKey();
    check (range >= -5 && range <= 6, "best transposition stays within -5..+6", str (range));
}

// B27: Soft Keys must not alias on high notes at full velocity (the 9th harmonic of MIDI 104 folds to ~14.2 kHz).
void testSoftKeysAliasing()
{
    constexpr double sr = 44100.0;
    constexpr int n = 882;   // first 20 ms, where the modulation index is highest
    PreviewSynth synth;
    synth.prepare (sr, 1024);
    synth.setTone (PreviewSynth::Tone::SoftKeys);
    synth.setGainDecibels (-9.0f);
    juce::AudioBuffer<float> buf (2, n);
    buf.clear();
    juce::MidiBuffer m;
    m.addEvent (juce::MidiMessage::noteOn (1, 104, (juce::uint8) 127), 0);
    synth.render (buf, m, n);

    auto magnitude = [&] (double freq)
    {
        double re = 0.0, im = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / (n - 1));   // Hann
            const double x = (double) buf.getSample (0, i) * w;
            re += x * std::cos (juce::MathConstants<double>::twoPi * freq * i / sr);
            im -= x * std::sin (juce::MathConstants<double>::twoPi * freq * i / sr);
        }
        return std::sqrt (re * re + im * im);
    };

    const double f0 = juce::MidiMessage::getMidiNoteInHertz (104);
    const double fundamental = magnitude (f0);
    const double alias9 = magnitude (std::abs (9.0 * f0 - sr));
    const double alias7 = magnitude (std::abs (7.0 * f0 - sr));
    check (fundamental > 0.0 && alias9 / fundamental < 0.03 && alias7 / fundamental < 0.05,
           "Soft Keys MIDI 104 vel 127: folded sidebands at least ~30 dB down",
           str ("9f ", alias9 / fundamental, " 7f ", alias7 / fundamental));
}

// B1 safety net: a Warm Pad note whose note-off is lost fades out after ~30 s instead of droning forever, but holds
// its level for long chords.
void testWarmPadFiniteSustain()
{
    constexpr double sr = 48000.0;
    constexpr int block = 4800;
    PreviewSynth synth;
    synth.prepare (sr, block);
    synth.setTone (PreviewSynth::Tone::WarmPad);
    synth.setGainDecibels (-9.0f);
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer on;
    on.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);

    float atOne = 0.0f, atFifteen = 0.0f, atEnd = 0.0f;
    for (int b = 0; b < 340; ++b)   // 34 s
    {
        buf.clear();
        synth.render (buf, b == 0 ? on : juce::MidiBuffer(), block);
        const float peak = buf.getMagnitude (0, 0, block);
        if (b == 10) atOne = peak;
        if (b == 150) atFifteen = peak;
        if (b == 339) atEnd = peak;
    }
    check (atFifteen > 0.7f * atOne, "Warm Pad: holds its level for long chords (15 s)", str (atOne, " -> ", atFifteen));
    check (atEnd < 1.0e-3f, "Warm Pad: a note without note-off fades out after ~30 s", str ("peak ", atEnd));
}

//==============================================================================
// F5: MIDI that passes through a host bypass is tracked. A key pressed during the bypass and held across its end is
// released exactly once afterwards and is never double-struck by playback; with MIDI Out off or Bass only it is closed
// (and a pedal pressed during the bypass released) when the bypass ends. No hanging notes or pedals in any mode.
void testBypassPassThrough()
{
    namespace id = cb::ParamIDs;
    auto cc = [] (int controller, int value, int channel, int sample = 0)
    {
        juce::MidiBuffer b;
        b.addEvent (juce::MidiMessage::controllerEvent (channel, controller, value), sample);
        return b;
    };

    for (int midiOut = 1; midiOut >= 0; --midiOut)
        for (int part = 0; part < 3; ++part)
        {
            const std::string label = str ("bypass pass-through (MIDI Out ", midiOut ? "on" : "off", ", part ", part, "): ");
            ChordbloomProcessor p;
            p.setProgression (makeProgression ({ C_ }));
            setParam (p, id::midiOut, (float) midiOut);
            setParam (p, id::midiOutPart, (float) part);
            const auto voicings = p.getProgressionVoicings();
            const int key = voicings.empty() || voicings[0].size() < 2 ? 64 : voicings[0][1];   // a note of the C chord

            Rig rig (p);
            p.setLocalPlayback (true);
            rig.run (4);

            rig.bypassed = true;
            juce::MidiBuffer in;
            in.addEvent (juce::MidiMessage::noteOn (1, key, (juce::uint8) 100), 5);
            in.addEvent (juce::MidiMessage::noteOn (2, 50, (juce::uint8) 100), 6);
            in.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), 7);
            rig.processBlock (in);
            rig.run (6);
            rig.bypassed = false;

            rig.run (300);   // playback strikes the held key's pitch again and again (~3 s, 4-beat slot at 120 bpm)
            check (rig.doubleOns == 0, label + "playback never double-strikes a key held across the bypass",
                   str (rig.doubleOns, " double note-ons"));

            juce::MidiBuffer release;
            release.addEvent (juce::MidiMessage::noteOff (1, key), 0);
            release.addEvent (juce::MidiMessage::noteOff (2, 50), 0);
            rig.processBlock (release);
            p.setLocalPlayback (false);
            rig.run (4);
            rig.processBlock (cc (64, 0, 1));
            rig.run (2);

            check (rig.openCount() == 0, label + "nothing left on downstream", str (rig.openCount(), " open"));
            check (rig.pedalsDown() == 0, label + "no pedal left down downstream", str (rig.pedalsDown(), " down"));
            check (rig.doubleOns == 0, label + "never a double note-on", str (rig.doubleOns));
        }

    // A key pressed during the bypass and released after it: with Bass only the routed synth still gets its note-off.
    {
        ChordbloomProcessor p;
        setParam (p, id::midiOutPart, 2.0f);
        Rig rig (p);
        rig.run (2);
        rig.bypassed = true;
        rig.processBlock (noteOn (70));
        rig.bypassed = false;
        rig.run (2);
        rig.processBlock (noteOff (70));
        check (rig.openCount() == 0, "bypass pass-through: Bass only closes a key pressed during the bypass");
    }
}

// F6: live input keeps its order at one sample: a key released just before the pedal goes down is not sustained.
void testSameSampleInputOrder()
{
    auto orderAt = [] (const juce::MidiBuffer& out, int sample)
    {
        std::string s;
        for (const auto meta : out)
        {
            if (meta.samplePosition != sample)
                continue;
            const auto m = meta.getMessage();
            if (m.isNoteOff() && m.getNoteNumber() == 72)
                s += "off ";
            else if (m.isController() && m.getControllerNumber() == 64)
                s += m.getControllerValue() >= 64 ? "down " : "up ";
        }
        return s;
    };

    ChordbloomProcessor p;
    Rig rig (p);
    rig.processBlock (noteOn (72));
    rig.run (20);
    juce::MidiBuffer in;
    in.addEvent (juce::MidiMessage::noteOff (1, 72), 10);
    in.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), 10);
    rig.processBlock (in);
    check (orderAt (rig.lastOut, 10) == "off down ", "input order: note-off then pedal down at one sample stays in that order",
           orderAt (rig.lastOut, 10));
    rig.run (80);   // ~0.85 s
    check (rig.blockPeak() < 5.0e-4f, "input order: a note released just before the pedal is not sustained",
           str ("peak ", rig.blockPeak()));
    {
        juce::MidiBuffer up;
        up.addEvent (juce::MidiMessage::controllerEvent (1, 64, 0), 0);
        rig.processBlock (up);
    }
    rig.run (100);

    rig.processBlock (noteOn (72));
    rig.run (20);
    juce::MidiBuffer in2;
    in2.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), 10);
    in2.addEvent (juce::MidiMessage::noteOff (1, 72), 10);
    rig.processBlock (in2);
    check (orderAt (rig.lastOut, 10) == "down off ", "input order: pedal down then note-off at one sample stays in that order",
           orderAt (rig.lastOut, 10));
    rig.run (60);
    check (rig.blockPeak() > 1.0e-3f, "input order: the pedal pressed first still sustains the note", str ("peak ", rig.blockPeak()));
    juce::MidiBuffer up;
    up.addEvent (juce::MidiMessage::controllerEvent (1, 64, 0), 0);
    rig.processBlock (up);
    rig.run (2);
    check (rig.openCount() == 0 && rig.pedalsDown() == 0, "input order: balanced afterwards");
}

// F8: the "FL loops every N bars, the progression keeps going" hint disappears once the host loop is no longer one
// the progression continues across (the FL pattern grew to 5 bars).
void testHostLoopHintClears()
{
    ChordbloomProcessor p;
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));   // 16 beats
    Rig rig (p, 48000.0, 441);
    rig.hostLoopLen = 4.0;
    rig.head.playing = true;
    const double spb = rig.samplesPerBeat();
    rig.run ((int) std::ceil (9.0 * spb / rig.blockSize));
    check (std::abs (p.getHostLoopBeats() - 4.0) < 1e-6, "host loop hint: a 1-bar FL pattern is reported", str (p.getHostLoopBeats()));

    rig.hostLoopLen = 20.0;   // the user draws a note in bar 5: every wrap is now a relocation
    rig.run ((int) std::ceil (22.0 * spb / rig.blockSize));
    check (p.getHostLoopBeats() == 0.0, "host loop hint: cleared once the host loop is not continued any more",
           str (p.getHostLoopBeats()));

    rig.hostLoopLen = 8.0;    // back to a 2-bar pattern: reported again at its next wrap
    rig.head.ppq = 0.0;
    rig.run ((int) std::ceil (17.0 * spb / rig.blockSize));
    check (std::abs (p.getHostLoopBeats() - 8.0) < 1e-6, "host loop hint: a continued loop is reported again", str (p.getHostLoopBeats()));
    rig.head.playing = false;
    rig.run (2);
    check (rig.openCount() == 0 && rig.doubleOns == 0, "host loop hint: balanced afterwards");
}

//==============================================================================
// Random host behaviour, random edits and random live input; afterwards everything must be silent.
void testFuzz()
{
    ChordbloomProcessor p;
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }, 2));
    Rig rig (p, 44100.0, 256);
    juce::Random rng (20261016);
    std::set<std::pair<int, int>> heldInput;   // (channel, note)
    namespace id = cb::ParamIDs;

    for (int b = 0; b < 6000; ++b)
    {
        juce::MidiBuffer in;
        const int r = rng.nextInt (100);

        if (r < 20)
        {
            const int channel = rng.nextInt (4) == 0 ? 2 : 1;   // channel 2 = the bass channel's input
            const int note = 36 + rng.nextInt (48);
            if (heldInput.count ({ channel, note }) == 0)
            {
                in.addEvent (juce::MidiMessage::noteOn (channel, note, (juce::uint8) (1 + rng.nextInt (127))), rng.nextInt (256));
                heldInput.insert ({ channel, note });
            }
            else
            {
                in.addEvent (juce::MidiMessage::noteOff (channel, note), rng.nextInt (256));
                heldInput.erase ({ channel, note });
            }
        }
        else if (r < 23) setParam (p, id::scaleLock, (float) rng.nextInt (2));
        else if (r < 26) setParam (p, id::chordTrigger, (float) rng.nextInt (2));
        else if (r < 28) setParam (p, id::triggerSevenths, (float) rng.nextInt (2));
        else if (r < 30) setParam (p, id::pattern, (float) rng.nextInt (cb::numRhythmPatterns));
        else if (r < 32) setParam (p, id::rate, (float) rng.nextInt (cb::numRateDivisions));
        else if (r < 33) setParam (p, id::bassMode, (float) rng.nextInt (cb::numBassModes));
        else if (r < 34) setParam (p, id::humanize, rng.nextFloat());
        else if (r < 35) setParam (p, id::keyRoot, (float) rng.nextInt (12));
        else if (r < 36) setParam (p, id::midiOut, (float) (rng.nextInt (4) > 0));
        else if (r < 37) setParam (p, id::syncToHost, (float) (rng.nextInt (4) > 0));
        else if (r < 39) rig.head.playing = ! rig.head.playing;
        else if (r < 41) rig.head.ppq = rng.nextDouble() * 40.0 - 2.0;
        else if (r < 42) rig.head.bpm = 60.0 + rng.nextDouble() * 140.0;
        else if (r < 44) p.setLocalPlayback (! p.isLocalPlaybackOn());
        else if (r < 47) p.startPreviewChord ({ rng.nextInt (12), (cb::ChordQuality) rng.nextInt (cb::numChordQualities) });
        else if (r < 49) p.stopPreview();
        else if (r < 50) p.startPreviewNote (rng.nextInt (128));
        else if (r < 52) p.addChord ({ rng.nextInt (12), (cb::ChordQuality) rng.nextInt (cb::numChordQualities) }, rng.nextInt (6) - 1);
        else if (r < 54) p.removeSlot (rng.nextInt (6));
        else if (r < 55) p.moveSlot (rng.nextInt (6), rng.nextInt (6));
        else if (r < 56) p.undo();
        else if (r < 57) p.redo();
        else if (r < 58 && ! p.getProgression().empty())
        {
            auto s = p.getProgression().slots[0];
            s.lengthBeats = 1 + rng.nextInt (8);
            s.inversion = rng.nextInt (5) - 1;
            p.updateSlot (0, s);
        }
        else if (r < 59) pumpMessages (0);
        else if (r < 60) setParam (p, id::midiOutPart, (float) rng.nextInt (3));
        else if (r < 61) setParam (p, id::gate, 0.05f + 0.95f * rng.nextFloat());
        else if (r < 62) setParam (p, id::swing, rng.nextFloat());
        else if (r < 63) in.addEvent (juce::MidiMessage::controllerEvent (1, 64, rng.nextBool() ? 127 : 0), rng.nextInt (256));
        else if (r < 64) rig.hostLoopLen = (double) (rng.nextInt (4) * 4);
        else if (r < 65) rig.head.reportLoop = rng.nextBool();
        else if (r < 66) setParam (p, id::previewTone, (float) rng.nextInt (3));
        else if (r < 67) in.addEvent (juce::MidiMessage::controllerEvent (2, 64, rng.nextBool() ? 127 : 0), rng.nextInt (256));
        else if (r < 69) rig.bypassed = ! rig.bypassed;   // host bypass on / off (F5)

        rig.head.loopStart = rig.hostLoopStart;
        rig.head.loopEnd = rig.hostLoopStart + juce::jmax (1.0, rig.hostLoopLen);
        if (r >= 69 && r < 71 && ! rig.bypassed)
            rig.processZero (in);   // VST3-style 0-sample flush calls in between
        else
            rig.processBlock (in);
    }

    pumpMessages (0);
    rig.bypassed = false;
    juce::MidiBuffer releaseAll;
    for (auto& held : heldInput)
        releaseAll.addEvent (juce::MidiMessage::noteOff (held.first, held.second), 0);
    releaseAll.addEvent (juce::MidiMessage::controllerEvent (1, 64, 0), 0);
    releaseAll.addEvent (juce::MidiMessage::controllerEvent (2, 64, 0), 0);
    setParam (p, id::midiOut, 1.0f);
    setParam (p, id::midiOutPart, 0.0f);
    p.stopPreview();
    p.setLocalPlayback (false);
    rig.head.playing = false;
    rig.processBlock (releaseAll);
    rig.run (2);

    check (rig.openCount() == 0, "fuzz: no hanging notes after random host/edit/input/bypass activity", str (rig.openCount(), " open"));
    check (rig.pedalsDown() == 0, "fuzz: no sustain pedal left down downstream", str (rig.pedalsDown(), " down"));
    check (p.getSoundingNotes().none(), "fuzz: getSoundingNotes empty at the end", str (p.getSoundingNotes().count()));
    check (rig.doubleOns == 0, "fuzz: never a double note-on", str (rig.doubleOns));
    check (rig.audioFinite && rig.peak < 4.0f, "fuzz: audio finite and bounded", str ("peak ", rig.peak));
    check (p.getTriggeredDegree() == -1, "fuzz: no chord-trigger degree left");
}
} // namespace

//==============================================================================
// Melody (harmonic guide piano roll)

void testMelodyEditingAndUndo()
{
    ChordbloomProcessor p;
    ChangeCounter changes;
    p.addChangeListener (&changes);

    check (p.getMelody().empty(), "melody: empty by default");

    // Melody edits share the undo stack with progression edits
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
    const auto m1 = makeMelody ({ melodyNote (72, 0.0, 1.0), melodyNote (76, 4.0, 2.0), melodyNote (79, 12.0, 1.0, 0.5f) });
    const int before = changes.count;
    p.setMelody (m1);
    p.dispatchPendingMessages();
    check (p.getMelody() == m1 && changes.count > before, "melody: setMelody stores it and notifies");
    p.setMelody (m1);
    p.undo();
    check (p.getMelody().empty() && p.getProgression().slots.size() == 4,
           "melody: an unchanged setMelody adds no undo step; undo removes the melody, keeps the chords");
    p.undo();
    check (p.getProgression().empty() && p.getMelody().empty(), "melody: next undo steps back through the progression edit");
    p.redo();
    p.redo();
    check (p.getMelody() == m1 && p.getProgression().slots.size() == 4, "melody: redo restores progression, then melody");

    // Messy input is sanitised
    cb::Melody messy;
    messy.notes = { melodyNote (130, -2.0, 0.001), melodyNote (60, 2.01, 1.0, 7.0f) };
    p.setMelody (messy);
    check (p.getMelody() == cb::sanitiseMelody (messy) && p.getMelody().notes[0].note == cb::kMelodyHighestNote,
           "melody: setMelody stores the sanitised melody");
    p.undo();

    // Structural edits move the notes with their chords (one undo step each)
    p.moveSlot (3, 0);   // G C Am F
    check (melodyStarts (p.getMelody()) == std::vector<double> { 0.0, 4.0, 8.0 } && p.getMelody().notes[0].note == 79,
           "melody follows moveSlot: the note over G moves to the front with it", startsToString (melodyStarts (p.getMelody())));
    p.undo();
    check (p.getMelody() == m1 && p.getProgression() == makeProgression ({ C_, Am_, F_, G_ }),
           "melody follows moveSlot: one undo restores chords and notes");

    p.removeSlot (1);   // C F G: the note over Am goes, the note over G moves 4 beats earlier
    check (melodyStarts (p.getMelody()) == std::vector<double> { 0.0, 8.0 }, "melody follows removeSlot",
           startsToString (melodyStarts (p.getMelody())));
    p.undo();

    p.duplicateSlot (1);   // C Am Am F G
    check (melodyStarts (p.getMelody()) == std::vector<double> { 0.0, 4.0, 8.0, 16.0 }, "melody follows duplicateSlot (copy gets the notes)",
           startsToString (melodyStarts (p.getMelody())));
    p.undo();

    p.addChord (Em_, 0);   // Em C Am F G
    check (melodyStarts (p.getMelody()) == std::vector<double> { 4.0, 8.0, 16.0 }, "melody follows addChord with an insert index",
           startsToString (melodyStarts (p.getMelody())));
    p.undo();
    p.addChord (Em_);      // append: the melody stays put
    check (p.getMelody() == m1, "melody: appending a chord leaves the melody alone");
    p.undo();

    auto slot = p.getProgression().slots[0];
    slot.lengthBeats = 2;   // C shortened: later notes move 2 beats earlier
    p.updateSlot (0, slot);
    check (melodyStarts (p.getMelody()) == std::vector<double> { 0.0, 2.0, 10.0 }, "melody follows a length change",
           startsToString (melodyStarts (p.getMelody())));
    p.undo();
    slot = p.getProgression().slots[1];
    slot.chord = { 9, cb::ChordQuality::Minor7 };
    p.updateSlot (1, slot);
    check (p.getMelody() == m1, "melody: changing a chord (not its length) leaves the melody alone");
    p.undo();

    // Transpose moves the melody with the chords (one undo step)
    p.transposeProgression (2);
    check (p.getMelody().notes.size() == 3 && p.getMelody().notes[0].note == 74 && p.getMelody().notes[2].note == 81
               && p.getProgression().slots[0].chord.root == 2,
           "melody: transposeProgression transposes the melody too");
    p.undo();
    check (p.getMelody() == m1 && p.getProgression().slots[0].chord.root == 0, "melody: transpose undoes in one step");

    // Replacing / clearing the chords keeps the melody in time
    p.setProgression (makeProgression ({ F_, G_ }, 8));
    check (p.getMelody() == m1, "melody: setProgression (Spark / presets) keeps the melody where it is");
    p.clearProgression();
    check (p.getMelody() == m1 && p.renderMelodyEvents().empty(), "melody: clearProgression keeps it, nothing renders without chords");
    p.undo();
    p.undo();

    p.clearMelody();
    check (p.getMelody().empty() && p.canUndo(), "melody: clearMelody is undoable");
    p.undo();
    check (p.getMelody() == m1, "melody: clearMelody undo");

    p.removeChangeListener (&changes);
}

void testMelodyState()
{
    namespace id = cb::ParamIDs;
    const auto m = makeMelody ({ melodyNote (72, 0.0, 1.0, 0.9f), melodyNote (74, 1.0 / 3.0, 1.0 / 3.0, 0.35f),
                                 melodyNote (60, 15.75, 0.25), melodyNote (90, 20.0, 2.0) });

    juce::MemoryBlock state;
    {
        ChordbloomProcessor a;
        a.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
        a.setMelody (m);
        setParam (a, id::melodyOn, 0.0f);
        a.getStateInformation (state);
    }

    ChordbloomProcessor b;
    b.setStateInformation (state.getData(), (int) state.getSize());
    check (b.getMelody() == m, "melody state: round trip is exact (triplets, velocities, parked notes)");
    check (getParam (b, id::melodyOn) < 0.5f, "melody state: melodyOn restored");
    check (! b.canUndo(), "melody state: loading clears the undo history");

    // A state saved before the melody existed: no melody, everything else as before
    {
        auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
        if (xml != nullptr)
            xml->removeChildElement (xml->getChildByName ("Melody"), true);
        juce::MemoryBlock old;
        if (xml != nullptr)
            juce::AudioProcessor::copyXmlToBinary (*xml, old);
        b.setStateInformation (old.getData(), (int) old.getSize());
        check (xml != nullptr && b.getMelody().empty() && b.getProgression().slots.size() == 4,
               "melody state: an older state without a melody loads with an empty melody");
    }

    // Hostile values are sanitised
    {
        auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
        auto* mel = xml != nullptr ? xml->getChildByName ("Melody") : nullptr;
        if (mel != nullptr)
        {
            auto* bad = mel->createNewChildElement ("Note");
            bad->setAttribute ("note", 400);
            bad->setAttribute ("start", -96);
            bad->setAttribute ("length", 0);
            bad->setAttribute ("velocity", "nan");
        }
        juce::MemoryBlock hostile;
        if (xml != nullptr)
            juce::AudioProcessor::copyXmlToBinary (*xml, hostile);
        b.setStateInformation (hostile.getData(), (int) hostile.getSize());
        const auto& got = b.getMelody();
        bool sane = got == cb::sanitiseMelody (got) && got.notes.size() == m.notes.size() + 1;
        for (auto& n : got.notes)
            sane = sane && n.note >= cb::kMelodyLowestNote && n.note <= cb::kMelodyHighestNote && n.startBeat >= 0.0
                   && n.lengthBeats > 0.0 && std::isfinite (n.velocity);
        check (mel != nullptr && sane, "melody state: hostile note values are clamped");
    }

    // Loaded from a non-message thread (some hosts do): applied on the message thread
    {
        ChordbloomProcessor c;
        std::thread loader ([&] { c.setStateInformation (state.getData(), (int) state.getSize()); });
        loader.join();
        juce::MemoryBlock echoed;
        c.getStateInformation (echoed);   // before the async apply: the loaded state must be reported back
        ChordbloomProcessor d;
        d.setStateInformation (echoed.getData(), (int) echoed.getSize());
        pumpMessages (60);
        check (c.getMelody() == m && d.getMelody() == m, "melody state: loaded off the message thread, echoed back intact");
    }
}

void testMelodyPlayback()
{
    namespace id = cb::ParamIDs;
    constexpr int mel = ChordbloomProcessor::kMelodyChannel;
    const auto anyTime = std::numeric_limits<juce::int64>::max();

    ChordbloomProcessor p;
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
    p.setMelody (makeMelody ({ melodyNote (72, 0.0, 1.0), melodyNote (76, 1.5, 0.5, 0.5f), melodyNote (79, 15.0, 4.0),
                               melodyNote (84, 16.0, 1.0) }));

    const auto rendered = p.renderMelodyEvents();
    check (rendered.size() == 3 && rendered[2].note == 79 && std::abs (rendered[2].lengthBeats - 1.0) < 1e-9
               && rendered[0].channel == mel,
           "melody render: notes inside the loop on channel 3, clipped at the loop end, parked note left out",
           str (rendered.size(), " events"));

    Rig rig (p);
    rig.head.playing = true;
    const double spb = rig.samplesPerBeat();
    rig.run ((int) std::ceil (33.0 * spb / rig.blockSize));

    const auto melodyEvs = onChannel (rig.all, mel);
    bool timingOk = true;
    std::string why;
    for (int loop = 0; loop < 2; ++loop)
    {
        for (auto& e : rendered)
        {
            const double base = loop * 16.0;
            const auto onAt = (juce::int64) std::floor ((base + e.startBeat) * spb);
            const auto offAt = (juce::int64) std::floor ((base + e.startBeat + e.lengthBeats) * spb);
            const auto* on = findEvent (melodyEvs, true, mel, e.note, onAt, 2);
            const auto* off = findEvent (melodyEvs, false, mel, e.note, offAt, 2);
            if (on == nullptr || off == nullptr || on->velocity != juce::jlimit (1, 127, juce::roundToInt (e.velocity * 127.0f)))
            {
                timingOk = false;
                why += str ("loop ", loop, " note ", e.note, "; ");
            }
        }
    }
    check (timingOk, "melody playback: every note on channel 3 at its beat with its velocity, two loops", why);
    check (countOns (rig.all, 0, (juce::int64) (16.0 * spb), mel) == 3, "melody playback: three melody notes per loop (parked note silent)",
           str (countOns (rig.all, 0, (juce::int64) (16.0 * spb), mel)));
    check (countOns (rig.all, 0, anyTime, 1) > 0 && countOns (rig.all, 0, anyTime, 2) > 0, "melody playback: chords and bass still play");

    // melodyOn off mid-note: the melody stops at once, chords go on
    rig.all.clear();
    while (rig.head.ppq < 32.0 + 15.2)   // inside the last melody note (beats 15..16 of the third loop)
        rig.processBlock();
    check (rig.openNotes (mel).count (79) == 1, "melody playback: the long note is sounding before melodyOn goes off");
    setParam (p, id::melodyOn, 0.0f);
    pumpMessages (60);
    rig.all.clear();
    rig.run ((int) std::ceil (17.0 * spb / rig.blockSize));
    check (rig.openNotes (mel).empty() && countOns (rig.all, 0, anyTime, mel) == 0 && countOns (rig.all, 0, anyTime, 1) > 0,
           "melody playback: melodyOn off closes the melody note and mutes the melody, chords keep playing");
    setParam (p, id::melodyOn, 1.0f);
    pumpMessages (60);
    rig.all.clear();
    rig.run ((int) std::ceil (17.0 * spb / rig.blockSize));
    {
        // On again at beat ~0.2: the first note is chased (0.8 beats left), then 76, 79 and the next loop's 72.
        const auto again = onChannel (rig.all, mel);
        std::set<int> notes;
        for (auto& e : again)
            if (e.on)
                notes.insert (e.note);
        check (countOns (rig.all, 0, anyTime, mel) == 4 && notes == std::set<int> { 72, 76, 79 },
               "melody playback: melodyOn on again plays the melody (the current note is picked up)",
               str (countOns (rig.all, 0, anyTime, mel), " ons"));
    }

    // MIDI out part filter
    auto runFor = [&] (double beats) { rig.all.clear(); return rig.run ((int) std::ceil (beats * spb / rig.blockSize)); };
    setParam (p, id::midiOutPart, 3.0f);   // Melody only
    auto evs = runFor (16.0);
    check (countOns (evs, 0, anyTime, mel) == 3 && countOns (evs, 0, anyTime, 1) == 0 && countOns (evs, 0, anyTime, 2) == 0,
           "midiOutPart Melody only: just the melody (channel 3) goes out");
    check (rig.openNotes (1).empty() && rig.openNotes (2).empty(), "midiOutPart -> Melody only: chord and bass notes were closed");

    while (std::fmod (rig.head.ppq, 16.0) < 15.2 || std::fmod (rig.head.ppq, 16.0) > 15.8)
        rig.processBlock();
    check (rig.openNotes (mel).count (79) == 1, "midiOutPart Melody only: the long melody note is sent");
    setParam (p, id::midiOutPart, 1.0f);   // Chords only, while the melody note sounds
    rig.processBlock();
    check (rig.openNotes (mel).empty(), "midiOutPart Melody only -> Chords only: the sent melody note is closed at once");
    check (p.getSoundingNotes().test (79), "midiOutPart: the built-in sound still plays the melody");

    evs = runFor (16.0);
    check (countOns (evs, 0, anyTime, mel) == 0 && countOns (evs, 0, anyTime, 1) > 0 && countOns (evs, 0, anyTime, 2) == 0,
           "midiOutPart Chords only: no melody");
    setParam (p, id::midiOutPart, 4.0f);   // Chords + Bass
    evs = runFor (16.0);
    check (countOns (evs, 0, anyTime, mel) == 0 && countOns (evs, 0, anyTime, 1) > 0 && countOns (evs, 0, anyTime, 2) > 0,
           "midiOutPart Chords + Bass: chords and bass, no melody");
    setParam (p, id::midiOutPart, 0.0f);   // All parts
    evs = runFor (16.0);
    check (countOns (evs, 0, anyTime, mel) == 3 && countOns (evs, 0, anyTime, 1) > 0 && countOns (evs, 0, anyTime, 2) > 0,
           "midiOutPart All parts: chords, bass and melody");

    // Editing while playing: a note drawn under the playhead starts right away, a deleted one stops
    while (std::fmod (rig.head.ppq, 16.0) < 6.1 || std::fmod (rig.head.ppq, 16.0) > 6.5)
        rig.processBlock();
    auto edited = p.getMelody();
    edited.notes.push_back (melodyNote (67, 6.0, 2.0));
    p.setMelody (edited);
    rig.processBlock();
    check (rig.openNotes (mel).count (67) == 1, "melody edit while playing: a new note under the playhead is chased");
    p.undo();
    rig.processBlock();
    check (rig.openNotes (mel).count (67) == 0, "melody edit while playing: undo stops it");

    rig.head.playing = false;
    rig.run (2);
    check (rig.openCount() == 0 && rig.doubleOns == 0 && rig.strayOffs == 0, "melody playback: nothing left on, balanced stream",
           str (rig.openCount(), " open, ", rig.doubleOns, " double, ", rig.strayOffs, " stray"));
}

void testMelodyPreview()
{
    namespace id = cb::ParamIDs;
    constexpr int mel = ChordbloomProcessor::kMelodyChannel;
    const auto anyTime = std::numeric_limits<juce::int64>::max();

    ChordbloomProcessor p;
    Rig rig (p);

    p.startPreviewMelodyNote (74);
    auto evs = rig.processBlock();
    const auto* on = findEvent (evs, true, mel, 74, 0, 0);
    check (on != nullptr && on->velocity == juce::roundToInt (cb::kDefaultMelodyVelocity * 127.0f) && ! p.getPreviewChord().has_value(),
           "melody audition: note-on on channel 3 at sample 0, no preview chord");
    check (p.getSoundingNotes().test (74), "melody audition: the built-in sound plays it");

    p.startPreviewChord (C_);   // a new audition replaces it
    evs = rig.processBlock();
    check (findEvent (evs, false, mel, 74, rig.samplePos - rig.blockSize, 0) != nullptr && countOns (evs, 0, anyTime, 1) >= 3,
           "melody audition: a chord preview closes the melody note (channel 3) and plays on channel 1");
    p.startPreviewMelodyNote (60);   // same pitch as the chord's C may be: still balanced
    rig.processBlock();
    p.stopPreview();
    rig.processBlock();

    setParam (p, id::midiOutPart, 3.0f);   // Melody only: melody auditions pass, chord previews do not
    p.startPreviewMelodyNote (76);
    evs = rig.processBlock();
    const auto auditionAt = rig.samplePos - rig.blockSize;
    p.startPreviewChord (F_);
    auto evs2 = rig.processBlock();
    p.stopPreview();
    rig.processBlock();
    check (findEvent (evs, true, mel, 76, auditionAt, 0) != nullptr && countOns (evs2, 0, anyTime, 1) == 0,
           "melody audition: counts as the melody part (passes Melody only; chord previews do not)");

    setParam (p, id::midiOutPart, 1.0f);   // Chords only: melody audition stays internal
    p.startPreviewMelodyNote (77);
    evs = rig.processBlock();
    check (countOns (evs, 0, anyTime, mel) == 0 && p.getSoundingNotes().test (77),
           "melody audition: filtered from MIDI out by Chords only, still audible");
    p.stopPreview();
    rig.run (2);
    check (rig.openCount() == 0 && rig.doubleOns == 0 && rig.strayOffs == 0 && p.getSoundingNotes().none(),
           "melody audition: balanced, nothing left on");
}

void testMelodyExport()
{
    using Part = ChordbloomProcessor::ExportPart;
    ChordbloomProcessor p;
    check (p.writeMidiToTempFile (Part::Melody) == juce::File(), "melody export: nothing without chords");
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
    check (p.writeMidiToTempFile (Part::Melody) == juce::File(), "melody export: nothing without melody notes");
    p.setMelody (makeMelody ({ melodyNote (90, 17.0, 1.0) }));
    check (p.writeMidiToTempFile (Part::Melody) == juce::File(), "melody export: nothing when every note is past the loop end");

    p.setMelody (makeMelody ({ melodyNote (72, 0.0, 1.0), melodyNote (76, 1.5, 0.5, 0.5f), melodyNote (79, 15.0, 4.0) }));
    const auto file = p.writeMidiToTempFile (Part::Melody);
    check (file.existsAsFile() && file.getFileName() == "C Am F G - C major - Melody.mid"
               && p.getExportFileName (Part::Melody) == file.getFileName(),
           "melody export: temp file named like the other parts", file.getFullPathName().toStdString());

    juce::MidiFile mf;
    {
        juce::FileInputStream in (file);
        check (in.openedOk() && mf.readFrom (in) && mf.getNumTracks() == 2, "melody export: tempo track + one melody track",
               str (mf.getNumTracks(), " tracks"));
    }

    juce::String name;
    int ons = 0, wrongChannel = 0;
    double eot = -1.0, lastOff = -1.0, secondOn = -1.0;
    if (mf.getNumTracks() == 2)
    {
        for (auto* h : *mf.getTrack (1))
        {
            const auto& msg = h->message;
            if (msg.isTrackNameEvent())
                name = msg.getTextFromTextMetaEvent();
            if (msg.isNoteOnOrOff() && msg.getChannel() != ChordbloomProcessor::kMelodyChannel)
                ++wrongChannel;
            if (msg.isNoteOn())
            {
                if (++ons == 2)
                    secondOn = msg.getTimeStamp();
            }
            if (msg.isNoteOff())
                lastOff = juce::jmax (lastOff, msg.getTimeStamp());
            if (msg.isEndOfTrackMetaEvent())
                eot = msg.getTimeStamp();
        }
    }
    check (name == "Chordbloom Melody" && ons == 3 && wrongChannel == 0, "melody export: \"Chordbloom Melody\", 3 notes on channel 3",
           str (name.toStdString(), ", ", ons, " ons"));
    check (std::abs (secondOn - 1440.0) < 0.5 && std::abs (lastOff - 16.0 * 960.0) < 0.5 && std::abs (eot - 16.0 * 960.0) < 0.5,
           "melody export: ticks match beats, the last note is clipped at the loop end, clip = one loop",
           str (secondOn, " ", lastOff, " ", eot));

    const auto saved = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Chordbloom")
                           .getChildFile ("selftest melody save.mid");
    check (p.saveMidiFile (saved, Part::Melody) && saved.existsAsFile(), "melody export: saveMidiFile (Melody)");
    saved.deleteFile();

    // The other parts never contain the melody
    juce::MidiFile both;
    {
        juce::FileInputStream in (p.writeMidiToTempFile (Part::Both));
        both.readFrom (in);
    }
    int melodyInBoth = 0;
    for (int t = 0; t < both.getNumTracks(); ++t)
        for (auto* h : *both.getTrack (t))
            if (h->message.isNoteOn() && h->message.getChannel() == ChordbloomProcessor::kMelodyChannel)
                ++melodyInBoth;
    check (both.getNumTracks() == 3 && melodyInBoth == 0, "melody export: Both stays chords + bass");

    // melodyOn only mutes playback; the export still has the melody
    setParam (p, cb::ParamIDs::melodyOn, 0.0f);
    pumpMessages (40);
    check (p.renderMelodyEvents().size() == 3 && p.writeMidiToTempFile (Part::Melody).existsAsFile(),
           "melody export: works while melodyOn is off");
}

void testGhostMelodies()
{
    auto a = std::make_unique<ChordbloomProcessor>();
    auto b = std::make_unique<ChordbloomProcessor>();
    check (b->getInstanceNumber() > a->getInstanceNumber(), "ghosts: instance numbers increase in creation order");
    check (a->getGhostMelodies().empty() && b->getGhostMelodies().empty(), "ghosts: none while nobody has a melody");

    const auto serial0 = b->getGhostSerial();
    a->setProgression (makeProgression ({ C_, Am_ }));
    a->setMelody (makeMelody ({ melodyNote (72, 0.0, 1.0), melodyNote (74, 4.0, 1.0) }));
    const auto ghosts = b->getGhostMelodies();
    check (ghosts.size() == 1 && ghosts[0].instanceNumber == a->getInstanceNumber() && ghosts[0].melody == a->getMelody()
               && std::abs (ghosts[0].loopBeats - 8.0) < 1e-9,
           "ghosts: the other instance sees the melody and its loop length");
    check (a->getGhostMelodies().empty(), "ghosts: an instance never sees its own melody");
    check (b->getGhostSerial() != serial0, "ghosts: the serial changes when a melody is published");

    // A state load publishes too
    juce::MemoryBlock state;
    a->getStateInformation (state);
    {
        ChordbloomProcessor c;
        c.setStateInformation (state.getData(), (int) state.getSize());
        check (b->getGhostMelodies().size() == 2, "ghosts: a loaded state is published");
    }
    check (b->getGhostMelodies().size() == 1, "ghosts: a deleted instance disappears");

    const auto serial1 = b->getGhostSerial();
    a.reset();
    check (b->getGhostMelodies().empty() && b->getGhostSerial() != serial1, "ghosts: closing the instance removes its ghost");
}

// Random melody edits, part switches and transport moves while playing: the MIDI stream stays balanced.
void testMelodyFuzz()
{
    namespace id = cb::ParamIDs;
    ChordbloomProcessor p;
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }, 2));
    Rig rig (p, 44100.0, 256);
    rig.head.playing = true;

    juce::Random rng (4242);
    for (int step = 0; step < 400; ++step)
    {
        switch (rng.nextInt (8))
        {
            case 0:
            {
                cb::Melody m;
                const int count = rng.nextInt (12);
                for (int i = 0; i < count; ++i)
                    m.notes.push_back (melodyNote (60 + rng.nextInt (24), rng.nextInt (40) * 0.25, 0.25 + rng.nextInt (12) * 0.25));
                p.setMelody (m);
                break;
            }
            case 1: setParam (p, id::midiOutPart, (float) rng.nextInt (5)); break;
            case 2: setParam (p, id::melodyOn, rng.nextBool() ? 1.0f : 0.0f); break;
            case 3: p.startPreviewMelodyNote (48 + rng.nextInt (36)); break;
            case 4: p.stopPreview(); break;
            case 5: rig.head.ppq = rng.nextInt (64) * 0.5; break;
            case 6: if (p.canUndo()) p.undo(); break;
            default: p.moveSlot (rng.nextInt (4), rng.nextInt (4)); break;
        }

        if (step % 16 == 0)
            pumpMessages (1);

        rig.run (1 + rng.nextInt (6));
    }

    p.stopPreview();
    rig.head.playing = false;
    rig.run (3);
    check (rig.doubleOns == 0 && rig.strayOffs == 0 && rig.openCount() == 0 && p.getSoundingNotes().none(),
           "melody fuzz: balanced stream, nothing left on",
           str (rig.doubleOns, " double, ", rig.strayOffs, " stray, ", rig.openCount(), " open"));
}

int runSelfTest()
{
    std::cout << "Chordbloom processor self-test\n";

    testBasicsAndDefaults();
    testEditingUndoRedo();
    testStateRoundTrip();
    testHostSyncedPlayback();
    testJumpsLoopsAndTempo();
    testLocalPlayback();
    testEditWhilePlaying();
    testParameterRerender();
    testPreview();
    testScaleLock();
    testChordTrigger();
    testMidiOutAndAudio();
    testReleaseResources();
    testBypass();
    testPreviewSynth();
    testMidiExport();
    testZeroSampleBlocks();
    testHostPatternLoop();
    testHostLoopWrapNoFlam();
    testTimingTweaksDoNotRestrike();
    testSustainPedal();
    testUnmatchedNoteOff();
    testBypassOrdering();
    testMidiOutPart();
    testExportNames();
    testSingleChordExport();
    testTranspose();
    testSoftKeysAliasing();
    testWarmPadFiniteSustain();
    testBypassPassThrough();
    testSameSampleInputOrder();
    testHostLoopHintClears();
    testFuzz();
    testMelodyEditingAndUndo();
    testMelodyState();
    testMelodyPlayback();
    testMelodyPreview();
    testMelodyExport();
    testGhostMelodies();
    testMelodyFuzz();

    // Phase 2: one file per work package (final-spec 8.2).
    runEngineSelfTests();   // tools/dev/SelfTestEngine.cpp (WP4)
    runRollSelfTests();     // tools/dev/SelfTestRoll.cpp (WP5)
    runShellSelfTests();    // tools/dev/SelfTestShell.cpp (WP6)

    std::cout << "\nselftest: " << numPassed << " passed, " << numFailed << " failed\n";
    return numFailed == 0 ? 0 : 1;
}
