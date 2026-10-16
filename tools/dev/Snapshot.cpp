#include "plugin/PluginProcessor.h"
#include "ui/DragPayload.h"
#include "ui/HeaderBar.h"
#include "ui/PluginEditor.h"
#include "ui/ProgressionLane.h"

#include <iostream>

// Renders the editor in several states to PNG files (owner: ui-shell agent).
//   ChordbloomDev snapshot <outDir>

namespace juce::detail
{
// Defined in juce_events (juce_Messaging_windows.cpp / _linux.cpp); lets this console tool pump the message queue so
// timers, async updaters and change messages run as they would inside a host.
bool dispatchNextMessageOnSystemQueue (bool returnIfNoPendingMessages);
}

namespace
{
void pumpMessages (int milliseconds)
{
   #if JUCE_WINDOWS || JUCE_LINUX
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) milliseconds;

    while (juce::Time::getMillisecondCounter() < end)
        if (! juce::detail::dispatchNextMessageOnSystemQueue (true))
            juce::Thread::sleep (2);
   #else
    juce::ignoreUnused (milliseconds);
   #endif
}

bool writePng (juce::Component& c, const juce::File& file)
{
    auto image = c.createComponentSnapshot (c.getLocalBounds(), true, 1.0f);
    file.deleteFile();
    juce::FileOutputStream out (file);
    juce::PNGImageFormat png;
    const bool ok = out.openedOk() && png.writeImageToStream (image, out);
    std::cout << (ok ? "wrote " : "FAILED ") << file.getFullPathName() << " (" << c.getWidth() << "x" << c.getHeight() << ")\n";
    return ok;
}

void setParam (ChordbloomProcessor& processor, const char* id, float denormalisedValue)
{
    if (auto* p = processor.getAPVTS().getParameter (id))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (denormalisedValue));
        p->endChangeGesture();
    }
}

cb::Progression makeProgression (std::initializer_list<std::pair<cb::Chord, int>> chords)
{
    cb::Progression prog;

    for (const auto& [chord, beats] : chords)
    {
        cb::ProgressionSlot slot;
        slot.chord = chord;
        slot.lengthBeats = beats;
        prog.slots.push_back (slot);
    }

    return prog;
}

// Draws look-and-feel pieces that only exist in popup windows (menus, tooltips, value bubbles) onto one sheet.
juce::Image renderWidgetSheet (juce::LookAndFeel& lf)
{
    juce::Image image (juce::Image::ARGB, 900, 420, true);
    juce::Graphics g (image);
    g.fillAll (cb::ui::Colours::background);

    struct Item
    {
        juce::String text;
        enum Kind { normal, heading, separator } kind = normal;
        bool ticked = false, highlighted = false, subMenu = false, enabled = true;
        juce::String shortcut {};
    };

    auto drawMenu = [&] (juce::Point<int> origin, const std::vector<Item>& items)
    {
        const int border = lf.getPopupMenuBorderSize();
        int width = 160, height = 2 * border;

        for (const auto& item : items)
        {
            int w = 0, h = 0;

            if (item.kind == Item::heading)
                lf.getIdealPopupMenuSectionHeaderSizeWithOptions (item.text, 24, w, h, juce::PopupMenu::Options());
            else
                lf.getIdealPopupMenuItemSize (item.shortcut.isEmpty() ? item.text : item.text + "   " + item.shortcut,
                                              item.kind == Item::separator, 24, w, h);

            width = juce::jmax (width, w + 2 * border);
            height += h;
        }

        juce::Graphics::ScopedSaveState state (g);
        g.setOrigin (origin);
        lf.drawPopupMenuBackground (g, width, height);

        int y = border;

        for (const auto& item : items)
        {
            int w = 0, h = 0;

            if (item.kind == Item::heading)
            {
                lf.getIdealPopupMenuSectionHeaderSizeWithOptions (item.text, 24, w, h, juce::PopupMenu::Options());
                lf.drawPopupMenuSectionHeader (g, { border, y, width - 2 * border, h }, item.text);
            }
            else
            {
                lf.getIdealPopupMenuItemSize (item.text, item.kind == Item::separator, 24, w, h);
                lf.drawPopupMenuItem (g, { border, y, width - 2 * border, h }, item.kind == Item::separator, item.enabled,
                                      item.highlighted, item.ticked, item.subMenu, item.text, item.shortcut, nullptr, nullptr);
            }

            y += h;
        }
    };

    drawMenu ({ 20, 20 }, { { "Major" }, { "Minor", Item::heading }, { "Natural Minor", Item::normal, true },
                            { "Harmonic Minor", Item::normal, false, true }, { "Melodic Minor" },
                            { "Modes", Item::heading }, { "Dorian" }, { "Phrygian" }, { "Lydian" }, { "Mixolydian" },
                            { "Locrian" } });

    drawMenu ({ 220, 20 }, { { "Length", Item::normal, false, true, true }, { "Inversion", Item::normal, false, false, true },
                             { "Octave", Item::normal, false, false, true }, { {}, Item::separator },
                             { "Duplicate" }, { "Delete" }, { "Disabled item", Item::normal, false, false, false, false } });

    // Key menu: both enharmonic spellings (I27)
    drawMenu ({ 400, 20 }, { { "C" }, { "C# / Db" }, { "D" }, { "D# / Eb", Item::normal, true, true }, { "E" }, { "F" },
                             { "F# / Gb" }, { "G" }, { "G# / Ab" }, { "A" }, { "A# / Bb" }, { "B" } });

    // MIDI out part menu (I13)
    {
        Item heading { "MIDI out sends", Item::heading };
        Item both { "Chords + Bass", Item::normal, true };
        both.shortcut = "ch 1 + 2";
        Item chords { "Chords only", Item::normal, false, true };
        chords.shortcut = "ch 1";
        Item bass { "Bass only" };
        bass.shortcut = "ch 2";
        drawMenu ({ 580, 20 }, { heading, both, chords, bass });
    }

    auto drawTip = [&] (const juce::String& tip, juce::Point<int> origin)
    {
        const auto bounds = lf.getTooltipBounds (tip, { 0, 0 }, { 0, 0, 1000, 1000 });
        juce::Graphics::ScopedSaveState state (g);
        g.setOrigin (origin);
        lf.drawTooltip (g, tip, bounds.getWidth(), bounds.getHeight());
    };

    drawTip ("Voice leading: picks inversions automatically so each chord moves smoothly to the next", { 580, 180 });
    drawTip ("Swing: delays every second step for a shuffled groove (double-click to reset)\n\nNo effect right now: Block "
             "holds each chord and the bass is held too. Try Pulse, an Arp or Broken, or the Octaves / Pulse bass.",
             { 580, 260 });

    return image;
}

bool writeImage (const juce::Image& image, const juce::File& file)
{
    file.deleteFile();
    juce::FileOutputStream out (file);
    juce::PNGImageFormat png;
    const bool ok = out.openedOk() && png.writeImageToStream (image, out);
    std::cout << (ok ? "wrote " : "FAILED ") << file.getFullPathName() << "\n";
    return ok;
}

// Renders a (possibly transparent) component onto the editor background with a margin, at `scale`.
juce::Image snapshotOnBackground (juce::Component& c, int margin, float scale = 1.0f)
{
    const auto shot = c.createComponentSnapshot (c.getLocalBounds(), true, scale);
    juce::Image image (juce::Image::ARGB, shot.getWidth() + 2 * margin, shot.getHeight() + 2 * margin, true);
    juce::Graphics g (image);
    g.fillAll (cb::ui::Colours::background);
    g.drawImageAt (shot, margin, margin);
    return image;
}

juce::Component* findByTitle (juce::Component& root, const juce::String& title)
{
    if (root.getTitle() == title)
        return &root;

    for (auto* child : root.getChildren())
        if (auto* found = findByTitle (*child, title))
            return found;

    return nullptr;
}

juce::MouseEvent syntheticMouseEvent (juce::Component& c, juce::Point<float> pos, juce::ModifierKeys mods)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos, mods,
                             juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, pos, now, 1, false);
}

float paramValue (ChordbloomProcessor& processor, const char* id)
{
    return processor.getAPVTS().getRawParameterValue (id)->load();
}

bool check (bool condition, const juce::String& what)
{
    std::cout << (condition ? "PASS " : "FAIL ") << what << "\n";
    return condition;
}

// Lets the processor and editor settle (change messages, parameter listeners, timers) before a snapshot.
void settle (ChordbloomProcessor& processor, ChordbloomEditor& editor)
{
    processor.dispatchPendingMessages();
    pumpMessages (120);
    processor.dispatchPendingMessages();
    editor.updateChordDisplays();
    pumpMessages (40);
}
} // namespace

int runSnapshot (const juce::File& outputDir)
{
    using Q = cb::ChordQuality;

    outputDir.createDirectory();
    bool ok = true;

    ChordbloomProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editorHolder (processor.createEditor());
    auto* editor = dynamic_cast<ChordbloomEditor*> (editorHolder.get());

    if (editor == nullptr)
    {
        std::cout << "FAILED: createEditor() did not return a ChordbloomEditor\n";
        return 1;
    }

    // 1. Default size, empty progression (C Major): the lane's empty state / opening glow, Rate + Swing dimmed (I12)
    editor->setSize (ChordbloomEditor::kDefaultWidth, ChordbloomEditor::kDefaultHeight);
    settle (processor, *editor);
    ok &= writePng (*editor, outputDir.getChildFile ("01_default_empty.png"));

    // 2. C - G - Am - F with slot 1 (G) selected
    processor.setProgression (makeProgression ({ { { 0, Q::Major }, 4 }, { { 7, Q::Major }, 4 },
                                                 { { 9, Q::Minor }, 4 }, { { 5, Q::Major }, 4 } }));
    settle (processor, *editor);
    editor->getProgressionLane().setSelectedSlot (1);
    settle (processor, *editor);
    ok &= writePng (*editor, outputDir.getChildFile ("02_default_progression_selected.png"));

    // 2b. Header close-up (2x): key, scale, sound, MIDI out + part chip, Play with FL, undo / redo, help
    {
        const auto header = editor->getLocalBounds().withHeight (56);
        ok &= writeImage (editor->createComponentSnapshot (header.withTrimmedLeft (header.getWidth() / 2), true, 2.0f),
                          outputDir.getChildFile ("02b_header_right_2x.png"));
        ok &= writeImage (editor->createComponentSnapshot (header.withWidth (header.getWidth() / 2), true, 2.0f),
                          outputDir.getChildFile ("02c_header_left_2x.png"));
    }

    // 3. A Natural Minor with a 6-chord progression: Am - F - C - G - Dm - E7 (i VI III VII iv V7)
    setParam (processor, cb::ParamIDs::keyRoot, 9.0f);
    setParam (processor, cb::ParamIDs::scale, (float) cb::ScaleType::NaturalMinor);
    settle (processor, *editor);
    processor.setProgression (makeProgression ({ { { 9, Q::Minor }, 4 }, { { 5, Q::Major }, 4 },
                                                 { { 0, Q::Major }, 4 }, { { 7, Q::Major }, 4 },
                                                 { { 2, Q::Minor7 }, 2 }, { { 4, Q::Dominant7 }, 2 } }));
    settle (processor, *editor);
    editor->getProgressionLane().setSelectedSlot (5);
    settle (processor, *editor);
    ok &= writePng (*editor, outputDir.getChildFile ("03_a_minor_six_chords.png"));

    // 3b. F minor with extended chords: Fm9 - Dbmaj7 - Ab(add9) - Eb6 - Bbm11 - C7b9, last slot selected
    setParam (processor, cb::ParamIDs::keyRoot, 5.0f);
    settle (processor, *editor);
    processor.setProgression (makeProgression ({ { { 5, Q::Minor9 }, 4 }, { { 1, Q::Major7 }, 4 },
                                                 { { 8, Q::Add9 }, 4 }, { { 3, Q::Major6 }, 4 },
                                                 { { 10, Q::Minor11 }, 2 }, { { 0, Q::Dominant7Flat9 }, 2 } }));
    settle (processor, *editor);
    editor->getProgressionLane().setSelectedSlot (5);
    settle (processor, *editor);
    ok &= writePng (*editor, outputDir.getChildFile ("03b_f_minor_extended.png"));

    // back to A minor for the size checks
    setParam (processor, cb::ParamIDs::keyRoot, 9.0f);
    processor.setProgression (makeProgression ({ { { 9, Q::Minor }, 4 }, { { 5, Q::Major }, 4 },
                                                 { { 0, Q::Major }, 4 }, { { 7, Q::Major }, 4 },
                                                 { { 2, Q::Minor7 }, 2 }, { { 4, Q::Dominant7 }, 2 } }));
    settle (processor, *editor);
    editor->getProgressionLane().setSelectedSlot (5);

    // 4. Minimum size (960x560) and the previous minimum (960x600)
    editor->setSize (ChordbloomEditor::kMinWidth, ChordbloomEditor::kMinHeight);
    settle (processor, *editor);
    ok &= writePng (*editor, outputDir.getChildFile ("04_min_960x560.png"));

    editor->setSize (960, 600);
    settle (processor, *editor);
    ok &= writePng (*editor, outputDir.getChildFile ("04b_960x600.png"));

    // 5. Maximum size
    editor->setSize (ChordbloomEditor::kMaxWidth, ChordbloomEditor::kMaxHeight);
    settle (processor, *editor);
    ok &= writePng (*editor, outputDir.getChildFile ("05_max_1600x1000.png"));

    // 6. Default size, other settings (pattern / voicing / toggles) to check the controls in non-default states
    setParam (processor, cb::ParamIDs::pattern, 4.0f);
    setParam (processor, cb::ParamIDs::rate, 2.0f);
    setParam (processor, cb::ParamIDs::swing, 0.4f);
    setParam (processor, cb::ParamIDs::voicing, 2.0f);
    setParam (processor, cb::ParamIDs::octave, -1.0f);
    setParam (processor, cb::ParamIDs::chordTrigger, 1.0f);
    setParam (processor, cb::ParamIDs::previewOn, 0.0f);
    setParam (processor, cb::ParamIDs::midiOutPart, 2.0f);   // Bass only
    setParam (processor, cb::ParamIDs::syncToHost, 0.0f);    // Play with FL off
    editor->setSize (ChordbloomEditor::kDefaultWidth, ChordbloomEditor::kDefaultHeight);
    settle (processor, *editor);
    ok &= writePng (*editor, outputDir.getChildFile ("06_default_settings_changed.png"));

    // 6b. Same, MIDI out off (part chip dimmed), Offbeat at 1/16 (Rate dimmed, Swing dimmed)
    setParam (processor, cb::ParamIDs::midiOut, 0.0f);
    setParam (processor, cb::ParamIDs::pattern, 3.0f);
    setParam (processor, cb::ParamIDs::bassMode, 1.0f);
    settle (processor, *editor);
    ok &= writeImage (editor->createComponentSnapshot (editor->getLocalBounds().withTrimmedTop (editor->getHeight() - 210), true, 1.0f),
                      outputDir.getChildFile ("06b_offbeat_midi_off_bottom.png"));
    ok &= writeImage (editor->createComponentSnapshot (editor->getLocalBounds().withHeight (56).withTrimmedLeft (editor->getWidth() / 2), true, 2.0f),
                      outputDir.getChildFile ("06c_header_midi_off_2x.png"));

    // 7. Popup menu / tooltip styling
    ok &= writeImage (renderWidgetSheet (editor->getLookAndFeel()), outputDir.getChildFile ("07_widgets.png"));

    // 8. Help card on its own (I11)
    {
        auto card = HeaderBar::createHelpContent();
        card->setLookAndFeel (&editor->getLookAndFeel());
        std::cout << "help card ideal size " << card->getWidth() << "x" << card->getHeight() << "\n";
        ok &= writeImage (snapshotOnBackground (*card, 24), outputDir.getChildFile ("08_help_card.png"));
        card->setLookAndFeel (nullptr);
    }

    // 9. Help popover open over the editor, default and minimum size
    editor->setHelpVisible (true);
    settle (processor, *editor);
    ok &= writePng (*editor, outputDir.getChildFile ("09_help_open_1100x690.png"));
    ok &= writeImage (editor->createComponentSnapshot ({ editor->getWidth() - 360, 0, 360, 130 }, true, 2.0f),
                      outputDir.getChildFile ("09b_help_arrow_2x.png"));
    editor->setSize (ChordbloomEditor::kMinWidth, ChordbloomEditor::kMinHeight);
    settle (processor, *editor);
    ok &= writePng (*editor, outputDir.getChildFile ("10_help_open_960x560.png"));
    editor->setHelpVisible (false);
    settle (processor, *editor);

    // 10b. G major walking slash bass (B12 spelling: D/F#, B7/D#) with the slot menu context, then the same chords
    //      after switching the key to F# minor: the "Move chords to F# minor" pill (I10).
    editor->setSize (ChordbloomEditor::kDefaultWidth, ChordbloomEditor::kDefaultHeight);
    setParam (processor, cb::ParamIDs::keyRoot, 7.0f);
    setParam (processor, cb::ParamIDs::scale, (float) cb::ScaleType::Major);
    setParam (processor, cb::ParamIDs::pattern, 0.0f);
    setParam (processor, cb::ParamIDs::chordTrigger, 0.0f);
    setParam (processor, cb::ParamIDs::previewOn, 1.0f);
    setParam (processor, cb::ParamIDs::midiOut, 1.0f);
    setParam (processor, cb::ParamIDs::midiOutPart, 0.0f);
    setParam (processor, cb::ParamIDs::syncToHost, 1.0f);
    settle (processor, *editor);
    {
        auto prog = makeProgression ({ { { 7, Q::Major }, 4 }, { { 2, Q::Major }, 4 }, { { 4, Q::Minor }, 4 },
                                       { { 11, Q::Dominant7 }, 4 }, { { 0, Q::Major }, 4 } });
        prog.slots[1].inversion = 1;   // D/F#
        prog.slots[3].inversion = 1;   // B7/D#
        processor.setProgression (prog);
    }
    settle (processor, *editor);
    editor->getProgressionLane().setSelectedSlot (3);
    settle (processor, *editor);
    ok &= writePng (*editor, outputDir.getChildFile ("10b_g_major_slash_chords.png"));

    setParam (processor, cb::ParamIDs::keyRoot, 6.0f);
    setParam (processor, cb::ParamIDs::scale, (float) cb::ScaleType::NaturalMinor);
    processor.setProgression (makeProgression ({ { { 0, Q::Major }, 4 }, { { 7, Q::Major }, 4 },
                                                 { { 9, Q::Minor }, 4 }, { { 5, Q::Major }, 4 } }));
    settle (processor, *editor);
    ok &= writeImage (editor->createComponentSnapshot (editor->getProgressionLane().getBounds().expanded (4), true, 1.0f),
                      outputDir.getChildFile ("10c_lane_move_to_key.png"));

    // 10d. Following FL in pattern mode with a 1-bar loop under a 4-bar progression (B4 hint + "Playing with FL" pill)
    {
        struct FakePlayHead final : public juce::AudioPlayHead
        {
            juce::Optional<PositionInfo> getPosition() const override { return info; }
            PositionInfo info;
        } head;

        constexpr double sr = 48000.0;
        constexpr int block = 480;
        const double beatsPerBlock = block / sr * 2.0;   // 120 BPM
        juce::AudioBuffer<float> audio (2, block);
        juce::MidiBuffer midi;

        setParam (processor, cb::ParamIDs::keyRoot, 9.0f);
        setParam (processor, cb::ParamIDs::previewOn, 0.0f);
        processor.setProgression (makeProgression ({ { { 9, Q::Minor }, 4 }, { { 5, Q::Major }, 4 },
                                                     { { 0, Q::Major }, 4 }, { { 7, Q::Major }, 4 } }));
        settle (processor, *editor);
        processor.setPlayHead (&head);
        processor.prepareToPlay (sr, block);

        double ppq = 0.0;
        head.info.setBpm (120.0);
        head.info.setIsLooping (true);
        head.info.setLoopPoints (juce::AudioPlayHead::LoopPoints { 0.0, 4.0 });

        for (int i = 0; i < 300; ++i)   // 6 beats of host time: FL's 1-bar loop wraps once, the progression goes on
        {
            head.info.setIsPlaying (true);
            head.info.setPpqPosition (ppq);
            midi.clear();
            processor.processBlock (audio, midi);
            ppq += beatsPerBlock;
            if (ppq >= 4.0 - 1.0e-9)
                ppq -= 4.0;
        }

        settle (processor, *editor);
        ok &= check (std::abs (processor.getHostLoopBeats() - 4.0) < 1.0e-6,
                     "host loop: 1-bar FL loop detected under a 4-bar progression ("
                         + juce::String (processor.getHostLoopBeats()) + " beats)");
        ok &= check (processor.getPlayingSlot() == 1, "host loop: the progression kept going into slot 2 (got slot "
                                                          + juce::String (processor.getPlayingSlot()) + ")");
        ok &= writeImage (editor->createComponentSnapshot (editor->getProgressionLane().getBounds().expanded (4), true, 1.0f),
                          outputDir.getChildFile ("10d_lane_following_fl_1bar_loop.png"));

        head.info.setIsPlaying (false);
        midi.clear();
        processor.processBlock (audio, midi);
        processor.releaseResources();
        processor.setPlayHead (nullptr);
        setParam (processor, cb::ParamIDs::previewOn, 1.0f);
        settle (processor, *editor);
    }

    // 10e. The longest pad names (Gbmaj7#11 / VImaj7#11, Bbm7b5 ...) at the minimum size: Bb natural minor, 960x560
    setParam (processor, cb::ParamIDs::keyRoot, 10.0f);
    setParam (processor, cb::ParamIDs::scale, (float) cb::ScaleType::NaturalMinor);
    processor.setProgression (makeProgression ({ { { 10, Q::Minor11 }, 4 }, { { 6, Q::Major7Sharp11 }, 4 },
                                                 { { 1, Q::Major69 }, 4 }, { { 5, Q::Dominant7Flat9 }, 4 } }));
    editor->setSize (ChordbloomEditor::kMinWidth, ChordbloomEditor::kMinHeight);
    settle (processor, *editor);
    editor->getProgressionLane().setSelectedSlot (1);
    settle (processor, *editor);
    ok &= writePng (*editor, outputDir.getChildFile ("10e_bb_minor_long_names_960x560.png"));
    editor->setSize (ChordbloomEditor::kDefaultWidth, ChordbloomEditor::kDefaultHeight);
    settle (processor, *editor);

    // 11. Keyboard focus (B10): no component of the editor may take keyboard focus, so FL's spacebar keeps working.
    {
        int focusable = 0;
        std::function<void (juce::Component&)> visit = [&] (juce::Component& c)
        {
            if (c.getWantsKeyboardFocus() && dynamic_cast<juce::TextEditor*> (&c) == nullptr)
            {
                ++focusable;
                std::cout << "  wants focus: " << c.getName() << " / " << c.getTitle() << " (" << typeid (c).name() << ")\n";
            }

            for (auto* child : c.getChildren())
                visit (*child);
        };
        visit (*editor);
        std::cout << (focusable == 0 ? "focus check OK: no component wants keyboard focus\n"
                                     : "FAILED focus check: components want keyboard focus\n");
        ok &= focusable == 0;
    }

    // 12. Interaction checks on the real widgets (synthetic events, no window)
    {
        const auto left = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier);
        const auto right = juce::ModifierKeys (juce::ModifierKeys::rightButtonModifier);

        // B22: octave stepper wheel accumulation
        if (auto* octave = findByTitle (*editor, "Octave"))
        {
            setParam (processor, cb::ParamIDs::octave, 0.0f);
            const auto ev = syntheticMouseEvent (*octave, octave->getLocalBounds().getCentre().toFloat(), {});

            juce::MouseWheelDetails smooth {};
            smooth.deltaY = 0.03f;   // a touchpad / smooth-scroll event

            for (int i = 0; i < 10; ++i)
                octave->mouseWheelMove (ev, smooth);

            ok &= check (juce::roundToInt (paramValue (processor, cb::ParamIDs::octave)) == 1,
                         "octave: ten small wheel events move one octave (got "
                             + juce::String (paramValue (processor, cb::ParamIDs::octave)) + ")");

            octave->mouseExit (ev);
            juce::MouseWheelDetails notch {};
            notch.deltaY = -0.234f;   // one 120-unit notch of a classic mouse wheel

            for (int i = 0; i < 2; ++i)
                octave->mouseWheelMove (ev, notch);

            ok &= check (juce::roundToInt (paramValue (processor, cb::ParamIDs::octave)) == -1,
                         "octave: every wheel notch is one octave");

            // B23: right-click on the + zone does nothing, left-click steps
            const auto plus = juce::Point<float> ((float) octave->getWidth() - 4.0f, (float) octave->getHeight() * 0.5f);
            octave->mouseDown (syntheticMouseEvent (*octave, plus, right));
            ok &= check (juce::roundToInt (paramValue (processor, cb::ParamIDs::octave)) == -1, "octave: right-click ignored");
            octave->mouseDown (syntheticMouseEvent (*octave, plus, left));
            ok &= check (juce::roundToInt (paramValue (processor, cb::ParamIDs::octave)) == 0, "octave: left-click steps");
        }
        else
        {
            ok &= check (false, "octave stepper found by its accessible title");
        }

        // B23: right-click on a Rate segment does nothing
        if (auto* rate = findByTitle (*editor, "Rate"))
        {
            setParam (processor, cb::ParamIDs::rate, 1.0f);
            const auto lastSegment = juce::Point<float> ((float) rate->getWidth() - 6.0f, (float) rate->getHeight() * 0.5f);
            rate->mouseDown (syntheticMouseEvent (*rate, lastSegment, right));
            ok &= check (juce::roundToInt (paramValue (processor, cb::ParamIDs::rate)) == 1, "rate: right-click ignored");
            rate->mouseDown (syntheticMouseEvent (*rate, lastSegment, left));
            ok &= check (juce::roundToInt (paramValue (processor, cb::ParamIDs::rate)) == 3, "rate: left-click selects");
            setParam (processor, cb::ParamIDs::rate, 1.0f);
        }
        else
        {
            ok &= check (false, "rate selector found by its accessible title");
        }

        // (Stock JUCE buttons only react to clicks while showing on screen, so cb::ui::LeftClickOnly on the toggles
        // cannot be exercised here without a window.)

        // I3: pad / block drags leaving the window become MIDI files
        {
            juce::StringArray files;
            bool canMove = true;
            const juce::DragAndDropTarget::SourceDetails chordDrag (cb::ui::drag::makeChordPayload ({ 9, Q::Minor7 }),
                                                                    editor, {});
            const bool chordOk = editor->shouldDropFilesWhenDraggedExternally (chordDrag, files, canMove);
            ok &= check (chordOk && files.size() == 1 && juce::File (files[0]).existsAsFile() && ! canMove,
                         "external drag: chord payload -> " + files.joinIntoString (", "));

            files.clear();
            const juce::DragAndDropTarget::SourceDetails slotDrag (cb::ui::drag::makeSlotPayload (0), editor, {});
            const bool slotOk = editor->shouldDropFilesWhenDraggedExternally (slotDrag, files, canMove);
            ok &= check (slotOk && files.size() == 1 && juce::File (files[0]).existsAsFile(),
                         "external drag: slot payload -> " + files.joinIntoString (", "));

            files.clear();
            const juce::DragAndDropTarget::SourceDetails badSlot (cb::ui::drag::makeSlotPayload (99), editor, {});
            ok &= check (! editor->shouldDropFilesWhenDraggedExternally (badSlot, files, canMove) && files.isEmpty(),
                         "external drag: invalid slot -> no file");
        }

        // I18: size fitting
        {
            using E = ChordbloomEditor;
            ok &= check (E::clampSizeToScreen ({ 1100, 690 }, { 0, 0, 1920, 1032 }) == juce::Point<int> (1100, 690),
                         "size: default fits 1080p at 100%");
            const auto at150 = E::clampSizeToScreen ({ 1100, 690 }, { 0, 0, 1280, 688 });
            ok &= check (at150.x == 1100 && at150.y <= 688 - 80 && at150.y >= E::kMinHeight,
                         "size: 1080p at 150% -> " + juce::String (at150.x) + "x" + juce::String (at150.y));
            ok &= check (E::clampSizeToScreen ({ 2000, 1200 }, {}) == juce::Point<int> (E::kMaxWidth, E::kMaxHeight),
                         "size: no screen -> resizable range only");
        }
    }

    editorHolder = nullptr;
    pumpMessages (20);
    return ok ? 0 : 1;
}
