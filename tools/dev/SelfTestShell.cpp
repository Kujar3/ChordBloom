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

//==============================================================================
// WP6 hooks that are not in the frozen headers (defined in RollToolbar.cpp / ExportPanel.cpp).
namespace cb::ui::rolltoolbar
{
juce::PopupMenu createPatternMenu (ChordbloomProcessor&, cb::Layer);
bool applyPatternMenuResult (ChordbloomProcessor&, cb::Layer, int result);
juce::PopupMenu createGeneratorMenu (ChordbloomProcessor&, const cb::MelodyGenSettings&);
void applyGeneratorMenuResult (RollToolbar&, ChordbloomProcessor&, int result);
void sendMouseEvent (RollToolbar&, const juce::MouseEvent&, int kind);   // 0 down, 1 up, 2 move, 3 exit, 4 drag
juce::String getBrowserText (RollToolbar&);
juce::String getTooltipAt (RollToolbar&, juce::Point<float>);
}

namespace cb::ui::exportpanel
{
float chipContentWidth (float chipWidth, const juce::String& label, bool off);
}

namespace
{
namespace rt = cb::ui::rolltoolbar;
using TI = RollToolbarItem;
using Part = ChordbloomProcessor::ExportPart;

const auto kLeft = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier);
const auto kRight = juce::ModifierKeys (juce::ModifierKeys::rightButtonModifier);

const char* itemName (TI item)
{
    static const char* names[] = { "Tabs", "TabMelody", "TabArp", "TabBass", "LedMelody", "LedArp", "LedBass", "Generate",
                                   "GenerateMenu", "BrowserPrev", "BrowserName", "BrowserNext", "Reset", "Snap", "Grid",
                                   "VelocityLane", "More" };
    const int i = (int) item;
    return i >= 0 && i < (int) std::size (names) ? names[i] : "?";
}

// A left (or right) click at an item's centre, routed like a real mouse event.
void clickItem (RollToolbar& tb, TI item, bool right = false)
{
    const auto c = tb.getItemBounds (item).toFloat().getCentre();
    const auto mods = right ? kRight : kLeft;
    rt::sendMouseEvent (tb, mouseEventFor (tb, c, mods), 0);
    rt::sendMouseEvent (tb, mouseEventFor (tb, c, mods), 1);
}

void clickAt (RollToolbar& tb, juce::Point<float> p, bool right = false)
{
    const auto mods = right ? kRight : kLeft;
    rt::sendMouseEvent (tb, mouseEventFor (tb, p, mods), 0);
    rt::sendMouseEvent (tb, mouseEventFor (tb, p, mods), 1);
}

int paramInt (ChordbloomProcessor& p, const char* id) { return juce::roundToInt (getParam (p, id)); }

// Makes an Arp / Bass layer custom: its auto notes minus the first one.
bool makeCustom (ChordbloomProcessor& p, cb::Layer layer)
{
    auto notes = p.getLayerNotes (layer);

    if (notes.notes.size() < 2)
        return false;

    notes.notes.erase (notes.notes.begin());
    p.setLayerNotes (layer, notes);
    return p.isLayerCustom (layer);
}

struct MenuEntry
{
    juce::String text, shortcut;
    int id = 0;
    bool enabled = true, ticked = false, header = false, separator = false, subMenu = false;
};

std::vector<MenuEntry> menuEntries (const juce::PopupMenu& menu)
{
    std::vector<MenuEntry> out;

    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
    {
        const auto& item = it.getItem();
        MenuEntry e;
        e.text = item.text;
        e.shortcut = item.shortcutKeyDescription;
        e.id = item.itemID;
        e.enabled = item.isEnabled;
        e.ticked = item.isTicked;
        e.header = item.isSectionHeader;
        e.separator = item.isSeparator;
        e.subMenu = item.subMenu != nullptr;
        out.push_back (e);
    }

    return out;
}

const juce::PopupMenu* subMenuOf (const juce::PopupMenu& menu, const juce::String& text)
{
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        if (it.getItem().text == text && it.getItem().subMenu != nullptr)
            return it.getItem().subMenu.get();

    return nullptr;
}

std::string entriesToString (const std::vector<MenuEntry>& entries)
{
    std::string s;

    for (const auto& e : entries)
        s += (e.separator ? std::string ("---") : (e.header ? "[" + e.text.toStdString() + "]" : e.text.toStdString()))
           + (e.ticked ? "*" : "") + (! e.enabled ? "(off)" : "") + " | ";

    return s;
}

juce::StringArray itemTexts (const std::vector<MenuEntry>& entries, bool includeHeaders = false)
{
    juce::StringArray texts;

    for (const auto& e : entries)
        if (! e.separator && (includeHeaders || ! e.header))
            texts.add (e.text);

    return texts;
}

//==============================================================================
// Toolbar geometry at one width: the level, no overlapping items, everything inside, hidden items empty.
struct LayoutReport
{
    int level = -1;
    bool overlap = false, outside = false, ledsInTabs = true, hiddenOk = true;
    std::string detail;
};

LayoutReport inspectToolbar (RollToolbar& tb, cb::Layer layer, bool custom)
{
    LayoutReport r;
    r.level = tb.getLayoutLevel();
    const auto local = tb.getLocalBounds();
    const TI leaves[] = { TI::TabMelody, TI::TabArp, TI::TabBass, TI::Generate, TI::GenerateMenu, TI::BrowserPrev,
                          TI::BrowserName, TI::BrowserNext, TI::Reset, TI::Snap, TI::Grid, TI::VelocityLane, TI::More };

    for (size_t i = 0; i < std::size (leaves); ++i)
    {
        const auto a = tb.getItemBounds (leaves[i]);

        if (a.isEmpty())
            continue;

        if (! local.contains (a))
        {
            r.outside = true;
            r.detail += str (itemName (leaves[i]), " outside; ");
        }

        for (size_t j = i + 1; j < std::size (leaves); ++j)
        {
            const auto b = tb.getItemBounds (leaves[j]);

            if (! b.isEmpty() && a.intersects (b))
            {
                r.overlap = true;
                r.detail += str (itemName (leaves[i]), " overlaps ", itemName (leaves[j]), "; ");
            }
        }
    }

    const std::pair<TI, TI> leds[] = { { TI::LedMelody, TI::TabMelody }, { TI::LedArp, TI::TabArp }, { TI::LedBass, TI::TabBass } };

    for (const auto& [led, tab] : leds)
        if (! tb.getItemBounds (tab).contains (tb.getItemBounds (led)) || ! tb.getItemBounds (TI::Tabs).contains (tb.getItemBounds (tab)))
            r.ledsInTabs = false;

    const bool melody = layer == cb::Layer::Melody;
    r.hiddenOk = (tb.getItemBounds (TI::Generate).isEmpty() != melody)
              && (tb.getItemBounds (TI::BrowserName).isEmpty() == melody)
              && (tb.getItemBounds (TI::Reset).isEmpty() == (melody || ! custom))
              && (tb.getItemBounds (TI::VelocityLane).isEmpty() == (r.level >= 5));
    return r;
}

//==============================================================================
// Standalone RollToolbar (final-spec 9 WP6): levels at the 782 / 918 px design budgets for every layer x state, no
// overlapping items, mouse events at the item centres, the browser, LEDs, Reset, Generate, menus, tooltips.
void testRollToolbarLayout()
{
    ChordbloomProcessor p;
    setParam (p, cb::ParamIDs::keyRoot, 9.0f);
    setParam (p, cb::ParamIDs::scale, (float) cb::ScaleType::NaturalMinor);
    p.setProgression (makeProgression ({ Am_, F_, C_, G_ }));
    pumpMessages (30);

    RollToolbar tb (p);
    check (tb.getNumChildComponents() == 0, "toolbar: every item is a painted region (no child components)");
    check (! tb.getWantsKeyboardFocus() && ! tb.getMouseClickGrabsKeyboardFocus(), "toolbar: never takes keyboard focus");

    struct Case
    {
        const char* name;
        cb::Layer layer;
        std::function<void()> setUp;
        bool custom;
    };

    auto resetAll = [&p]
    {
        p.resetLayer (cb::Layer::Arp);
        p.resetLayer (cb::Layer::Bass);
        setParam (p, cb::ParamIDs::arpOn, 0.0f);
        setParam (p, cb::ParamIDs::bassOn, 1.0f);
        setParam (p, cb::ParamIDs::bassMode, 1.0f);
        pumpMessages (10);
    };

    const auto melodyNotes = makeMelody ({ melodyNote (76, 0.0, 1.0), melodyNote (72, 1.0, 1.0), melodyNote (74, 4.0, 2.0) });
    const std::vector<Case> cases = {
        { "melody empty", cb::Layer::Melody, [&] { resetAll(); p.clearMelody(); }, false },
        { "melody with notes", cb::Layer::Melody, [&] { resetAll(); p.setMelody (melodyNotes); }, false },
        { "arp auto off", cb::Layer::Arp, [&] { resetAll(); }, false },
        { "arp auto on", cb::Layer::Arp, [&] { resetAll(); setParam (p, cb::ParamIDs::arpOn, 1.0f); }, false },
        { "arp custom on", cb::Layer::Arp, [&] { resetAll(); setParam (p, cb::ParamIDs::arpOn, 1.0f); makeCustom (p, cb::Layer::Arp); }, true },
        { "arp custom off", cb::Layer::Arp, [&] { resetAll(); makeCustom (p, cb::Layer::Arp); }, true },
        { "bass auto", cb::Layer::Bass, [&] { resetAll(); }, false },
        { "bass custom", cb::Layer::Bass, [&] { resetAll(); makeCustom (p, cb::Layer::Bass); }, true },
        { "bass off (bassOn)", cb::Layer::Bass, [&] { resetAll(); setParam (p, cb::ParamIDs::bassOn, 0.0f); }, false },
        { "bass mode Off", cb::Layer::Bass, [&] { resetAll(); setParam (p, cb::ParamIDs::bassMode, 0.0f); }, false },
        { "bass custom muted", cb::Layer::Bass, [&] { resetAll(); makeCustom (p, cb::Layer::Bass); setParam (p, cb::ParamIDs::bassOn, 0.0f); }, true },
    };

    for (const auto& c : cases)
    {
        c.setUp();
        tb.setLayer (c.layer);
        tb.refresh();
        check (p.isLayerCustom (c.layer) == c.custom, str ("toolbar setup: ", c.name, " has the expected custom flag"));

        for (const auto [width, height] : { std::pair<int, int> { 782, 24 }, std::pair<int, int> { 918, 26 } })
        {
            tb.setSize (width, height);
            const auto r = inspectToolbar (tb, c.layer, c.custom);
            const std::string at = str (" (", c.name, " at ", width, "x", height, ", level ", r.level, ")");
            check (! r.overlap && ! r.outside, "toolbar: no overlapping items, all inside" + at, r.detail);
            check (r.ledsInTabs, "toolbar: each LED sits in its tab, the tabs in the tab bar" + at);
            check (r.hiddenOk, "toolbar: only this layer's controls are shown (Reset only while custom)" + at);

            if (width == 782)
                check (r.level <= 3, "toolbar: level <= 3 at the 960x560 budget (782 px)" + at);
            else if (c.layer == cb::Layer::Melody)
                check (r.level == 0, "toolbar: the Melody tab is level 0 at the 1100x690 budget (918 px)" + at);
            else
                check (r.level <= (c.custom ? 1 : 0), "toolbar: Arp / Bass at 918 px: auto level 0, custom level <= 1" + at);
        }
    }

    // The tab strip keeps its geometry when the tab changes (laid out from the width alone), at every budget width.
    {
        resetAll();
        makeCustom (p, cb::Layer::Arp);
        makeCustom (p, cb::Layer::Bass);

        for (const int width : { 700, 750, 782, 800, 860, 918, 1000 })
        {
            tb.setSize (width, width < 900 ? 24 : 26);
            std::array<juce::Rectangle<int>, 4> first {};
            bool same = true;
            std::string levels;

            for (auto layer : { cb::Layer::Melody, cb::Layer::Arp, cb::Layer::Bass })
            {
                tb.setLayer (layer);
                tb.refresh();
                const std::array<juce::Rectangle<int>, 4> now { tb.getItemBounds (TI::Tabs), tb.getItemBounds (TI::TabMelody),
                                                                tb.getItemBounds (TI::TabArp), tb.getItemBounds (TI::TabBass) };
                levels += str (cb::layerName (layer), " L", tb.getLayoutLevel(), " ");

                if (layer == cb::Layer::Melody)
                    first = now;
                else
                    same = same && now == first;
            }

            check (same, str ("toolbar: switching tabs never moves the tabs (", width, " px, arp + bass custom)"), levels);
        }

        resetAll();
    }

    // Wider is never worse; 1400 px is level 0 for every tab.
    {
        resetAll();
        makeCustom (p, cb::Layer::Arp);
        int worstWide = 0;

        for (auto layer : { cb::Layer::Melody, cb::Layer::Arp, cb::Layer::Bass })
        {
            tb.setLayer (layer);
            tb.refresh();
            int previous = 6;
            bool monotonic = true;

            for (int w = 600; w <= 1400; w += 20)
            {
                tb.setSize (w, 26);
                monotonic = monotonic && tb.getLayoutLevel() <= previous;
                previous = tb.getLayoutLevel();
            }

            worstWide = std::max (worstWide, tb.getLayoutLevel());
            check (monotonic, str ("toolbar: the level never grows with the width (", cb::layerName (layer), ")"));
        }

        check (worstWide == 0, "toolbar: level 0 for every tab at 1400 px (1600x1000)");
        tb.setLayer (cb::Layer::Melody);
        tb.setSize (600, 24);
        check (tb.getLayoutLevel() == 5 && tb.getItemBounds (TI::VelocityLane).isEmpty(),
               "toolbar: level 5 hides the velocity toggle (it lives in the menu)");
        std::cout << "  toolbar widths: Generate split " << tb.getItemBounds (TI::Generate).getWidth() << " + "
                  << tb.getItemBounds (TI::GenerateMenu).getWidth() << " at level 5\n";
        tb.setSize (918, 26);
        std::cout << "  toolbar widths at 918: tabs " << tb.getItemBounds (TI::Tabs).getWidth() << ", Generate "
                  << tb.getItemBounds (TI::Generate).getWidth() << " + style " << tb.getItemBounds (TI::GenerateMenu).getWidth()
                  << ", Snap " << tb.getItemBounds (TI::Snap).getWidth() << ", Grid " << tb.getItemBounds (TI::Grid).getWidth() << "\n";
        resetAll();
    }
}

void testRollToolbarInteraction()
{
    ChordbloomProcessor p;
    setParam (p, cb::ParamIDs::keyRoot, 9.0f);
    setParam (p, cb::ParamIDs::scale, (float) cb::ScaleType::NaturalMinor);
    p.setProgression (makeProgression ({ Am_, F_, C_, G_ }));
    pumpMessages (30);

    RollToolbar tb (p);
    tb.setSize (918, 26);
    std::vector<cb::Layer> chosen, tabMenus;
    std::vector<cb::MelodySnap> snaps;
    std::vector<cb::RateDivision> grids;
    std::vector<bool> velocity;
    std::vector<juce::Rectangle<int>> moreAreas;
    juce::StringArray hints;
    int generated = 0;

    tb.onLayerChosen = [&] (cb::Layer l) { chosen.push_back (l); tb.setLayer (l); };
    tb.onTabMenu = [&] (cb::Layer l) { tabMenus.push_back (l); };
    tb.onSnapChosen = [&] (cb::MelodySnap s) { snaps.push_back (s); };
    tb.onGridChosen = [&] (cb::RateDivision g) { grids.push_back (g); };
    tb.onVelocityLaneToggled = [&] (bool on) { velocity.push_back (on); };
    tb.onMoreClicked = [&] (juce::Rectangle<int> area) { moreAreas.push_back (area); };
    tb.onHint = [&] (const juce::String& h) { hints.add (h); };
    tb.onMelodyGenerated = [&] { ++generated; };

    // Tabs: click selects (through onLayerChosen); right-click on a tab / LED opens that tab's menu and changes nothing.
    clickItem (tb, TI::TabArp);
    check (chosen.size() == 1 && chosen[0] == cb::Layer::Arp, "toolbar: clicking the Arp tab chooses the Arp layer");
    check (! tb.getItemBounds (TI::BrowserName).isEmpty() && tb.getItemBounds (TI::Generate).isEmpty(),
           "toolbar: the Arp tab shows the pattern browser instead of Generate");

    const float arpOnBefore = getParam (p, cb::ParamIDs::arpOn);
    clickItem (tb, TI::TabBass, true);
    clickItem (tb, TI::LedArp, true);
    check (tabMenus.size() == 2 && tabMenus[0] == cb::Layer::Bass && tabMenus[1] == cb::Layer::Arp && chosen.size() == 1
               && getParam (p, cb::ParamIDs::arpOn) == arpOnBefore,
           "toolbar: right-click on a tab or LED opens that layer's menu; it neither switches the tab nor toggles");

    // LEDs: arpOn toggles; the bass LED follows final-spec 2.3.
    clickItem (tb, TI::LedArp);
    check (getParam (p, cb::ParamIDs::arpOn) >= 0.5f && p.isLayerOn (cb::Layer::Arp), "toolbar: the Arp LED turns the arp on");
    clickItem (tb, TI::LedArp);
    check (getParam (p, cb::ParamIDs::arpOn) < 0.5f, "toolbar: a second click turns it off");
    check (chosen.size() == 1, "toolbar: an LED click never switches the tab");

    setParam (p, cb::ParamIDs::bassMode, 0.0f);
    setParam (p, cb::ParamIDs::bassOn, 0.0f);
    clickItem (tb, TI::LedBass);
    check (getParam (p, cb::ParamIDs::bassOn) >= 0.5f && paramInt (p, cb::ParamIDs::bassMode) == (int) cb::BassMode::Root,
           "toolbar: the unlit Bass LED turns bassOn on and bassMode Off into Root");
    clickItem (tb, TI::LedBass);
    check (getParam (p, cb::ParamIDs::bassOn) < 0.5f && paramInt (p, cb::ParamIDs::bassMode) == (int) cb::BassMode::Root,
           "toolbar: the lit Bass LED only turns bassOn off");
    setParam (p, cb::ParamIDs::bassOn, 1.0f);

    // Browser: > steps arpShape (the parameter first, then the pick switches the arp on); right-clicks change nothing.
    tb.setLayer (cb::Layer::Arp);
    setParam (p, cb::ParamIDs::arpShape, 0.0f);
    tb.refresh();
    clickItem (tb, TI::BrowserNext, true);
    clickItem (tb, TI::BrowserPrev, true);
    clickItem (tb, TI::BrowserName, true);
    check (paramInt (p, cb::ParamIDs::arpShape) == 0 && getParam (p, cb::ParamIDs::arpOn) < 0.5f,
           "toolbar: right-clicks on the browser arrows and name change nothing");
    clickItem (tb, TI::BrowserNext);
    check (paramInt (p, cb::ParamIDs::arpShape) == 1 && getParam (p, cb::ParamIDs::arpOn) >= 0.5f,
           "toolbar: > steps the arp shape (Up 3 -> Down 3) and switches the arp on");
    clickItem (tb, TI::BrowserPrev);
    clickItem (tb, TI::BrowserPrev);
    check (paramInt (p, cb::ParamIDs::arpShape) == cb::numArpShapes - 1, "toolbar: < wraps from Up 3 to the last shape");
    tb.stepPattern (1);
    check (paramInt (p, cb::ParamIDs::arpShape) == 0, "toolbar: > wraps back to Up 3");

    // Rate suffix: the arp always (at full width), the bass only for Octaves / Pulse.
    check (rt::getBrowserText (tb).endsWith (cb::rateDivisionName (cb::RateDivision::Sixteenth)) && rt::getBrowserText (tb).startsWith ("Up 3"),
           "toolbar: the arp browser reads \"Up 3 . 1/16\"", rt::getBrowserText (tb).toStdString());
    tb.setLayer (cb::Layer::Bass);
    const std::pair<cb::BassMode, bool> suffixCases[] = { { cb::BassMode::Root, false }, { cb::BassMode::Octaves, true },
                                                          { cb::BassMode::Pulse, true }, { cb::BassMode::Offbeat8, false },
                                                          { cb::BassMode::Driving8, false } };
    bool suffixOk = true;
    std::string suffixDetail;

    for (const auto& [mode, suffix] : suffixCases)
    {
        setParam (p, cb::ParamIDs::bassMode, (float) mode);
        tb.refresh();
        const auto text = rt::getBrowserText (tb);
        suffixDetail += text.toStdString() + "; ";
        suffixOk = suffixOk && text.startsWith (cb::bassModeName (mode)) && (text.length() > juce::String (cb::bassModeName (mode)).length()) == suffix;
    }

    check (suffixOk, "toolbar: the rate suffix shows only for the Octaves / Pulse bass", suffixDetail);
    tb.setSize (782, 24);
    setParam (p, cb::ParamIDs::bassMode, (float) cb::BassMode::Octaves);
    makeCustom (p, cb::Layer::Bass);
    tb.refresh();
    check (tb.getLayoutLevel() >= 3 ? rt::getBrowserText (tb) == "Octaves" : true,
           "toolbar: no rate suffix on the compact browser", rt::getBrowserText (tb).toStdString());
    p.resetLayer (cb::Layer::Bass);
    tb.setSize (918, 26);

    // Bass browser from Off: > Root, < Dotted quarters.
    setParam (p, cb::ParamIDs::bassMode, 0.0f);
    tb.refresh();
    clickItem (tb, TI::BrowserNext);
    check (paramInt (p, cb::ParamIDs::bassMode) == (int) cb::BassMode::Root, "toolbar: from bass Off, > goes to Root");
    setParam (p, cb::ParamIDs::bassMode, 0.0f);
    clickItem (tb, TI::BrowserPrev);
    check (paramInt (p, cb::ParamIDs::bassMode) == (int) cb::BassMode::DottedQuarter, "toolbar: from bass Off, < goes to Dotted quarters");

    // A pick on a custom layer: the parameter is set, the layer resets (one undo step) and turns on; one hint per session.
    tb.setLayer (cb::Layer::Arp);
    setParam (p, cb::ParamIDs::arpShape, 0.0f);
    setParam (p, cb::ParamIDs::arpOn, 0.0f);
    check (makeCustom (p, cb::Layer::Arp), "toolbar setup: custom arp");
    tb.refresh();
    check (! tb.getItemBounds (TI::Reset).isEmpty(), "toolbar: the Reset pill shows while the arp is custom");
    const auto customNotes = p.getLayerNotes (cb::Layer::Arp);
    clickItem (tb, TI::BrowserNext);
    check (paramInt (p, cb::ParamIDs::arpShape) == 1 && ! p.isLayerCustom (cb::Layer::Arp) && p.isLayerOn (cb::Layer::Arp),
           "toolbar: > on a custom arp sets the shape, resets the layer and turns it on");
    check (hints.size() == 1 && hints[0].contains ("Arp follows the pattern again"), "toolbar: the reset hint is shown",
           hints.joinIntoString ("; ").toStdString());
    tb.refresh();
    check (tb.getItemBounds (TI::Reset).isEmpty(), "toolbar: the Reset pill goes once the arp follows the pattern again");
    p.undo();
    check (p.isLayerCustom (cb::Layer::Arp) && p.getLayerNotes (cb::Layer::Arp) == customNotes
               && paramInt (p, cb::ParamIDs::arpShape) == 1,
           "toolbar: one undo step brings the custom notes back (the shape parameter stays: never undoable)");
    tb.stepPattern (1);
    check (hints.size() == 1, "toolbar: each hint kind is shown once per editor session");

    // Reset pill: right-click does nothing, left-click resets.
    makeCustom (p, cb::Layer::Arp);
    tb.refresh();
    clickItem (tb, TI::Reset, true);
    check (p.isLayerCustom (cb::Layer::Arp), "toolbar: right-click on Reset changes nothing");
    clickItem (tb, TI::Reset);
    check (! p.isLayerCustom (cb::Layer::Arp), "toolbar: Reset makes the arp follow the chords again");

    // Pattern menus (what the async menus show) and their picks.
    {
        const auto arpMenu = menuEntries (rt::createPatternMenu (p, cb::Layer::Arp));
        const auto texts = itemTexts (arpMenu, true);
        int shapes = 0;
        bool shapeTicked = false;

        for (const auto& e : arpMenu)
            if (! e.header && ! e.separator && ! e.subMenu)
            {
                ++shapes;
                shapeTicked = shapeTicked || (e.ticked && e.text == cb::arpShapeName ((cb::ArpShape) paramInt (p, cb::ParamIDs::arpShape)));
            }

        check (shapes == cb::numArpShapes && shapeTicked && texts.contains ("Ladders") && texts.contains ("Pedal & bounce")
                   && texts.contains ("Two-note") && texts.contains ("Rate") && texts.contains ("Rhythm") && texts.contains ("Gate")
                   && texts.contains ("Octave"),
               "toolbar: the arp menu lists 17 shapes in three families plus Rate / Rhythm / Gate / Octave", entriesToString (arpMenu));

        bool suggestion = true;

        for (const auto& e : arpMenu)
            if (e.text.startsWith ("Alternate"))
                suggestion = suggestion && e.shortcut == "1/8";

        check (suggestion, "toolbar: the two-note shapes show \"1/8\" as a grey suggestion");

        auto fullMenu = rt::createPatternMenu (p, cb::Layer::Arp);
        const auto* gate = subMenuOf (fullMenu, "Gate");
        const auto gateTexts = gate != nullptr ? itemTexts (menuEntries (*gate)) : juce::StringArray();
        check (gateTexts.joinIntoString (", ") == "Pluck 45%, Short 65%, Normal 90%, Legato 100%",
               "toolbar: the arp gate presets", gateTexts.joinIntoString (", ").toStdString());
        setParam (p, cb::ParamIDs::arpGate, 0.3f);
        fullMenu = rt::createPatternMenu (p, cb::Layer::Arp);
        gate = subMenuOf (fullMenu, "Gate");
        check (gate != nullptr && itemTexts (menuEntries (*gate)).contains ("Custom 30%"),
               "toolbar: an unmatched gate adds \"Custom NN%\"");

        makeCustom (p, cb::Layer::Arp);
        check (rt::applyPatternMenuResult (p, cb::Layer::Arp, 400) && std::abs (getParam (p, cb::ParamIDs::arpGate) - 0.45f) < 0.01f
                   && ! p.isLayerCustom (cb::Layer::Arp),
               "toolbar: a gate preset pick sets arpGate and resets the custom arp");

        const auto bassMenu = menuEntries (rt::createPatternMenu (p, cb::Layer::Bass));
        const auto bassTexts = itemTexts (bassMenu, true);
        check (itemTexts (bassMenu).size() == cb::numBassModes && bassTexts[0] == "Off" && bassTexts.contains ("Held")
                   && bassTexts.contains ("Follows Rate") && bassTexts.contains ("Trance") && bassTexts.contains ("Groove")
                   && bassTexts.indexOf ("Trance") < bassTexts.indexOf ("Offbeat 8ths"),
               "toolbar: the bass menu lists Off and the 14 modes under Held / Follows Rate / Trance / Groove",
               entriesToString (bassMenu));

        setParam (p, cb::ParamIDs::bassMode, 1.0f);
        makeCustom (p, cb::Layer::Bass);
        rt::applyPatternMenuResult (p, cb::Layer::Bass, 600);
        check (paramInt (p, cb::ParamIDs::bassMode) == 0 && p.isLayerCustom (cb::Layer::Bass),
               "toolbar: picking bass Off only sets Off (custom notes kept)");
        rt::applyPatternMenuResult (p, cb::Layer::Bass, 600 + (int) cb::BassMode::Gallop);
        check (paramInt (p, cb::ParamIDs::bassMode) == (int) cb::BassMode::Gallop && ! p.isLayerCustom (cb::Layer::Bass),
               "toolbar: picking a bass pattern sets it and resets the custom bass");
    }

    // Snap / Grid / velocity toggle / "...": left-clicks call back, right-clicks do nothing.
    tb.setLayer (cb::Layer::Melody);
    const auto snapArea = tb.getItemBounds (TI::Snap).toFloat();
    clickAt (tb, { snapArea.getRight() - 6.0f, snapArea.getCentreY() }, true);
    clickAt (tb, { snapArea.getRight() - 6.0f, snapArea.getCentreY() });
    const auto gridArea = tb.getItemBounds (TI::Grid).toFloat();
    clickAt (tb, { gridArea.getX() + 6.0f, gridArea.getCentreY() });
    clickItem (tb, TI::VelocityLane, true);
    clickItem (tb, TI::VelocityLane);
    clickItem (tb, TI::More, true);
    clickItem (tb, TI::More);
    check (snaps.size() == 1 && snaps[0] == cb::MelodySnap::ChordTones, "toolbar: the last Snap segment chooses Chord (right-click ignored)");
    check (grids.size() == 1 && grids[0] == cb::RateDivision::Quarter, "toolbar: the first Grid segment chooses 1/4");
    check (velocity.size() == 1 && velocity[0], "toolbar: the velocity toggle turns the lane on (right-click ignored)");
    check (moreAreas.size() == 1 && ! moreAreas[0].isEmpty(), "toolbar: \"...\" asks the roll to open its menu at its screen area");

    // Tooltips (painted regions have their own hover tips).
    {
        const auto genTip = rt::getTooltipAt (tb, tb.getItemBounds (TI::Generate).toFloat().getCentre());
        check (genTip.contains ("Generate a new melody") && genTip.contains (cb::melodyStyleName (tb.getGenerateSettings().style)),
               "toolbar: the Generate tooltip names the style", genTip.toStdString());
        const auto tabTip = rt::getTooltipAt (tb, tb.getItemBounds (TI::TabArp).toFloat().getCentre());
        check (tabTip.startsWith ("Arp: auto") && tabTip.contains ("MIDI ch 4"), "toolbar: the Arp tab tooltip carries the readout",
               tabTip.toStdString());
        makeCustom (p, cb::Layer::Bass);
        const auto bassTip = rt::getTooltipAt (tb, tb.getItemBounds (TI::TabBass).toFloat().getCentre());
        check (bassTip.contains ("edited") && bassTip.contains ("Reset follows the chords again"), "toolbar: the Bass tab tooltip says edited",
               bassTip.toStdString());
        p.resetLayer (cb::Layer::Bass);
    }

    // Generate (WP1 + WP4): one undo step per click; a settings pick regenerates the untouched result with its seed and
    // only stores the setting after a hand edit; cb.gen.* round trip; disabled without chords.
    {
        p.clearMelody();
        const int undoDepthProbe = 0;
        juce::ignoreUnused (undoDepthProbe);
        clickItem (tb, TI::Generate);
        const auto first = p.getMelody();
        const auto seed = (int) p.getAPVTS().state.getProperty ("cb.gen.seed", 0);
        check (! first.empty() && generated == 1 && seed != 0 && p.isMelodyFromGenerator(),
               "toolbar: Generate writes a melody, stores cb.gen.seed and calls onMelodyGenerated",
               str (first.notes.size(), " notes, seed ", seed));
        p.undo();
        check (p.getMelody().empty(), "toolbar: one undo step takes the generated melody away");
        p.redo();
        check (p.getMelody() == first, "toolbar: redo brings it back");

        // Regenerating with the same seed while untouched: the result follows the new setting.
        p.setMelody (cb::Melody());
        check (tb.generateMelody ((std::uint32_t) seed) && p.getMelody() == first, "toolbar: generateMelody (seed) is deterministic");
        auto s = tb.getGenerateSettings();
        s.density = 0.85f;
        tb.setGenerateSettings (s);
        const auto busier = p.getMelody();
        check (busier != first && p.isMelodyFromGenerator() && busier.notes.size() > first.notes.size(),
               "toolbar: a settings pick regenerates the untouched melody with its seed (Busy has more notes)",
               str (first.notes.size(), " -> ", busier.notes.size()));

        auto edited = busier;
        edited.notes.pop_back();
        p.setMelody (edited);
        s.density = 0.15f;
        tb.setGenerateSettings (s);
        check (p.getMelody() == edited && std::abs ((double) p.getAPVTS().state.getProperty ("cb.gen.density", 0.0) - 0.15) < 1.0e-6,
               "toolbar: after a hand edit a settings pick is only stored");

        // "Generate again (same idea)" uses cb.gen.seed with the current settings.
        auto genMenu = menuEntries (rt::createGeneratorMenu (p, tb.getGenerateSettings()));
        bool againEnabled = false;
        for (const auto& e : genMenu)
            againEnabled = againEnabled || (e.text.startsWith ("Generate again") && e.enabled);
        check (againEnabled, "toolbar: \"Generate again\" is enabled once a seed is stored");
        rt::applyGeneratorMenuResult (tb, p, 701);
        check (p.getMelody() != edited && p.isMelodyFromGenerator(), "toolbar: Generate again regenerates from the stored seed");

        const auto genTexts = itemTexts (genMenu, true);
        check (genTexts.contains ("Melody generator") && genTexts.contains ("Style") && genTexts.contains ("Density")
                   && genTexts.contains ("Range") && genTexts.contains ("Contour") && genTexts.contains ("Repetition")
                   && genTexts.contains ("End on the home note") && genTexts.contains ("New melody"),
               "toolbar: the generator menu lists Style / Density / Range / Contour / Repetition / End on the home note / "
               "New melody / Generate again", entriesToString (genMenu));
        auto fullGen = rt::createGeneratorMenu (p, tb.getGenerateSettings());
        const auto* styles = subMenuOf (fullGen, "Style");
        check (styles != nullptr && itemTexts (menuEntries (*styles)).size() == cb::numMelodyStyles,
               "toolbar: the Style submenu lists the four styles");

        // A style pick through the menu result handler, then the cb.gen.* round trip through the plugin state.
        rt::applyGeneratorMenuResult (tb, p, 100 + (int) cb::MelodyStyle::Driving);
        rt::applyGeneratorMenuResult (tb, p, 300 + 2);   // Range High
        rt::applyGeneratorMenuResult (tb, p, 400 + (int) cb::MelodyContour::Arch);
        rt::applyGeneratorMenuResult (tb, p, 600);       // End on the home note off
        const auto picked = tb.getGenerateSettings();
        check (picked.style == cb::MelodyStyle::Driving && picked.registerShift == 5 && picked.contour == cb::MelodyContour::Arch
                   && ! picked.resolveEnding && std::abs (picked.density - 0.15f) < 1.0e-6f,
               "toolbar: generator menu picks are stored in cb.gen.*");

        juce::MemoryBlock state;
        p.getStateInformation (state);
        ChordbloomProcessor q;
        q.setStateInformation (state.getData(), (int) state.getSize());
        pumpMessages (60);
        RollToolbar other (q);
        check (other.getGenerateSettings() == picked && ! q.isMelodyFromGenerator(),
               "toolbar: cb.gen.* survive a state round trip (and a loaded melody is never treated as untouched)");

        ChordbloomProcessor empty;
        RollToolbar emptyBar (empty);
        emptyBar.setSize (918, 26);
        emptyBar.generateNewMelody();
        clickItem (emptyBar, TI::Generate);
        check (empty.getMelody().empty() && rt::getTooltipAt (emptyBar, emptyBar.getItemBounds (TI::Generate).toFloat().getCentre())
                                                == "Add chords first",
               "toolbar: Generate is disabled without chords (tooltip \"Add chords first\")");

        // Default style follows the Spark mood until the user picks one.
        check (emptyBar.getGenerateSettings().style == cb::melodyStyleForMood (cb::Mood::Emotional),
               "toolbar: the default style follows the Spark mood (Emotional -> Lyrical)");
    }
}

// (WP2 + WP4) With a custom arp on and playing, stepping the browser strikes no note of the old notes on channel 4.
void testBrowserPickWhilePlaying()
{
    ChordbloomProcessor p;
    setParam (p, cb::ParamIDs::previewOn, 0.0f);
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));
    setParam (p, cb::ParamIDs::arpOn, 1.0f);
    setParam (p, cb::ParamIDs::arpShape, 0.0f);
    pumpMessages (30);
    makeCustom (p, cb::Layer::Arp);

    RollToolbar tb (p);
    tb.setSize (918, 26);
    tb.setLayer (cb::Layer::Arp);

    Rig rig (p);
    rig.head.playing = true;
    rig.run (40);
    const auto pickSample = rig.samplePos;
    tb.stepPattern (-1);   // Up 3 -> Alternate 1-5: no thirds
    const auto after = rig.run (60);

    std::set<int> newPitches;
    for (const auto& e : p.renderLayerEvents (cb::Layer::Arp))
        newPitches.insert (e.note);

    int stray = 0;
    for (const auto& e : after)
        if (e.on && e.channel == 4 && e.sample >= pickSample && newPitches.count (e.note) == 0)
            ++stray;

    check (paramInt (p, cb::ParamIDs::arpShape) == (int) cb::ArpShape::Alternate15 && ! p.isLayerCustom (cb::Layer::Arp),
           "browser while playing: the pick applied (Alternate 1-5) and reset the custom arp");
    check (countOns (after, 0, std::numeric_limits<juce::int64>::max(), 4) > 0 && stray == 0,
           "browser while playing: no channel-4 note-on of the old notes after the pick", str (stray, " stray"));
    rig.head.playing = false;
    rig.run (4);
}

//==============================================================================
// Export panel (final-spec 7.10) in the real editor.
void testExportPanel()
{
    namespace ep = cb::ui::exportpanel;
    ChordbloomProcessor p;
    p.setProgression (makeProgression ({ Am_, F_, C_, G_ }));
    std::unique_ptr<juce::AudioProcessorEditor> holder (p.createEditor());
    auto* ed = dynamic_cast<ChordbloomEditor*> (holder.get());

    if (ed == nullptr)
    {
        check (false, "export: editor");
        return;
    }

    auto& panel = ed->getExportPanel();
    const Part parts[] = { Part::Chords, Part::Bass, Part::Melody, Part::Arp, Part::All };

    // The documented font rule, recomputed here independently of ExportPanel.cpp.
    auto ruleContent = [] (float w, const juce::String& label, bool off)
    {
        const float size = w >= 76.0f ? 13.0f : (w >= 64.0f ? 12.0f : 11.5f);
        const float gap = w >= 76.0f ? 8.0f : (w >= 64.0f ? 6.0f : 5.0f);
        const float badgeW = cb::ui::textWidth (cb::ui::font (9.0f, 700).withExtraKerningFactor (0.08f), "OFF") + 8.0f;
        const float plain = 8.0f + gap + cb::ui::textWidth (cb::ui::font (size, 600), label);
        const bool badge = off && w >= 70.0f && plain + 6.0f + badgeW <= w - 12.0f;   // the badge only where it fits
        return plain + (badge ? 6.0f + badgeW : 0.0f);
    };

    for (const auto [w, h] : { std::pair<int, int> { 960, 560 }, std::pair<int, int> { 1100, 690 }, std::pair<int, int> { 1600, 1000 } })
    {
        ed->setSize (w, h);
        pumpMessages (40);
        ed->updateChordDisplays();
        const auto local = panel.getLocalBounds();
        const auto saveArea = panel.getSaveButtonBounds();
        const std::string at = str (" at ", w, "x", h);
        bool inside = true, noOverlap = true, wideEnough = true, ruleOk = true, offRuleOk = true;
        int minW = 1000, maxW = 0, chipTop = 1000;
        std::string widths, ruleDetail;

        for (size_t i = 0; i < std::size (parts); ++i)
        {
            const auto b = panel.getChipBounds (parts[i]);
            const auto label = juce::String (parts[i] == Part::All ? "All" : (parts[i] == Part::Arp ? "Arp" : (parts[i] == Part::Melody ? "Melody" : (parts[i] == Part::Bass ? "Bass" : "Chords"))));
            inside = inside && local.contains (b) && ! b.isEmpty();
            wideEnough = wideEnough && b.getWidth() >= 56;
            minW = std::min (minW, b.getWidth());
            maxW = std::max (maxW, b.getWidth());
            chipTop = std::min (chipTop, b.getY());
            widths += str (b.getWidth(), " ");

            for (size_t j = i + 1; j < std::size (parts); ++j)
                noOverlap = noOverlap && ! b.intersects (panel.getChipBounds (parts[j]));

            noOverlap = noOverlap && ! b.intersects (saveArea);
            const float cw = (float) b.getWidth();
            const float normal = ruleContent (cw, label, false);
            const float off = ruleContent (cw, label, true);
            ruleOk = ruleOk && normal <= cw - 12.0f && std::abs (normal - ep::chipContentWidth (cw, label, false)) < 0.01f;
            offRuleOk = offRuleOk && off <= cw - 12.0f && std::abs (off - ep::chipContentWidth (cw, label, true)) < 0.01f;
            ruleDetail += str (label.toStdString(), " ", normal, "/", off, " in ", cw - 12.0f, "; ");
        }

        check (inside && noOverlap, "export: five chips inside the card, not overlapping each other or Save" + at);
        check (maxW - minW <= 1, "export: the five chips are equal (within 1 px)" + at, widths);
        check (panel.getChipBounds (Part::Both).isEmpty(), "export: Both has no chip (it stays in the Save menu)" + at);
        check (! saveArea.isEmpty() && local.contains (saveArea) && saveArea.getBottom() <= chipTop,
               "export: the Save pill sits in the caption row above the chips" + at);

        if (w == 960)
            check (wideEnough, "export: every chip is >= 56 px wide at 960x560", widths);

        check (ruleOk, "export: grip + gap + label <= chip width - 12 for every chip (normal state)" + at, ruleDetail);
        check (offRuleOk, "export: the same with the OFF / disabled state (no badge below 70 px)" + at, ruleDetail);
    }

    ed->setSize (960, 560);
    pumpMessages (30);
    ed->updateChordDisplays();

    auto chip = [ed] (const char* title) { return findComponentByTitle (*ed, juce::String (title) + " MIDI clip"); };
    auto tip = [] (juce::Component* c)
    {
        auto* client = dynamic_cast<juce::TooltipClient*> (c);
        return client != nullptr ? client->getTooltip() : juce::String();
    };

    check (chip ("Arp") != nullptr && chip ("Arp")->isEnabled() && getParam (p, cb::ParamIDs::arpOn) < 0.5f,
           "export: the Arp chip is enabled with chords while the arp is off");
    check (tip (chip ("Arp")).contains ("muted in Chordbloom") && tip (chip ("Arp")).contains ("Up 3, 1/16"),
           "export: the Arp chip tooltip names the pattern and says it is muted", tip (chip ("Arp")).toStdString());

    // Automation of arpOn / bassOn: the chips and tooltips follow through the serial poll.
    setParam (p, cb::ParamIDs::arpOn, 1.0f);
    setParam (p, cb::ParamIDs::bassOn, 0.0f);
    pumpMessages (120);
    check (! tip (chip ("Arp")).contains ("muted") && tip (chip ("Bass")).contains (", muted in Chordbloom"),
           "export: arpOn / bassOn automation updates the chip tooltips", (tip (chip ("Arp")) + " | " + tip (chip ("Bass"))).toStdString());
    setParam (p, cb::ParamIDs::bassOn, 1.0f);

    // Bass Off: OFF and refused.
    setParam (p, cb::ParamIDs::bassMode, 0.0f);
    pumpMessages (120);
    check (tip (chip ("Bass")).contains ("Bass is Off") && ! p.writeMidiToTempFile (Part::Bass).existsAsFile(),
           "export: the Bass chip refuses with bassMode Off", tip (chip ("Bass")).toStdString());

    // An edited bass with the mode Off is silent (2.3) but still exports its notes: enabled, tooltip says muted.
    {
        setParam (p, cb::ParamIDs::bassMode, 1.0f);
        pumpMessages (60);
        auto bass = p.getLayerNotes (cb::Layer::Bass);

        if (! bass.notes.empty())
            bass.notes.erase (bass.notes.begin());

        p.setLayerNotes (cb::Layer::Bass, bass);
        setParam (p, cb::ParamIDs::bassMode, 0.0f);
        pumpMessages (120);
        const auto t = tip (chip ("Bass"));
        check (p.isLayerCustom (cb::Layer::Bass) && ! p.isLayerOn (cb::Layer::Bass) && chip ("Bass")->isEnabled()
                   && t.contains ("edited") && t.contains (", muted in Chordbloom")
                   && p.writeMidiToTempFile (Part::Bass).existsAsFile(),
               "export: an edited bass with mode Off exports, and its tooltip says muted", t.toStdString());
        p.resetLayer (cb::Layer::Bass);
        setParam (p, cb::ParamIDs::bassMode, 0.0f);
        pumpMessages (60);
    }

    // Menus
    {
        const auto save = menuEntries (panel.createSaveMenu());
        const auto texts = itemTexts (save);
        check (texts.size() == 6 && texts[0].startsWith ("All parts") && texts[1].startsWith ("Chords + bass")
                   && texts[2] == "Chords only" && texts[3].startsWith ("Bass only") && texts[4].startsWith ("Melody only")
                   && texts[5] == "Arp only",
               "export: the Save menu lists All parts / Chords + bass / Chords / Bass / Melody / Arp", entriesToString (save));
        auto enabledOf = [&save] (const juce::String& prefix)
        {
            for (const auto& e : save)
                if (! e.header && e.text.startsWith (prefix))
                    return e.enabled;

            return false;
        };

        check (! enabledOf ("Bass only") && ! enabledOf ("Chords + bass") && enabledOf ("Chords only") && enabledOf ("All parts")
                   && enabledOf ("Arp only") && ! enabledOf ("Melody only"),
               "export: with bassMode Off and no melody, Bass / Chords + bass / Melody are disabled", entriesToString (save));

        setParam (p, cb::ParamIDs::bassMode, 1.0f);
        pumpMessages (60);
        bool allChips = true;
        std::string detail;

        for (auto part : parts)
        {
            const auto m = itemTexts (menuEntries (panel.createChipMenu (part)));
            detail += m.joinIntoString (", ").toStdString() + "; ";
            allChips = allChips && m.size() == 1 && m[0].startsWith ("Save ") && m[0].contains (" as .mid");
        }

        check (allChips && itemTexts (menuEntries (panel.createChipMenu (Part::Arp)))[0].startsWith ("Save Arp as .mid"),
               "export: a chip right-click menu has one item \"Save <Part> as .mid...\"", detail);
    }

    // All writes the playing parts (Chords + Bass + Melody + Arp here).
    {
        p.setMelody (makeMelody ({ melodyNote (76, 0.0, 1.0), melodyNote (72, 4.0, 1.0) }));
        pumpMessages (60);
        const auto file = p.writeMidiToTempFile (Part::All);
        juce::MidiFile mf;
        juce::FileInputStream in (file);
        const bool read = in.openedOk() && mf.readFrom (in);
        check (read && mf.getNumTracks() == 5, "export: All writes tempo + Chords, Bass, Melody, Arp tracks",
               str (mf.getNumTracks(), " tracks"));
        setParam (p, cb::ParamIDs::arpOn, 0.0f);
        pumpMessages (60);
        check (tip (chip ("All")).startsWith ("Chords, Bass, Melody: 3 tracks"), "export: the All tooltip lists the playing parts",
               tip (chip ("All")).toStdString());
    }
}

//==============================================================================
// Controls (final-spec 7.11) in the real editor.
void testControlsPanel()
{
    ChordbloomProcessor p;
    p.setProgression (makeProgression ({ Am_, F_, C_, G_ }));
    std::unique_ptr<juce::AudioProcessorEditor> holder (p.createEditor());
    auto* ed = dynamic_cast<ChordbloomEditor*> (holder.get());

    if (ed == nullptr)
    {
        check (false, "controls: editor");
        return;
    }

    pumpMessages (40);
    auto& controls = ed->getControlsPanel();

    // User pick on a custom bass: reset (one undo step) + bassOn; automation never resets; Off only mutes.
    setParam (p, cb::ParamIDs::bassMode, 1.0f);
    check (makeCustom (p, cb::Layer::Bass), "controls setup: custom bass");
    setParam (p, cb::ParamIDs::bassOn, 0.0f);
    pumpMessages (120);
    check (controls.getBassStateText() == "edited, muted", "controls: \"edited, muted\" after automation (no change message)",
           controls.getBassStateText().toStdString());

    const auto custom = p.getLayerNotes (cb::Layer::Bass);
    setParam (p, cb::ParamIDs::bassMode, (float) cb::BassMode::Gallop);   // host automation
    pumpMessages (60);
    check (p.isLayerCustom (cb::Layer::Bass) && p.getLayerNotes (cb::Layer::Bass) == custom,
           "controls: host automation of bassMode while custom resets nothing");

    controls.pickBassModeAsUser (cb::BassMode::Off);
    check (p.isLayerCustom (cb::Layer::Bass) && paramInt (p, cb::ParamIDs::bassMode) == 0,
           "controls: picking Off only sets Off (custom notes kept)");

    controls.pickBassModeAsUser (cb::BassMode::Offbeat8);
    check (! p.isLayerCustom (cb::Layer::Bass) && getParam (p, cb::ParamIDs::bassOn) >= 0.5f
               && paramInt (p, cb::ParamIDs::bassMode) == (int) cb::BassMode::Offbeat8,
           "controls: a user pick resets the custom bass and switches it on");
    p.undo();
    check (p.isLayerCustom (cb::Layer::Bass) && p.getLayerNotes (cb::Layer::Bass) == custom,
           "controls: the reset was one undo step");
    pumpMessages (120);
    check (controls.getBassStateText().startsWith ("edited"), "controls: \"edited\" again after the undo");

    // A dismissed Bass popup changes nothing; a later automation onChange resets nothing.
    if (auto* combo = dynamic_cast<juce::ComboBox*> (findComponentByTitle (*ed, "Bass line")))
    {
        check (combo->getNumItems() == cb::numBassModes, "controls: the Bass combo lists the 15 modes",
               str (combo->getNumItems()));
        const int modeBefore = paramInt (p, cb::ParamIDs::bassMode);
        combo->showPopup();
        pumpMessages (60);
        juce::PopupMenu::dismissAllActiveMenus();
        pumpMessages (60);
        check (paramInt (p, cb::ParamIDs::bassMode) == modeBefore && p.isLayerCustom (cb::Layer::Bass),
               "controls: a dismissed Bass popup changes nothing");
        setParam (p, cb::ParamIDs::bassMode, (float) cb::BassMode::Rolling16);
        pumpMessages (60);
        check (p.isLayerCustom (cb::Layer::Bass) && combo->getSelectedId() == (int) cb::BassMode::Rolling16 + 1,
               "controls: a later automation change updates the combo and resets nothing");
    }
    else
    {
        check (false, "controls: the Bass combo found by its title");
    }

    // Rate / Swing dimming (final-spec 7.11): arp on + Block chords + held bass: neither dimmed; 1/8T: swing dimmed.
    auto* rate = findComponentByTitle (*ed, "Rate");
    auto* swingSlider = findComponentByTitle (*ed, "Swing");
    auto* swing = swingSlider != nullptr ? swingSlider->getParentComponent() : nullptr;

    if (rate != nullptr && swing != nullptr)
    {
        p.resetLayer (cb::Layer::Bass);
        setParam (p, cb::ParamIDs::pattern, 0.0f);
        setParam (p, cb::ParamIDs::bassMode, 1.0f);
        setParam (p, cb::ParamIDs::rate, 1.0f);
        setParam (p, cb::ParamIDs::arpOn, 0.0f);
        pumpMessages (120);
        check (rate->getAlpha() < 1.0f && swing->getAlpha() < 1.0f, "controls: Block + Root bass + arp off: Rate and Swing dimmed");
        setParam (p, cb::ParamIDs::arpOn, 1.0f);   // automation: no change message
        pumpMessages (120);
        check (rate->getAlpha() == 1.0f && swing->getAlpha() == 1.0f,
               "controls: arp on (automation) un-dims Rate and Swing (the arp swings on the Rate grid)");
        setParam (p, cb::ParamIDs::rate, 3.0f);
        pumpMessages (120);
        check (rate->getAlpha() == 1.0f && swing->getAlpha() < 1.0f, "controls: at 1/8T Swing is dimmed (no swing on triplets)");
        setParam (p, cb::ParamIDs::arpOn, 0.0f);
        setParam (p, cb::ParamIDs::bassMode, (float) cb::BassMode::Tresillo);
        setParam (p, cb::ParamIDs::rate, 2.0f);
        pumpMessages (120);
        check (rate->getAlpha() == 1.0f && swing->getAlpha() == 1.0f, "controls: a bass pattern un-dims Rate and Swing too");
    }
    else
    {
        check (false, "controls: Rate and Swing found by their titles");
    }
}

//==============================================================================
// Header, view switch, keyboard focus and (after WP5) the toolbar inside the real roll.
void testShellViewAndFocus()
{
    ChordbloomProcessor p;
    p.setProgression (makeProgression ({ Am_, F_, C_, G_ }));
    std::unique_ptr<juce::AudioProcessorEditor> holder (p.createEditor());
    auto* ed = dynamic_cast<ChordbloomEditor*> (holder.get());

    if (ed == nullptr)
    {
        check (false, "shell: editor");
        return;
    }

    pumpMessages (40);
    auto* view = findComponentByTitle (*ed, "View");
    auto valueOf = [] (juce::Component* c)
    {
        auto handler = c != nullptr ? c->createAccessibilityHandler() : nullptr;
        auto* value = handler != nullptr ? handler->getValueInterface() : nullptr;
        return value != nullptr ? value->getCurrentValueAsString() : juce::String();
    };
    auto dot = [view] { return view != nullptr && (bool) view->getProperties().getWithDefault ("cb.rollHasContent", false); };

    ed->setMelodyViewVisible (false);
    pumpMessages (40);
    check (valueOf (view) == "Chords" && ! dot(), "view switch: Chords, no dot without roll content");
    p.setMelody (makeMelody ({ melodyNote (72, 0.0, 1.0) }));
    pumpMessages (80);
    check (dot(), "view switch: the dot shows while the hidden roll has melody notes");
    p.clearMelody();
    makeCustom (p, cb::Layer::Arp);
    pumpMessages (80);
    check (dot(), "view switch: the dot shows for a custom layer too");
    p.resetLayer (cb::Layer::Arp);
    pumpMessages (80);
    check (! dot(), "view switch: the dot goes when nothing is left");
    ed->setMelodyViewVisible (true);
    pumpMessages (40);
    check (valueOf (view) == "Roll", "view switch: reads \"Roll\"", valueOf (view).toStdString());
    check (p.getAPVTS().state.getProperty ("cb.editorView").toString() == "melody", "view switch: cb.editorView stays \"melody\"");

    // Keyboard focus: nothing in the editor (roll + toolbar included) wants it.
    {
        int focusable = 0;
        std::function<void (juce::Component&)> visit = [&] (juce::Component& c)
        {
            if (c.getWantsKeyboardFocus() && dynamic_cast<juce::TextEditor*> (&c) == nullptr)
                ++focusable;

            for (auto* child : c.getChildren())
                visit (*child);
        };
        visit (*ed);
        check (focusable == 0 && ! ed->getMelodyRoll().getToolbar().getWantsKeyboardFocus(),
               "shell: no component wants keyboard focus (roll view)", str (focusable, " focusable"));
    }

    // The toolbar inside the roll (after WP5 hosts it): levels, inside the roll and left of the view switch.
    auto& roll = ed->getMelodyRoll();
    auto& tb = roll.getToolbar();

    if (tb.getParentComponent() != &roll)
    {
        std::cout << "SKIP toolbar in the real editor: MelodyRoll does not host the RollToolbar yet (after WP5)\n";
        return;
    }

    for (const auto [w, h] : { std::pair<int, int> { 960, 560 }, std::pair<int, int> { 1100, 690 }, std::pair<int, int> { 1600, 1000 } })
    {
        ed->setSize (w, h);
        pumpMessages (40);

        for (auto layer : { cb::Layer::Melody, cb::Layer::Arp, cb::Layer::Bass })
        {
            for (bool custom : { false, true })
            {
                if (layer == cb::Layer::Melody && custom)
                    continue;

                p.resetLayer (cb::Layer::Arp);
                p.resetLayer (cb::Layer::Bass);

                if (custom)
                    makeCustom (p, layer);

                roll.setLayer (layer);
                pumpMessages (60);
                const auto r = inspectToolbar (tb, layer, custom);
                const auto switchLeft = view != nullptr ? view->getX() : ed->getWidth();
                bool leftOfSwitch = true;

                for (int i = 0; i < (int) TI::NumItems; ++i)
                {
                    const auto b = tb.getItemBounds ((TI) i);

                    if (! b.isEmpty())
                        leftOfSwitch = leftOfSwitch && ed->getLocalArea (&tb, b).getRight() <= switchLeft;
                }

                const std::string at = str (" (", cb::layerName (layer), custom ? " custom" : "", " at ", w, "x", h, ", level ", r.level, ")");
                check (! r.overlap && ! r.outside && leftOfSwitch, "toolbar in the editor: no overlap, inside, left of the view switch" + at,
                       r.detail);
                check (w == 960 ? r.level <= 3 : (w == 1600 || layer == cb::Layer::Melody ? r.level == 0 : r.level <= 1),
                       "toolbar in the editor: level as budgeted" + at);
            }
        }
    }
}
} // namespace

void cbtest::runShellSelfTests()
{
    testWp0ShellHooks();
    testEditorSize();
    testEditorInteraction();
    testUserDefaults();
    testEditorAuditionsAndWheel();
    testRollToolbarLayout();
    testRollToolbarInteraction();
    testBrowserPickWhilePlaying();
    testExportPanel();
    testControlsPanel();
    testShellViewAndFocus();
}
