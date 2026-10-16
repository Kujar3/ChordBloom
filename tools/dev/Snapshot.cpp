#include "plugin/PluginProcessor.h"
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
    juce::Image image (juce::Image::ARGB, 720, 400, true);
    juce::Graphics g (image);
    g.fillAll (cb::ui::Colours::background);

    struct Item
    {
        juce::String text;
        enum Kind { normal, heading, separator } kind = normal;
        bool ticked = false, highlighted = false, subMenu = false, enabled = true;
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
                lf.getIdealPopupMenuItemSize (item.text, item.kind == Item::separator, 24, w, h);

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
                                      item.highlighted, item.ticked, item.subMenu, item.text, {}, nullptr, nullptr);
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

    {
        const juce::String tip ("Voice leading: picks inversions automatically so each chord moves smoothly to the next");
        const auto bounds = lf.getTooltipBounds (tip, { 0, 0 }, { 0, 0, 1000, 1000 });
        juce::Graphics::ScopedSaveState state (g);
        g.setOrigin ({ 420, 24 });
        lf.drawTooltip (g, tip, bounds.getWidth(), bounds.getHeight());
    }

    {
        const juce::String tip ("Undo the last progression edit");
        const auto bounds = lf.getTooltipBounds (tip, { 0, 0 }, { 0, 0, 1000, 1000 });
        juce::Graphics::ScopedSaveState state (g);
        g.setOrigin ({ 420, 120 });
        lf.drawTooltip (g, tip, bounds.getWidth(), bounds.getHeight());
    }

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

    // 1. Default size, empty progression (C Major)
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

    // 4. Minimum size
    editor->setSize (ChordbloomEditor::kMinWidth, ChordbloomEditor::kMinHeight);
    settle (processor, *editor);
    ok &= writePng (*editor, outputDir.getChildFile ("04_min_960x600.png"));

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
    editor->setSize (ChordbloomEditor::kDefaultWidth, ChordbloomEditor::kDefaultHeight);
    settle (processor, *editor);
    ok &= writePng (*editor, outputDir.getChildFile ("06_default_settings_changed.png"));

    // 7. Popup menu / tooltip styling
    ok &= writeImage (renderWidgetSheet (editor->getLookAndFeel()), outputDir.getChildFile ("07_widgets.png"));

    editorHolder = nullptr;
    pumpMessages (20);
    return ok ? 0 : 1;
}
