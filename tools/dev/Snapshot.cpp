#include "plugin/PluginProcessor.h"
#include "ui/ControlsPanel.h"
#include "ui/DragPayload.h"
#include "ui/ExportPanel.h"
#include "ui/HeaderBar.h"
#include "ui/MelodyRoll.h"
#include "ui/PluginEditor.h"
#include "ui/ProgressionLane.h"
#include "ui/RollToolbar.h"

#include <iostream>
#include <typeinfo>

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
    juce::Image image (juce::Image::ARGB, 980, 500, true);
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
    drawMenu ({ 480, 20 }, { { "C" }, { "C# / Db" }, { "D" }, { "D# / Eb", Item::normal, true, true }, { "E" }, { "F" },
                             { "F# / Gb" }, { "G" }, { "G# / Ab" }, { "A" }, { "A# / Bb" }, { "B" } });

    // MIDI out part menu (I13; melody and arp parts added in phase 2, menu order {0, 4, 1, 2, 3, 5})
    {
        Item heading { "MIDI out sends", Item::heading };
        Item all { "All parts", Item::normal, true };
        all.shortcut = "ch 1-4";
        Item both { "Chords + Bass" };
        both.shortcut = "ch 1 + 2";
        Item chords { "Chords only", Item::normal, false, true };
        chords.shortcut = "ch 1";
        Item bass { "Bass only" };
        bass.shortcut = "ch 2";
        Item melody { "Melody only" };
        melody.shortcut = "ch 3";
        Item arp { "Arp only" };
        arp.shortcut = "ch 4";
        drawMenu ({ 660, 20 }, { heading, all, both, chords, bass, melody, arp });
    }

    auto drawTip = [&] (const juce::String& tip, juce::Point<int> origin)
    {
        const auto bounds = lf.getTooltipBounds (tip, { 0, 0 }, { 0, 0, 1000, 1000 });
        juce::Graphics::ScopedSaveState state (g);
        g.setOrigin (origin);
        lf.drawTooltip (g, tip, bounds.getWidth(), bounds.getHeight());
    };

    drawTip ("Voice leading: picks inversions automatically so each chord moves smoothly to the next", { 660, 245 });
    drawTip ("Swing: delays every second step for a shuffled groove (double-click to reset)\n\nNo effect right now: Block "
             "holds each chord and the bass is held too. Try Pulse, an Arp or Broken, or the Octaves / Pulse bass.",
             { 660, 315 });

    // Roll menu, layer and view sections (final-spec 7.8; the real menu is built by MelodyRoll::createMenu)
    drawMenu ({ 220, 232 }, { { "Melody  (your lead line)", Item::heading }, { "Play melody", Item::normal, true },
                              { "Clear melody" }, { "View", Item::heading },
                              { "Snap", Item::normal, false, true, true }, { "Grid", Item::normal, false, false, true },
                              { "Velocity lane" }, { "Show other layers and instances", Item::normal, true } });

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

//==============================================================================
// Phase 2 (final-spec 8.4)
juce::Image stackImages (const std::vector<juce::Image>& images, int gap)
{
    int w = 0, h = 0;

    for (const auto& im : images)
    {
        w = std::max (w, im.getWidth());
        h += im.getHeight() + (h > 0 ? gap : 0);
    }

    juce::Image out (juce::Image::ARGB, std::max (1, w), std::max (1, h), true);
    {
        juce::Graphics g (out);   // scoped: the drawing is committed when the context ends
        g.fillAll (cb::ui::Colours::background);
        int y = 0;

        for (const auto& im : images)
        {
            g.drawImageAt (im, 0, y);
            y += im.getHeight() + gap;
        }
    }

    return out;
}

// The toolbar row as it looks in the roll card at 2x: cropped from the editor once MelodyRoll hosts the toolbar (WP5),
// otherwise the toolbar on its own at the width the roll would give it, on the roll card's colour.
juce::Image toolbarStrip (ChordbloomEditor& editor, RollToolbar& toolbar)
{
    if (toolbar.getParentComponent() != nullptr && editor.getMelodyRoll().isVisible())
    {
        const auto row = editor.getLocalArea (&toolbar, toolbar.getLocalBounds())
                             .withRight (editor.getMelodyRoll().getRight() - 4).expanded (8, 6).withLeft (editor.getMelodyRoll().getX());
        return editor.createComponentSnapshot (row, true, 2.0f);
    }

    const auto shot = toolbar.createComponentSnapshot (toolbar.getLocalBounds(), true, 2.0f);
    juce::Image image (juce::Image::ARGB, shot.getWidth() + 40, shot.getHeight() + 24, true);
    {
        juce::Graphics g (image);
        g.fillAll (cb::ui::Colours::panel.brighter (0.02f));
        g.drawImageAt (shot, 20, 12);
    }
    return image;
}

// The width MelodyRoll gives its toolbar: the roll's inner width (12 px padding) up to 12 px left of the view switch.
int toolbarWidthIn (ChordbloomEditor& editor)
{
    const auto& roll = editor.getMelodyRoll();

    if (auto* view = findByTitle (editor, "View"))
        return view->getX() - 12 - (roll.getX() + 12);

    return roll.getWidth() - 24 - 134;
}

// Opens a PopupMenu for real, snapshots its window and closes it again.
juce::Image snapshotMenu (juce::PopupMenu menu, juce::LookAndFeel& lf)
{
    menu.setLookAndFeel (&lf);
    // Snapshot straight away: a console process is never the foreground app, so the menu would close itself on its
    // first timer tick.
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea ({ 200, 200, 10, 10 }), nullptr);
    juce::Image image;
    auto& desktop = juce::Desktop::getInstance();

    for (int i = desktop.getNumComponents(); --i >= 0;)
    {
        auto* c = desktop.getComponent (i);

        if (c != nullptr && juce::String (typeid (*c).name()).contains ("MenuWindow"))
        {
            image = snapshotOnBackground (*c, 10);
            break;
        }
    }

    juce::PopupMenu::dismissAllActiveMenus();
    pumpMessages (80);
    return image;
}

const juce::PopupMenu* subMenuNamed (const juce::PopupMenu& menu, const juce::String& text)
{
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        if (it.getItem().text == text && it.getItem().subMenu != nullptr)
            return it.getItem().subMenu.get();

    return nullptr;
}
} // namespace

// Menu builders of the toolbar (RollToolbar.cpp; not in the frozen header).
namespace cb::ui::rolltoolbar
{
juce::PopupMenu createPatternMenu (ChordbloomProcessor&, cb::Layer);
juce::PopupMenu createGeneratorMenu (ChordbloomProcessor&, const cb::MelodyGenSettings&);
}

namespace
{
bool renderPhase2Snapshots (ChordbloomProcessor& processor, ChordbloomEditor& editor, const juce::File& outputDir)
{
    using Q = cb::ChordQuality;
    bool ok = true;
    auto& roll = editor.getMelodyRoll();
    auto& toolbar = roll.getToolbar();
    const auto defaultSize = [&editor] { editor.setSize (ChordbloomEditor::kDefaultWidth, ChordbloomEditor::kDefaultHeight); };
    const auto minSize = [&editor] { editor.setSize (ChordbloomEditor::kMinWidth, ChordbloomEditor::kMinHeight); };
    const auto shoot = [&] (const char* name) { settle (processor, editor); ok &= writePng (editor, outputDir.getChildFile (name)); };

    auto resetLayers = [&]
    {
        processor.resetLayer (cb::Layer::Arp);
        processor.resetLayer (cb::Layer::Bass);
        setParam (processor, cb::ParamIDs::arpOn, 0.0f);
        setParam (processor, cb::ParamIDs::bassOn, 1.0f);
        setParam (processor, cb::ParamIDs::bassMode, 1.0f);
        setParam (processor, cb::ParamIDs::arpShape, 0.0f);
    };

    // A minor i-VI-III-VII, Uplifting, seed 7
    setParam (processor, cb::ParamIDs::keyRoot, 9.0f);
    setParam (processor, cb::ParamIDs::scale, (float) cb::ScaleType::NaturalMinor);
    setParam (processor, cb::ParamIDs::pattern, 0.0f);
    setParam (processor, cb::ParamIDs::rate, 2.0f);
    setParam (processor, cb::ParamIDs::swing, 0.0f);
    resetLayers();
    processor.setProgression (makeProgression ({ { { 9, Q::Minor }, 4 }, { { 5, Q::Major }, 4 },
                                                 { { 0, Q::Major }, 4 }, { { 7, Q::Major }, 4 } }));
    processor.clearMelody();
    defaultSize();
    editor.setMelodyViewVisible (true);
    roll.setLayer (cb::Layer::Melody);
    settle (processor, editor);

    auto settings = toolbar.getGenerateSettings();
    settings.style = cb::MelodyStyle::Uplifting;
    toolbar.setGenerateSettings (settings);
    const bool generatedOk = toolbar.generateMelody (7);
    ok &= check (generatedOk && ! processor.getMelody().empty(),
                 "generate: Uplifting seed 7 over Am F C G (" + juce::String ((int) processor.getMelody().notes.size()) + " notes)");

    // Centre the rows on the new melody (MelodyRoll does this itself on onMelodyGenerated once it hosts the toolbar).
    if (! processor.getMelody().empty())
    {
        int lo = 127, hi = 0;

        for (const auto& n : processor.getMelody().notes)
        {
            lo = std::min (lo, n.note);
            hi = std::max (hi, n.note);
        }

        settle (processor, editor);
        roll.setTopNote ((lo + hi) / 2 + (roll.getTopNote() - roll.getBottomNote()) / 2);
    }

    settle (processor, editor);
    shoot ("14a_roll_melody_generated_1100x690.png");
    minSize();
    shoot ("14b_roll_melody_generated_960x560.png");

    const auto generated = processor.getMelody();
    processor.clearMelody();
    defaultSize();
    shoot ("14c_roll_melody_empty_generate_card_1100x690.png");
    processor.setMelody (generated);

    // Arp tab: off (pill), then on at the minimum size with the other layers shown.
    roll.setLayer (cb::Layer::Arp);
    shoot ("14d_roll_arp_off_pill_1100x690.png");
    setParam (processor, cb::ParamIDs::arpOn, 1.0f);
    roll.setOtherLayersShown (true);
    minSize();
    shoot ("14e_roll_arp_on_960x560.png");

    // Bass tab: Offbeat 8ths with one edited slot (slot 2 a fifth up, so it stays in view) -> strip dot + Reset pill.
    setParam (processor, cb::ParamIDs::bassMode, (float) cb::BassMode::Offbeat8);
    settle (processor, editor);
    {
        auto bass = processor.getLayerNotes (cb::Layer::Bass);

        for (auto& n : bass.notes)
            if (n.startBeat >= 4.0 && n.startBeat < 8.0)
                n.note = std::min (n.note + 7, 72);

        processor.setLayerNotes (cb::Layer::Bass, bass);
    }
    roll.setLayer (cb::Layer::Bass);
    ok &= check (processor.isLayerCustom (cb::Layer::Bass), "bass: one edited slot makes the layer custom");
    shoot ("14f_roll_bass_edited_reset_960x560.png");

    // Toolbar close-ups (2x): Melody, Arp custom, Bass custom at the 960 and the 1100 widths.
    {
        auto strips = [&]
        {
            std::vector<juce::Image> images;
            const int width = toolbarWidthIn (editor);
            const int height = editor.getHeight() < 600 ? 24 : 26;

            if (toolbar.getParentComponent() == nullptr)
                toolbar.setSize (width, height);

            std::cout << "toolbar width " << width << " x " << height << "\n";

            for (auto layer : { cb::Layer::Melody, cb::Layer::Arp, cb::Layer::Bass })
            {
                if (layer == cb::Layer::Arp && ! processor.isLayerCustom (cb::Layer::Arp))
                {
                    auto arp = processor.getLayerNotes (cb::Layer::Arp);

                    if (arp.notes.size() > 2)
                        arp.notes.erase (arp.notes.begin() + 1);

                    processor.setLayerNotes (cb::Layer::Arp, arp);
                }

                roll.setLayer (layer);
                toolbar.setLayer (layer);
                settle (processor, editor);
                toolbar.refresh();
                std::cout << "  " << cb::layerName (layer) << ": level " << toolbar.getLayoutLevel() << "\n";
                images.push_back (toolbarStrip (editor, toolbar));
            }

            return stackImages (images, 8);
        };

        minSize();
        settle (processor, editor);
        ok &= writeImage (strips(), outputDir.getChildFile ("14i_roll_toolbar_960_2x.png"));
        defaultSize();
        settle (processor, editor);
        ok &= writeImage (strips(), outputDir.getChildFile ("14j_roll_toolbar_1100_2x.png"));
    }

    // Melody tab: a selection with its pill and the velocity lane; then a marquee in progress.
    roll.setLayer (cb::Layer::Melody);
    roll.setVelocityLaneVisible (true);
    roll.setSelection ({ 1, 2, 3 });
    shoot ("14g_roll_selection_pill_velocity_lane_1100x690.png");
    roll.setSelection ({});
    roll.setVelocityLaneVisible (false);
    settle (processor, editor);
    {
        const auto before = processor.getMelody();
        auto* surface = roll.getChildComponent (0);
        const auto ctrl = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::commandModifier);
        const auto from = roll.getCellCentre (1.3, roll.getTopNote() - 1);
        const auto to = roll.getCellCentre (7.6, roll.getBottomNote() + 3);
        surface->mouseDown (syntheticMouseEvent (*surface, from, ctrl));
        surface->mouseDrag (syntheticMouseEvent (*surface, (from + to) * 0.5f, ctrl));
        surface->mouseDrag (syntheticMouseEvent (*surface, to, ctrl));
        settle (processor, editor);
        ok &= writePng (editor, outputDir.getChildFile ("14h_roll_marquee_in_progress_1100x690.png"));
        surface->mouseUp (syntheticMouseEvent (*surface, to, juce::ModifierKeys()));
        surface->mouseExit (syntheticMouseEvent (*surface, to, juce::ModifierKeys()));
        roll.setSelection ({});
        processor.setMelody (before);
        settle (processor, editor);
    }

    // Export: five chips (normal, then bass Off) at 960 and 1100, 2x.
    {
        auto exportShots = [&]
        {
            const auto area = editor.getExportPanel().getBounds().expanded (6);
            std::vector<juce::Image> images;
            images.push_back (editor.createComponentSnapshot (area, true, 2.0f));
            setParam (processor, cb::ParamIDs::bassMode, 0.0f);
            setParam (processor, cb::ParamIDs::arpOn, 0.0f);
            settle (processor, editor);
            pumpMessages (300);
            images.push_back (editor.createComponentSnapshot (area, true, 2.0f));
            setParam (processor, cb::ParamIDs::bassMode, (float) cb::BassMode::Offbeat8);
            setParam (processor, cb::ParamIDs::arpOn, 1.0f);
            settle (processor, editor);
            return stackImages (images, 10);
        };

        processor.resetLayer (cb::Layer::Bass);
        minSize();
        settle (processor, editor);
        ok &= writeImage (exportShots(), outputDir.getChildFile ("14k_export_five_chips_960_2x.png"));
        defaultSize();
        settle (processor, editor);
        ok &= writeImage (exportShots(), outputDir.getChildFile ("14l_export_five_chips_1100_2x.png"));
    }

    // Controls: the Bass caption glyphs (edited + muted), 2x.
    {
        auto bass = processor.getLayerNotes (cb::Layer::Bass);

        if (! bass.notes.empty())
            bass.notes.erase (bass.notes.begin());

        processor.setLayerNotes (cb::Layer::Bass, bass);
        setParam (processor, cb::ParamIDs::bassOn, 0.0f);
        settle (processor, editor);
        auto voicingGroup = [&editor]
        {
            const auto& controls = editor.getControlsPanel();
            return controls.getBounds().withTrimmedLeft (controls.getWidth() / 3).withWidth (controls.getWidth() / 3 + 8);
        };

        ok &= check (editor.getControlsPanel().getBassStateText() == "edited, muted", "controls: bass state \"edited, muted\"");
        std::vector<juce::Image> images;
        images.push_back (editor.createComponentSnapshot (voicingGroup(), true, 2.0f));
        minSize();
        settle (processor, editor);
        images.push_back (editor.createComponentSnapshot (voicingGroup(), true, 2.0f));
        defaultSize();
        ok &= writeImage (stackImages (images, 10), outputDir.getChildFile ("14m_controls_bass_edited_2x.png"));
        setParam (processor, cb::ParamIDs::bassOn, 1.0f);
        processor.resetLayer (cb::Layer::Bass);
    }

    // G major slash chords: the bass on the Bass tab.
    {
        setParam (processor, cb::ParamIDs::keyRoot, 7.0f);
        setParam (processor, cb::ParamIDs::scale, (float) cb::ScaleType::Major);
        setParam (processor, cb::ParamIDs::bassMode, (float) cb::BassMode::OctaveBounce8);
        auto prog = makeProgression ({ { { 7, Q::Major }, 2 }, { { 2, Q::Major }, 2 }, { { 4, Q::Minor }, 2 }, { { 0, Q::Major }, 2 },
                                       { { 9, Q::Minor7 }, 2 }, { { 2, Q::Dominant7Sus4 }, 2 }, { { 7, Q::Major }, 2 }, { { 0, Q::Major }, 2 } });
        prog.slots[1].inversion = 1;   // D/F#
        prog.slots[6].inversion = 1;   // G/B
        processor.setProgression (prog);
        roll.setLayer (cb::Layer::Bass);
        defaultSize();
        shoot ("14n_roll_bass_slash_chords_1100x690.png");
    }

    // Maximum size: the generated melody over A minor with the arp shown.
    {
        setParam (processor, cb::ParamIDs::keyRoot, 9.0f);
        setParam (processor, cb::ParamIDs::scale, (float) cb::ScaleType::NaturalMinor);
        setParam (processor, cb::ParamIDs::bassMode, (float) cb::BassMode::Offbeat8);
        processor.setProgression (makeProgression ({ { { 9, Q::Minor }, 4 }, { { 5, Q::Major }, 4 },
                                                     { { 0, Q::Major }, 4 }, { { 7, Q::Major }, 4 } }));
        processor.resetLayer (cb::Layer::Arp);
        processor.setMelody (generated);
        setParam (processor, cb::ParamIDs::arpOn, 1.0f);
        roll.setLayer (cb::Layer::Melody);
        editor.setSize (ChordbloomEditor::kMaxWidth, ChordbloomEditor::kMaxHeight);
        shoot ("14o_roll_1600x1000.png");
    }

    // Menus as they open (the real popup windows): generator, its styles, arp and bass patterns, Save, Controls Bass.
    {
        auto& lf = editor.getLookAndFeel();
        std::vector<juce::Image> left, right;
        const auto genMenu = cb::ui::rolltoolbar::createGeneratorMenu (processor, toolbar.getGenerateSettings());
        left.push_back (snapshotMenu (genMenu, lf));

        if (const auto* styles = subMenuNamed (genMenu, "Style"))
            left.push_back (snapshotMenu (*styles, lf));

        left.push_back (snapshotMenu (editor.getExportPanel().createSaveMenu(), lf));
        right.push_back (snapshotMenu (cb::ui::rolltoolbar::createPatternMenu (processor, cb::Layer::Arp), lf));
        right.push_back (snapshotMenu (cb::ui::rolltoolbar::createPatternMenu (processor, cb::Layer::Bass), lf));

        const auto a = stackImages (left, 12), b = stackImages (right, 12);
        juce::Image sheet (juce::Image::ARGB, a.getWidth() + b.getWidth() + 24, std::max (a.getHeight(), b.getHeight()), true);
        {
            juce::Graphics g (sheet);   // scoped: the drawing is committed when the context ends
            g.fillAll (cb::ui::Colours::background);
            g.drawImageAt (a, 0, 0);
            g.drawImageAt (b, a.getWidth() + 24, 0);
        }
        ok &= writeImage (sheet, outputDir.getChildFile ("14p_toolbar_and_save_menus.png"));
    }

    // A standalone toolbar at the design widths (final-spec 7.1 / 7.2), each strip labelled with its width, tab and
    // level: 14q the shrink levels (Melody, Arp custom, Bass custom at 918 / 782 / 750 / 700), 14r the off states at
    // 782 (LED rings, bass Off in the browser, Generate disabled without chords).
    {
        RollToolbar strip (processor);
        auto makeCustom = [&processor] (cb::Layer layer)
        {
            if (processor.isLayerCustom (layer))
                return;

            auto notes = processor.getLayerNotes (layer);

            if (notes.notes.size() > 2)
                notes.notes.erase (notes.notes.begin() + 1);

            processor.setLayerNotes (layer, notes);
        };

        auto labelled = [&strip] (const juce::String& label)
        {
            const auto shot = strip.createComponentSnapshot (strip.getLocalBounds(), true, 2.0f);
            const int labelW = 2 * 150;
            juce::Image image (juce::Image::ARGB, labelW + shot.getWidth() + 40, shot.getHeight() + 20, true);
            {
                juce::Graphics g (image);   // scoped: the drawing is committed when the context ends
                g.fillAll (cb::ui::Colours::panel.brighter (0.02f));
                g.setColour (cb::ui::Colours::textDim);
                g.setFont (cb::ui::font (22.0f));
                g.drawText (label, juce::Rectangle<int> (16, 0, labelW - 24, image.getHeight()), juce::Justification::centredLeft, true);
                g.drawImageAt (shot, labelW, 10);
            }
            return image;
        };

        auto row = [&] (cb::Layer layer, int width, const juce::String& what)
        {
            strip.setSize (width, width >= 900 ? 26 : 24);
            strip.setLayer (layer);
            pumpMessages (10);
            strip.refresh();
            const auto label = juce::String (width) + " px  " + what + "  L" + juce::String (strip.getLayoutLevel());
            std::cout << "  strip " << label << "\n";
            return labelled (label);
        };

        setParam (processor, cb::ParamIDs::arpOn, 1.0f);
        setParam (processor, cb::ParamIDs::bassOn, 1.0f);
        setParam (processor, cb::ParamIDs::bassMode, (float) cb::BassMode::Offbeat8);
        settle (processor, editor);
        makeCustom (cb::Layer::Arp);
        makeCustom (cb::Layer::Bass);
        settle (processor, editor);
        std::vector<juce::Image> levels;

        for (const int width : { 918, 782, 750, 700 })
        {
            levels.push_back (row (cb::Layer::Melody, width, "Melody"));
            levels.push_back (row (cb::Layer::Arp, width, "Arp custom"));
            levels.push_back (row (cb::Layer::Bass, width, "Bass custom"));
        }

        ok &= writeImage (stackImages (levels, 4), outputDir.getChildFile ("14q_roll_toolbar_levels_2x.png"));

        // Off states at 782: auto layers, the arp and the bass muted, then bass mode Off, then no chords at all.
        processor.resetLayer (cb::Layer::Arp);
        processor.resetLayer (cb::Layer::Bass);
        setParam (processor, cb::ParamIDs::arpOn, 0.0f);
        setParam (processor, cb::ParamIDs::bassOn, 0.0f);
        settle (processor, editor);
        std::vector<juce::Image> states;
        states.push_back (row (cb::Layer::Arp, 782, "Arp off"));
        states.push_back (row (cb::Layer::Bass, 782, "Bass off (dot)"));
        setParam (processor, cb::ParamIDs::bassOn, 1.0f);
        setParam (processor, cb::ParamIDs::bassMode, 0.0f);
        settle (processor, editor);
        states.push_back (row (cb::Layer::Bass, 782, "Bass mode Off"));
        const auto progression = processor.getProgression();
        processor.setProgression (cb::Progression {});
        settle (processor, editor);
        states.push_back (row (cb::Layer::Melody, 782, "no chords"));
        states.push_back (row (cb::Layer::Arp, 782, "Arp, no chords"));
        ok &= writeImage (stackImages (states, 4), outputDir.getChildFile ("14r_roll_toolbar_off_states_782_2x.png"));
        processor.setProgression (progression);
        settle (processor, editor);
    }

    // Back to the state the following checks expect: chords view, default size.
    resetLayers();
    editor.setMelodyViewVisible (false);
    defaultSize();
    settle (processor, editor);
    return ok;
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

    // 13. Melody view (harmonic guide piano roll): A minor, Am F C G with a 2-beat G (the loop ends at beat 14, the
    //     roll dims beats 14..16), a melody with chord tones, passing tones, one note outside the key and one note
    //     past the end.
    {
        setParam (processor, cb::ParamIDs::keyRoot, 9.0f);
        setParam (processor, cb::ParamIDs::scale, (float) cb::ScaleType::NaturalMinor);
        processor.setProgression (makeProgression ({ { { 9, Q::Minor }, 4 }, { { 5, Q::Major }, 4 },
                                                     { { 0, Q::Major }, 4 }, { { 7, Q::Major }, 2 } }));
        auto note = [] (int n, double start, double length, float velocity = cb::kDefaultMelodyVelocity)
        {
            cb::MelodyNote m;
            m.note = n;
            m.startBeat = start;
            m.lengthBeats = length;
            m.velocity = velocity;
            return m;
        };

        cb::Melody empty;
        processor.setMelody (empty);
        editor->setSize (ChordbloomEditor::kDefaultWidth, ChordbloomEditor::kDefaultHeight);
        editor->setMelodyViewVisible (true);
        settle (processor, *editor);
        editor->getMelodyRoll().setTopNote (83);
        settle (processor, *editor);
        ok &= writePng (*editor, outputDir.getChildFile ("13a_melody_empty_1100x690.png"));

        cb::Melody melody;
        melody.notes = { note (76, 0.0, 1.0), note (72, 1.0, 0.5, 0.6f), note (74, 1.5, 0.5, 0.6f), note (76, 2.0, 2.0),
                         note (77, 4.0, 1.0), note (76, 5.0, 0.5, 0.6f), note (74, 5.5, 0.5, 0.6f), note (72, 6.0, 2.0),
                         note (79, 8.0, 1.5), note (76, 9.5, 0.5, 0.6f), note (72, 10.0, 1.0), note (75, 11.0, 0.5, 0.5f),
                         note (74, 11.5, 0.5, 0.6f), note (71, 12.0, 1.0), note (74, 13.0, 1.0), note (69, 14.5, 1.0) };
        processor.setMelody (melody);
        settle (processor, *editor);
        ok &= writePng (*editor, outputDir.getChildFile ("13b_melody_1100x690.png"));

        // Hover: the row name pill and the outline of the note a click would draw (Chord snap over C)
        {
            auto& roll = editor->getMelodyRoll();
            roll.setSnap (cb::MelodySnap::ChordTones);
            auto* surface = roll.getChildComponent (0);
            const auto pos = roll.getCellCentre (10.6, 80);
            surface->mouseMove (syntheticMouseEvent (*surface, pos, {}));
            settle (processor, *editor);
            ok &= writeImage (editor->createComponentSnapshot (roll.getBounds(), true, 1.0f),
                              outputDir.getChildFile ("13c_melody_hover_draw_preview.png"));
            surface->mouseExit (syntheticMouseEvent (*surface, pos, {}));
            roll.setSnap (cb::MelodySnap::Scale);
        }

        // Playing (local playback at beat ~9): playhead and the sounding note's glow
        {
            constexpr double sr = 48000.0;
            constexpr int block = 480;
            juce::AudioBuffer<float> audio (2, block);
            juce::MidiBuffer midi;
            setParam (processor, cb::ParamIDs::previewOn, 0.0f);
            processor.prepareToPlay (sr, block);
            processor.setLocalPlayback (true);

            for (int i = 0; i < 900; ++i)   // 9 beats at 120 BPM
            {
                midi.clear();
                processor.processBlock (audio, midi);
            }

            settle (processor, *editor);
            ok &= writePng (*editor, outputDir.getChildFile ("13d_melody_playing_1100x690.png"));
            processor.setLocalPlayback (false);
            midi.clear();
            processor.processBlock (audio, midi);
            processor.releaseResources();
            setParam (processor, cb::ParamIDs::previewOn, 1.0f);
            settle (processor, *editor);
        }

        // Minimum and maximum sizes
        editor->setSize (ChordbloomEditor::kMinWidth, ChordbloomEditor::kMinHeight);
        settle (processor, *editor);
        ok &= writePng (*editor, outputDir.getChildFile ("13e_melody_960x560.png"));
        editor->setSize (ChordbloomEditor::kMaxWidth, ChordbloomEditor::kMaxHeight);
        settle (processor, *editor);
        ok &= writePng (*editor, outputDir.getChildFile ("13f_melody_1600x1000.png"));

        // Ghost notes: a second Chordbloom instance with a bass melody
        {
            ChordbloomProcessor other;
            other.setProgression (processor.getProgression());
            cb::Melody bass;
            bass.notes = { note (69, 0.0, 2.0), note (72, 2.0, 2.0), note (65, 4.0, 4.0), note (67, 8.0, 2.0), note (64, 10.0, 2.0),
                           note (71, 12.0, 2.0) };
            other.setMelody (bass);
            editor->setSize (ChordbloomEditor::kDefaultWidth, ChordbloomEditor::kDefaultHeight);
            settle (processor, *editor);
            editor->getMelodyRoll().setTopNote (81);
            settle (processor, *editor);
            ok &= check (editor->getMelodyRoll().getNumGhostNotesShown() > 0,
                         "melody: ghost notes of another instance are shown ("
                             + juce::String (editor->getMelodyRoll().getNumGhostNotesShown()) + ")");
            ok &= writePng (*editor, outputDir.getChildFile ("13g_melody_ghosts_1100x690.png"));
        }

        settle (processor, *editor);
        ok &= check (editor->getMelodyRoll().getNumGhostNotesShown() == 0, "melody: ghosts go when the other instance closes");

        // Export panel with the Melody chip, default and minimum width
        ok &= writeImage (editor->createComponentSnapshot (editor->getLocalBounds().withTrimmedTop (editor->getHeight() - 80)
                                                               .withTrimmedLeft (editor->getWidth() / 2), true, 2.0f),
                          outputDir.getChildFile ("13h_export_melody_chip_2x.png"));
        editor->setSize (ChordbloomEditor::kMinWidth, ChordbloomEditor::kMinHeight);
        settle (processor, *editor);
        ok &= writeImage (editor->createComponentSnapshot (editor->getLocalBounds().withTrimmedTop (editor->getHeight() - 76)
                                                               .withTrimmedLeft (editor->getWidth() / 2), true, 2.0f),
                          outputDir.getChildFile ("13i_export_melody_chip_960_2x.png"));

        // The palette view with the switch (and its "has notes" dot), at the minimum size
        editor->setMelodyViewVisible (false);
        settle (processor, *editor);
        ok &= writeImage (editor->createComponentSnapshot ({ editor->getWidth() - 260, 56, 260, 80 }, true, 2.0f),
                          outputDir.getChildFile ("13j_view_switch_chords_960_2x.png"));
        editor->setSize (ChordbloomEditor::kDefaultWidth, ChordbloomEditor::kDefaultHeight);
        settle (processor, *editor);
        ok &= writePng (*editor, outputDir.getChildFile ("13k_chords_view_with_switch_1100x690.png"));
    }

    // 14. Phase 2: layers, Generate, the toolbar, five export chips, the Controls bass glyphs (final-spec 8.4).
    ok &= renderPhase2Snapshots (processor, *editor, outputDir);

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
