#include "SelfTestUtil.h"

// Shell selftests (owner: WP6; final-spec 8.2 / 9 WP6): editor, roll toolbar, export panel, controls, header.
// Registered in runSelfTest (SelfTest.cpp) by WP0.

using namespace cbtest;

namespace
{
//==============================================================================
// Editor size persistence through the real editor: default size first, then whatever the user resized it to,
// also after a state round trip (project reload).
void testEditorSize()
{
    auto sizeOf = [] (ChordbloomProcessor& proc)
    {
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
        return juce::Point<int> (ed->getWidth(), ed->getHeight());
    };

    ChordbloomProcessor p;
    const auto first = sizeOf (p);
    check (first == juce::Point<int> (1100, 690), "editor: opens at the default 1100x690", str (first.x, "x", first.y));
    check (p.getSavedEditorSize() == juce::Point<int> (1100, 690) || p.getSavedEditorSize() == juce::Point<int>(),
           "editor: default size is what gets saved", str (p.getSavedEditorSize().x, "x", p.getSavedEditorSize().y));

    {
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        ed->setSize (1300, 800);
    }
    check (p.getSavedEditorSize() == juce::Point<int> (1300, 800), "editor: resized size is remembered");
    const auto again = sizeOf (p);
    check (again == juce::Point<int> (1300, 800), "editor: reopens at the remembered size", str (again.x, "x", again.y));

    juce::MemoryBlock state;
    p.getStateInformation (state);
    ChordbloomProcessor q;
    q.setStateInformation (state.getData(), (int) state.getSize());
    const auto restored = sizeOf (q);
    check (restored == juce::Point<int> (1300, 800), "editor: size survives a state round trip", str (restored.x, "x", restored.y));
}

//==============================================================================
// The real editor on the real processor, driven with synthesized mouse events: every press that starts a preview
// must stop it on release (no stuck preview notes), and double-clicking a pad appends the chord.
void testEditorInteraction()
{
    ChordbloomProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> holder (p.createEditor());
    auto* ed = dynamic_cast<ChordbloomEditor*> (holder.get());
    check (ed != nullptr, "ui: createEditor returns the Chordbloom editor");
    if (ed == nullptr)
        return;

    const auto left = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier);
    const auto none = juce::ModifierKeys();
    pumpMessages (50);

    // Palette pad: first pad of the first row is the tonic triad
    auto* padSurface = ed->getPaletteView().getChildComponent (0);
    juce::Point<float> padPos;
    for (float x = 20.0f; x < (float) padSurface->getWidth() && padPos.isOrigin(); x += 4.0f)
    {
        const juce::Point<float> pos (x, 30.0f);
        padSurface->mouseDown (mouseEventFor (*padSurface, pos, left));
        if (p.getPreviewChord().has_value())
            padPos = pos;
        padSurface->mouseUp (mouseEventFor (*padSurface, pos, none));
    }
    check (! padPos.isOrigin(), "ui palette: a pad press starts a chord preview");

    padSurface->mouseDown (mouseEventFor (*padSurface, padPos, left));
    const auto previewed = p.getPreviewChord();
    const auto previewNotes = p.getPreviewNotes();
    check (previewed == cb::Chord { 0, cb::ChordQuality::Major } && previewNotes.size() >= 3,
           "ui palette: first pad previews C major with its notes", notesToString (previewNotes));
    padSurface->mouseUp (mouseEventFor (*padSurface, padPos, none));
    check (! p.getPreviewChord().has_value() && p.getPreviewNotes().empty(), "ui palette: mouse up stops the preview");

    // Double-click: down, up, down (2 clicks), double-click, up -> one chord appended, preview stopped
    padSurface->mouseDown (mouseEventFor (*padSurface, padPos, left));
    padSurface->mouseUp (mouseEventFor (*padSurface, padPos, none));
    padSurface->mouseDown (mouseEventFor (*padSurface, padPos, left, 2));
    padSurface->mouseDoubleClick (mouseEventFor (*padSurface, padPos, left, 2));
    padSurface->mouseUp (mouseEventFor (*padSurface, padPos, none, 2));
    check (p.getProgression().slots.size() == 1 && p.getProgression().slots[0].chord == cb::Chord { 0, cb::ChordQuality::Major },
           "ui palette: double-click appends the chord", str (p.getProgression().slots.size(), " slots"));
    check (! p.getPreviewChord().has_value() && p.getPreviewNotes().empty(), "ui palette: no preview left after double-click");

    p.addChord ({ 7, cb::ChordQuality::Major });
    pumpMessages (50);
    ed->updateChordDisplays();

    // Lane block: press previews the slot's voicing, release stops it
    auto& lane = ed->getProgressionLane();
    auto* laneSurface = lane.getChildComponent (0);
    juce::Point<float> blockPos;
    for (float y = (float) laneSurface->getHeight() - 12.0f; y > 30.0f && blockPos.isOrigin(); y -= 6.0f)
    {
        const juce::Point<float> pos (40.0f, y);
        laneSurface->mouseDown (mouseEventFor (*laneSurface, pos, left));
        if (p.getPreviewChord().has_value())
            blockPos = pos;
        laneSurface->mouseUp (mouseEventFor (*laneSurface, pos, none));
    }
    check (! blockPos.isOrigin(), "ui lane: pressing a block previews it");
    laneSurface->mouseDown (mouseEventFor (*laneSurface, blockPos, left));
    const auto voicings = p.getProgressionVoicings();
    check (lane.getSelectedSlot() == 0 && ! voicings.empty() && p.getPreviewNotes() == voicings[0],
           "ui lane: block press selects it and previews its exact voicing", notesToString (p.getPreviewNotes()));
    laneSurface->mouseUp (mouseEventFor (*laneSurface, blockPos, none));
    check (! p.getPreviewChord().has_value() && p.getPreviewNotes().empty(), "ui lane: mouse up stops the preview");

    // Keyboard: press a key, slide to another, release
    KeyboardGuide* keyboard = nullptr;
    for (auto* child : ed->getChildren())
        if (auto* k = dynamic_cast<KeyboardGuide*> (child))
            keyboard = k;
    check (keyboard != nullptr && keyboard->getNumChildComponents() > 0, "ui keyboard: present");
    if (keyboard != nullptr && keyboard->getNumChildComponents() > 0)
    {
        auto* keys = keyboard->getChildComponent (0);
        const float y = (float) keys->getHeight() - 6.0f;
        const juce::Point<float> a (4.0f, y), b ((float) keys->getWidth() * 0.5f, y);
        keys->mouseDown (mouseEventFor (*keys, a, left));
        const auto first = p.getPreviewNotes();
        keys->mouseDrag (mouseEventFor (*keys, b, left, 1, true));
        const auto second = p.getPreviewNotes();
        keys->mouseUp (mouseEventFor (*keys, b, none, 1, true));
        check (first.size() == 1 && second.size() == 1 && first != second, "ui keyboard: press and glide play single notes",
               notesToString (first) + " -> " + notesToString (second));
        check (p.getPreviewNotes().empty(), "ui keyboard: mouse up stops the note");
    }

    // Nothing may be left sounding once the audio thread has caught up
    {
        Rig rig (p);
        rig.run (4);
        check (rig.openCount() == 0 && p.getSoundingNotes().none(), "ui: no stuck preview notes after the interactions",
               str (rig.openCount(), " open"));
    }
}

// I26: a new instance starts with the last key / scale / tone picked in the editor; a loaded state always wins.
void testUserDefaults()
{
    namespace id = cb::ParamIDs;
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Chordbloom-selftest.settings");
    file.deleteFile();
    ChordbloomProcessor::setUserDefaultsFile (file);

    {
        ChordbloomProcessor fresh;
        check (fresh.getKey().tonic == 0 && fresh.getKey().scale == cb::ScaleType::Major, "user defaults: none yet -> C Major");
    }
    {
        ChordbloomProcessor p;
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditorAndMakeActive());
        setParam (p, id::keyRoot, 6.0f);
        setParam (p, id::scale, 1.0f);
        setParam (p, id::previewTone, 1.0f);
        pumpMessages (500);
        check (! file.existsAsFile(), "user defaults: parameter changes alone (host / validator) are not recorded");
        p.rememberUserDefaults();   // what the header's key / scale / tone menus call after the user's pick
        pumpMessages (500);
        check (file.existsAsFile(), "user defaults: written after picking a key in the editor");

        // Host automation / a validator moving the key while the editor is open must not become the default.
        setParam (p, id::keyRoot, 3.0f);
        setParam (p, id::scale, 6.0f);
        pumpMessages (500);
    }
    {
        ChordbloomProcessor fresh;
        check (fresh.getKey().tonic == 6 && fresh.getKey().scale == cb::ScaleType::NaturalMinor && getParam (fresh, id::previewTone) == 1.0f,
               "user defaults: a new instance starts in F# minor with Warm Pad",
               str (fresh.getKey().tonic, " ", (int) fresh.getKey().scale, " tone ", getParam (fresh, id::previewTone)));
        check (fresh.getPalette().size() == (size_t) cb::numPaletteSections && ! fresh.getPalette()[0].chords.empty()
                   && fresh.getPalette()[0].chords[0].chord.root == 6,
               "user defaults: the palette is built for that key");
    }

    juce::MemoryBlock cMajorState;
    {
        ChordbloomProcessor src;   // starts in F# minor; set to C major without an editor (not recorded)
        setParam (src, id::keyRoot, 0.0f);
        setParam (src, id::scale, 0.0f);
        setParam (src, id::previewTone, 0.0f);
        pumpMessages (400);
        src.getStateInformation (cMajorState);
    }
    {
        ChordbloomProcessor q;
        std::unique_ptr<juce::AudioProcessorEditor> editor (q.createEditorAndMakeActive());
        q.setStateInformation (cMajorState.getData(), (int) cMajorState.getSize());
        pumpMessages (500);
        check (q.getKey().tonic == 0 && q.getKey().scale == cb::ScaleType::Major, "user defaults: a loaded project state wins");
    }
    {
        ChordbloomProcessor fresh;
        check (fresh.getKey().tonic == 6 && fresh.getKey().scale == cb::ScaleType::NaturalMinor,
               "user defaults: neither a state load nor changes without the editor overwrite them");
    }

    ChordbloomProcessor::setUserDefaultsFile ({});   // back to disabled (dev tool default)
    file.deleteFile();
    ChordbloomProcessor plain;
    check (plain.getKey().tonic == 0, "user defaults: disabled -> factory defaults");
}

// F1 + F4: timed slot auditions belong to the editor (closing it stops them; their stop never cuts a newer audition),
// and scroll-to-resize steps one beat per notch-worth of travel with one undo step per gesture.
void testEditorAuditionsAndWheel()
{
    const auto left = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier);
    const auto none = juce::ModifierKeys();

    ChordbloomProcessor p;
    p.setProgression (makeProgression ({ C_, G_ }));

    auto findProxy = [] (juce::Component& laneSurface, const juce::String& prefix) -> juce::Component*
    {
        for (auto* c : laneSurface.getChildren())
            if (c->getTitle().startsWith (prefix) && dynamic_cast<juce::Button*> (c) == nullptr)
                return c;
        return nullptr;
    };
    auto press = [] (juce::Component* proxy)
    {
        if (proxy == nullptr)
            return false;
        auto handler = proxy->createAccessibilityHandler();
        return handler != nullptr && handler->getActions().invoke (juce::AccessibilityActionType::press);
    };

    // (a) Closing the editor right after a timed audition releases it.
    {
        std::unique_ptr<juce::AudioProcessorEditor> holder (p.createEditor());
        auto* ed = dynamic_cast<ChordbloomEditor*> (holder.get());
        if (ed == nullptr)
        {
            check (false, "audition: editor");
            return;
        }
        pumpMessages (50);
        ed->updateChordDisplays();
        auto* proxy = findProxy (*ed->getProgressionLane().getChildComponent (0), "C (");
        check (press (proxy) && ! p.getPreviewNotes().empty(), "audition: the block's accessibility press plays it");
        holder.reset();
        check (p.getPreviewNotes().empty() && ! p.getPreviewChord().has_value(),
               "audition: closing the editor stops a timed audition at once (F1)");
        pumpMessages (900);   // the orphaned stop timer fires into nothing
        Rig rig (p);
        rig.run (4);
        check (rig.openCount() == 0 && p.getSoundingNotes().none(), "audition: nothing left sounding after the editor closed",
               str (rig.openCount(), " open"));
    }

    std::unique_ptr<juce::AudioProcessorEditor> holder (p.createEditor());
    auto* ed = dynamic_cast<ChordbloomEditor*> (holder.get());
    if (ed == nullptr)
        return;
    pumpMessages (50);
    ed->updateChordDisplays();
    auto* laneSurface = ed->getProgressionLane().getChildComponent (0);

    // (b) A timed audition stops by itself, but never cuts a pad held after it.
    {
        check (press (findProxy (*laneSurface, "C (")), "audition: press");
        pumpMessages (950);
        check (p.getPreviewNotes().empty(), "audition: a timed audition stops by itself after ~0.7 s");

        auto* padSurface = ed->getPaletteView().getChildComponent (0);
        juce::Point<float> padPos;
        for (float x = 20.0f; x < (float) padSurface->getWidth() && padPos.isOrigin(); x += 4.0f)
        {
            const juce::Point<float> pos (x, 30.0f);
            padSurface->mouseDown (mouseEventFor (*padSurface, pos, left));
            if (p.getPreviewChord().has_value())
                padPos = pos;
            padSurface->mouseUp (mouseEventFor (*padSurface, pos, none));
        }

        check (press (findProxy (*laneSurface, "G (")), "audition: press G");
        padSurface->mouseDown (mouseEventFor (*padSurface, padPos, left));   // hold a pad within the 0.7 s
        const auto held = p.getPreviewChord();
        pumpMessages (950);
        check (held.has_value() && p.getPreviewChord() == held && ! p.getPreviewNotes().empty(),
               "audition: the timed stop does not cut a pad held after it (F1)");
        padSurface->mouseUp (mouseEventFor (*padSurface, padPos, none));
        check (p.getPreviewNotes().empty(), "audition: releasing the pad stops it");
    }

    // (c) Scroll-to-resize.
    {
        juce::Point<float> blockPos;
        for (float y = (float) laneSurface->getHeight() - 12.0f; y > 30.0f && blockPos.isOrigin(); y -= 6.0f)
        {
            const juce::Point<float> pos (40.0f, y);
            laneSurface->mouseDown (mouseEventFor (*laneSurface, pos, left));
            if (p.getPreviewChord().has_value())
                blockPos = pos;
            laneSurface->mouseUp (mouseEventFor (*laneSurface, pos, none));
        }
        check (! blockPos.isOrigin(), "wheel: found the first block");

        auto wheel = [&] (float dy)
        {
            juce::MouseWheelDetails w;
            w.deltaX = 0.0f;
            w.deltaY = dy;
            w.isReversed = false;
            w.isSmooth = true;
            w.isInertial = false;
            laneSurface->mouseWheelMove (mouseEventFor (*laneSurface, blockPos, none), w);
        };
        auto length0 = [&p] { return p.getProgression().slots.empty() ? -1 : p.getProgression().slots[0].lengthBeats; };

        const int before = length0();
        for (int i = 0; i < 25; ++i)
            wheel (0.03f);   // a touchpad: 25 small deltas, about three notches of travel
        check (length0() == before, "wheel: the gesture is previewed, not committed per step");
        pumpMessages (600);
        check (length0() == before + 3, "wheel: three notches of touchpad travel = three beats (was eight)",
               str (before, " -> ", length0()));
        p.undo();
        check (length0() == before, "wheel: the whole gesture is one undo step", str (length0()));
        pumpMessages (50);

        wheel (0.234f);   // one classic notch
        pumpMessages (600);
        check (length0() == before + 1, "wheel: one notch = one beat", str (length0()));
        p.undo();
        pumpMessages (50);

        wheel (0.15f);   // direction change: leftovers never carry over
        wheel (-0.15f);
        wheel (0.15f);
        pumpMessages (600);
        check (length0() == before, "wheel: back-and-forth jitter does not resize", str (length0()));

        wheel (0.15f);   // a pause restarts the accumulation
        pumpMessages (500);
        wheel (0.15f);
        pumpMessages (600);
        check (length0() == before, "wheel: leftovers do not carry into the next gesture", str (length0()));
    }

    holder.reset();
    Rig rig (p);
    rig.run (4);
    check (rig.openCount() == 0 && p.getSoundingNotes().none(), "audition/wheel: nothing left sounding", str (rig.openCount(), " open"));
}
} // namespace

namespace
{
//==============================================================================
// WP0 (contract review) hooks: the MIDI out part chip label / menu order, the Controls bass state text and the
// user-pick order (parameter first, then beginUserLayerPick, which switches the bass on).
void testWp0ShellHooks()
{
    ChordbloomProcessor p;
    pumpMessages (30);

    HeaderBar header (p);
    setParam (p, cb::ParamIDs::midiOutPart, 5.0f);
    pumpMessages (40);
    check (header.getOutPartLabel() == "Arp", "header: the out-part chip shows \"Arp\" for choice 5",
           header.getOutPartLabel().toStdString());
    check (header.getOutPartMenuOrder() == std::vector<int> { 0, 4, 1, 2, 3, 5 },
           "header: the out-part menu order is { 0, 4, 1, 2, 3, 5 }");

    ControlsPanel controls (p);
    check (controls.getBassStateText().isEmpty(), "controls: bass state text empty for an audible auto bass");
    setParam (p, cb::ParamIDs::bassOn, 0.0f);
    pumpMessages (40);
    check (controls.getBassStateText() == "muted", "controls: bass state text \"muted\" while bassOn is off",
           controls.getBassStateText().toStdString());

    controls.pickBassModeAsUser (cb::BassMode::Offbeat8);
    pumpMessages (40);
    check (juce::roundToInt (getParam (p, cb::ParamIDs::bassMode)) == (int) cb::BassMode::Offbeat8
               && getParam (p, cb::ParamIDs::bassOn) >= 0.5f,
           "controls: a user pick sets bassMode and then switches the bass on");

    setParam (p, cb::ParamIDs::bassOn, 0.0f);
    pumpMessages (40);
    controls.pickBassModeAsUser (cb::BassMode::Off);
    pumpMessages (40);
    check (juce::roundToInt (getParam (p, cb::ParamIDs::bassMode)) == 0 && getParam (p, cb::ParamIDs::bassOn) < 0.5f,
           "controls: a user pick of Off only sets Off (bassOn untouched)");
}
} // namespace

void cbtest::runShellSelfTests()
{
    testWp0ShellHooks();
    testEditorSize();
    testEditorInteraction();
    testUserDefaults();
    testEditorAuditionsAndWheel();
}
