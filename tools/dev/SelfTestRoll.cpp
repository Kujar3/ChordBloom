#include "SelfTestUtil.h"

#include <typeinfo>

// Roll selftests (owner: WP5; final-spec 8.2 / 9 WP5): the roll canvas, layers, selection, commands, velocity lane.
// Registered in runSelfTest (SelfTest.cpp) by WP0.

using namespace cbtest;

namespace
{
// The melody roll driven with synthetic mouse events, like a user would.
void testMelodyRollEditing()
{
    const auto left = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier);
    const auto right = juce::ModifierKeys (juce::ModifierKeys::rightButtonModifier);
    const auto leftAlt = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::altModifier);

    ChordbloomProcessor p;
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));   // C major, 16 beats

    std::unique_ptr<juce::AudioProcessorEditor> holder (p.createEditor());
    auto* ed = dynamic_cast<ChordbloomEditor*> (holder.get());

    if (ed == nullptr)
    {
        check (false, "roll: editor");
        return;
    }

    ed->setSize (ChordbloomEditor::kDefaultWidth, ChordbloomEditor::kDefaultHeight);
    pumpMessages (40);
    check (! ed->isMelodyViewVisible() && ed->getPaletteView().isVisible(), "roll: a new editor opens on the chord palette");
    ed->setMelodyViewVisible (true);
    auto& roll = ed->getMelodyRoll();
    check (ed->isMelodyViewVisible() && roll.isVisible() && ! ed->getPaletteView().isVisible()
               && roll.getBounds() == ed->getPaletteView().getBounds(),
           "roll: the Melody view shows the roll in the palette's place");

    // The roll's beats line up with the lane's chord blocks (same x range and scale as the lane's track)
    {
        const auto lane = ed->getProgressionLane().getBounds();
        const float beat0 = (float) roll.getX() + roll.getCellCentre (0.0, 72).x;
        const float beat16 = (float) roll.getX() + roll.getCellCentre (16.0, 72).x;
        check (std::abs (beat0 - (float) (lane.getX() + 12)) < 0.01f && std::abs (beat16 - (float) (lane.getRight() - 12)) < 0.01f,
               "roll: beat axis = the lane's track (chords line up under the notes)", str (beat0, " ", beat16));
    }

    auto* surface = roll.getChildComponent (0);
    auto event = [surface] (juce::Point<float> pos, juce::ModifierKeys mods, juce::Point<float> downPos)
    {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos, mods, juce::MouseInputSource::defaultPressure,
                                 0.0f, 0.0f, 0.0f, 0.0f, surface, surface, now, downPos, now, 1, pos != downPos);
    };
    auto click = [&] (juce::Point<float> pos, juce::ModifierKeys mods)
    {
        surface->mouseDown (event (pos, mods, pos));
        surface->mouseUp (event (pos, mods, pos));
    };
    auto drag = [&] (juce::Point<float> from, juce::Point<float> to, juce::ModifierKeys mods)
    {
        surface->mouseDown (event (from, mods, from));
        for (int i = 1; i <= 4; ++i)
            surface->mouseDrag (event (from + (to - from) * ((float) i / 4.0f), mods, from));
        surface->mouseUp (event (to, mods, from));
    };
    auto melodyText = [&p]
    {
        std::ostringstream ss;
        for (auto& n : p.getMelody().notes)
            ss << n.note << "@" << n.startBeat << "+" << n.lengthBeats << " ";
        return ss.str();
    };

    roll.setSnap (cb::MelodySnap::Scale);
    roll.setGrid (cb::RateDivision::Eighth);
    roll.setTopNote (78);
    check (roll.getTopNote() == 78 && roll.getBottomNote() <= 62, "roll: pitch range scrolls (78 down to 62 or lower visible)",
           str (roll.getTopNote(), "..", roll.getBottomNote()));

    // Draw: click on F# at beat 1.3 -> F (Scale snap in C major, ties go down) on the 1/8 grid line at beat 1
    {
        const auto pos = roll.getCellCentre (1.3, 66);
        surface->mouseDown (event (pos, left, pos));
        const auto auditioned = p.getPreviewNotes();
        surface->mouseUp (event (pos, left, pos));
        const auto& m = p.getMelody();
        check (m.notes.size() == 1 && m.notes[0].note == 65 && m.notes[0].startBeat == 1.0 && m.notes[0].lengthBeats == 1.0,
               "roll: click draws a scale-snapped note on the grid (F at beat 1, one beat)", melodyText());
        check (auditioned == std::vector<int> { 65 } && p.getPreviewNotes().empty(), "roll: the drawn note is auditioned while held");
        check (p.canUndo(), "roll: drawing is undoable");
    }

    // Move: drag it two beats later and up to A
    drag (roll.getCellCentre (1.5, 65), roll.getCellCentre (3.5, 69), left);
    check (p.getMelody().notes.size() == 1 && p.getMelody().notes[0].note == 69 && p.getMelody().notes[0].startBeat == 3.0,
           "roll: dragging a note moves it by grid steps and rows", melodyText());
    p.undo();
    check (p.getMelody().notes.size() == 1 && p.getMelody().notes[0].note == 65 && p.getMelody().notes[0].startBeat == 1.0,
           "roll: one undo step per drag");
    p.redo();

    // Chord snap: moved onto B over Am (A C E) -> C
    roll.setSnap (cb::MelodySnap::ChordTones);
    drag (roll.getCellCentre (3.5, 69), roll.getCellCentre (5.5, 71), left);
    check (p.getMelody().notes.size() == 1 && p.getMelody().notes[0].note == 72 && p.getMelody().notes[0].startBeat == 5.0,
           "roll: Chord snap moves the note to the nearest tone of the chord under it", melodyText());

    // Resize: drag the right edge to beat ~7.3 -> 7.5 on the grid
    {
        const auto r = roll.getNoteBounds (0);
        drag ({ r.getRight() - 2.0f, r.getCentreY() }, { roll.getCellCentre (7.3, 72).x, r.getCentreY() }, left);
        check (p.getMelody().notes.size() == 1 && p.getMelody().notes[0].lengthBeats == 2.5,
               "roll: dragging the right edge resizes on the grid", melodyText());
    }

    // The next drawn note takes the last length; Alt places it off the grid
    roll.setSnap (cb::MelodySnap::Off);
    click (roll.getCellCentre (9.39, 76), leftAlt);
    {
        const auto& m = p.getMelody();
        check (m.notes.size() == 2 && m.notes[1].note == 76 && std::abs (m.notes[1].startBeat - 9.375) < 1e-9
                   && m.notes[1].lengthBeats == 2.5,
               "roll: a new note takes the last length; Alt draws off the grid (1/48 beat)", melodyText());
    }
    click (roll.getCellCentre (12.2, 63), left);   // Snap Off: D# stays D#
    check (p.getMelody().notes.size() == 3 && p.getMelody().notes[2].note == 63 && p.getMelody().notes[2].startBeat == 12.0,
           "roll: Snap Off keeps any pitch", melodyText());

    // Right-click deletes a note; a right-drag deletes every note it crosses
    {
        const auto r = roll.getNoteBounds (2);
        click (r.getCentre(), right);
        check (p.getMelody().notes.size() == 2, "roll: right-click deletes a note", melodyText());
        p.undo();
        check (p.getMelody().notes.size() == 3, "roll: deleting is undoable");

        const auto a = roll.getNoteBounds (0), c = roll.getNoteBounds (2);
        drag (a.getCentre(), c.getCentre(), right);   // from the C at beat 5 down to the D# at beat 12: the E above stays
        check (p.getMelody().notes.size() == 1 && p.getMelody().notes[0].note == 76,
               "roll: a right-drag erases the notes it crosses (and only those)", melodyText());
        p.undo();
        check (p.getMelody().notes.size() == 3, "roll: the erase undoes in one step");
    }

    // Mouse wheel scrolls the pitch range
    {
        const int top = roll.getTopNote();
        juce::MouseWheelDetails notch {};
        notch.deltaY = 0.234f;
        const auto pos = roll.getCellCentre (2.0, top - 3);
        surface->mouseWheelMove (event (pos, {}, pos), notch);
        check (roll.getTopNote() == top + 2, "roll: one wheel notch scrolls up two rows", str (top, " -> ", roll.getTopNote()));
    }

    // Export chip follows the melody; Clear empties it (undoable)
    {
        pumpMessages (40);
        ed->updateChordDisplays();
        auto* chip = findComponentByTitle (*ed, "Melody MIDI clip");
        check (chip != nullptr && chip->isEnabled(), "roll: the Melody chip is enabled with a melody in the loop");
        const bool cleared = roll.performCommand (MelodyRoll::Command::ClearMelody);
        pumpMessages (40);
        ed->updateChordDisplays();
        check (cleared && p.getMelody().empty(), "roll: Clear empties the melody");
        check (chip != nullptr && ! chip->isEnabled(), "roll: the Melody chip is disabled without a melody");
        p.undo();
        ed->updateChordDisplays();
    }

    // Snap, grid, the view and the melody are part of the plugin state
    roll.setSnap (cb::MelodySnap::ChordTones);
    roll.setGrid (cb::RateDivision::Sixteenth);
    juce::MemoryBlock state;
    p.getStateInformation (state);
    holder.reset();
    check (p.getPreviewNotes().empty(), "roll: no audition left after closing the editor");

    ChordbloomProcessor q;
    q.setStateInformation (state.getData(), (int) state.getSize());
    std::unique_ptr<juce::AudioProcessorEditor> holder2 (q.createEditor());
    auto* ed2 = dynamic_cast<ChordbloomEditor*> (holder2.get());
    check (ed2 != nullptr && ed2->isMelodyViewVisible() && ed2->getMelodyRoll().getSnap() == cb::MelodySnap::ChordTones
               && ed2->getMelodyRoll().getGrid() == cb::RateDivision::Sixteenth && q.getMelody() == p.getMelody(),
           "roll: view, snap, grid and melody come back with the plugin state");
}

//==============================================================================
// Phase 2 (WP5): layers, selection, gestures, commands, menus, pills, velocity lane.
using Layer = cb::Layer;
using Command = MelodyRoll::Command;
using Section = MelodyRoll::MenuSection;

const auto kLeft = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier);
const auto kRight = juce::ModifierKeys (juce::ModifierKeys::rightButtonModifier);
const auto kCtrl = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::ctrlModifier);
const auto kCtrlShift = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::ctrlModifier
                                            | juce::ModifierKeys::shiftModifier);
const auto kShift = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier);

// An editor showing the roll, driven with synthetic mouse events on the grid surface (child 0).
struct RollRig
{
    explicit RollRig (int width = ChordbloomEditor::kDefaultWidth, int height = ChordbloomEditor::kDefaultHeight,
                      std::initializer_list<cb::Chord> chords = { C_, Am_, F_, G_ })
    {
        if (chords.size() > 0)
            p.setProgression (makeProgression (chords));

        holder.reset (p.createEditor());
        ed = dynamic_cast<ChordbloomEditor*> (holder.get());

        if (ed != nullptr)
        {
            ed->setSize (width, height);
            pumpMessages (30);
            ed->setMelodyViewVisible (true);
            roll = &ed->getMelodyRoll();
            surface = roll->getChildComponent (0);
            pumpMessages (30);
        }
    }

    ~RollRig()
    {
        juce::PopupMenu::dismissAllActiveMenus();
        pumpMessages (20);
    }

    bool ok() const { return ed != nullptr && roll != nullptr && surface != nullptr; }

    juce::MouseEvent event (juce::Point<float> pos, juce::ModifierKeys mods, juce::Point<float> downPos) const
    {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos, mods, juce::MouseInputSource::defaultPressure,
                                 0.0f, 0.0f, 0.0f, 0.0f, surface, surface, now, downPos, now, 1, pos != downPos);
    }

    void down (juce::Point<float> pos, juce::ModifierKeys mods)
    {
        downAt = pos;
        surface->mouseDown (event (pos, mods, pos));
    }

    void move (juce::Point<float> pos, juce::ModifierKeys mods) { surface->mouseDrag (event (pos, mods, downAt)); }
    void up (juce::Point<float> pos, juce::ModifierKeys mods) { surface->mouseUp (event (pos, mods, downAt)); }

    void click (juce::Point<float> pos, juce::ModifierKeys mods)
    {
        down (pos, mods);
        up (pos, mods);
    }

    void drag (juce::Point<float> from, juce::Point<float> to, juce::ModifierKeys mods)
    {
        down (from, mods);
        for (int i = 1; i <= 4; ++i)
            move (from + (to - from) * ((float) i / 4.0f), mods);
        up (to, mods);
    }

    juce::Point<float> cell (double beat, int note) const { return roll->getCellCentre (beat, note); }
    // Centre of a note's body (left part, clear of the resize edge).
    juce::Point<float> noteBody (int index) const
    {
        const auto r = roll->getNoteBounds (index);
        return { r.getX() + std::min (6.0f, r.getWidth() * 0.3f), r.getCentreY() };
    }
    juce::Point<float> noteEdge (int index) const
    {
        const auto r = roll->getNoteBounds (index);
        return { r.getRight() - 1.5f, r.getCentreY() };
    }

    ChordbloomProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> holder;
    ChordbloomEditor* ed = nullptr;
    MelodyRoll* roll = nullptr;
    juce::Component* surface = nullptr;
    juce::Point<float> downAt;
};

// Number of undo steps (undoes everything, then redoes it all back).
int undoDepth (ChordbloomProcessor& p)
{
    int n = 0;
    while (p.undo())
        ++n;
    for (int i = 0; i < n; ++i)
        p.redo();
    return n;
}

std::string layerText (const cb::Melody& m)
{
    std::ostringstream ss;
    for (auto& n : m.notes)
        ss << n.note << "@" << n.startBeat << "+" << n.lengthBeats << "v" << n.velocity << " ";
    return ss.str();
}

std::string selText (const std::vector<int>& s)
{
    std::ostringstream ss;
    ss << "{";
    for (size_t i = 0; i < s.size(); ++i)
        ss << (i > 0 ? "," : "") << s[i];
    ss << "}";
    return ss.str();
}

struct MenuEntry
{
    juce::String text;
    bool enabled = false, ticked = false, header = false, hasSub = false;
    int id = 0;
};

std::vector<MenuEntry> menuEntries (const juce::PopupMenu& menu, bool recurse = false)
{
    std::vector<MenuEntry> out;
    for (juce::PopupMenu::MenuItemIterator it (menu, recurse); it.next();)
    {
        const auto& item = it.getItem();
        if (item.isSeparator)
            continue;
        out.push_back ({ item.text, item.isEnabled, item.isTicked, item.isSectionHeader, item.subMenu != nullptr, item.itemID });
    }
    return out;
}

const MenuEntry* findEntry (const std::vector<MenuEntry>& entries, const juce::String& text)
{
    for (auto& e : entries)
        if (e.text == text)
            return &e;
    return nullptr;
}

bool hasEntries (const std::vector<MenuEntry>& entries, std::initializer_list<const char*> texts, std::string& missing)
{
    for (auto* t : texts)
        if (findEntry (entries, t) == nullptr)
        {
            missing += std::string (t) + "; ";
        }
    return missing.empty();
}

const juce::PopupMenu* subMenuOf (const juce::PopupMenu& menu, const juce::String& text)
{
    for (juce::PopupMenu::MenuItemIterator it (menu, false); it.next();)
        if (it.getItem().text == text)
            return it.getItem().subMenu.get();
    return nullptr;
}

cb::Melody fourNotes()
{
    return makeMelody ({ melodyNote (60, 0.0, 1.0), melodyNote (64, 2.0, 1.0), melodyNote (67, 4.0, 1.0), melodyNote (72, 8.0, 1.0) });
}

void forEachComponent (juce::Component& root, const std::function<void (juce::Component&)>& fn)
{
    fn (root);
    for (auto* c : root.getChildren())
        forEachComponent (*c, fn);
}

//==============================================================================
void testRollStructureAndLayers()
{
    RollRig rig;
    if (! rig.ok())
    {
        check (false, "roll2: editor");
        return;
    }

    auto& roll = *rig.roll;
    auto& p = rig.p;
    check (roll.getChildComponent (0) == rig.surface && roll.getChildComponent (1) == &roll.getToolbar(),
           "roll2: child 0 is the grid surface, child 1 the RollToolbar");

    {
        const auto tb = roll.getToolbar().getBounds();
        auto* viewSwitch = findComponentByTitle (*rig.ed, "View");
        const int switchLeft = viewSwitch != nullptr ? viewSwitch->getX() - roll.getX() : roll.getWidth();
        check (tb.getY() >= 10 && tb.getHeight() == 26 && tb.getX() == 12 && tb.getRight() <= switchLeft - 12 + 1
                   && tb.getRight() >= switchLeft - 14,
               "roll2: the toolbar fills the toolbar row left of the view switch",
               str (tb.toString(), " switch at ", switchLeft));
    }

    bool focusFree = true;
    forEachComponent (roll, [&focusFree] (juce::Component& c)
    {
        focusFree = focusFree && ! c.getWantsKeyboardFocus() && ! c.getMouseClickGrabsKeyboardFocus();
    });
    check (focusFree, "roll2: no component of the roll wants keyboard focus");

    // Layers: switching persists cb.rollLayer; each layer keeps its own snap, grid and top note; rows follow its limits
    check (roll.getLayer() == Layer::Melody && roll.getSnap() == cb::MelodySnap::Scale && roll.getGrid() == cb::RateDivision::Eighth,
           "roll2: the Melody layer opens with Scale snap and the 1/8 grid");
    roll.setTopNote (200);
    check (roll.getTopNote() == 96, "roll2: Melody rows end at MIDI 96", str (roll.getTopNote()));
    roll.setTopNote (80);

    roll.setLayer (Layer::Bass);
    const auto bassNotes = p.getLayerNotes (Layer::Bass);
    {
        int lo = 127, hi = 0;
        for (auto& n : bassNotes.notes)
        {
            lo = std::min (lo, n.note);
            hi = std::max (hi, n.note);
        }
        check (roll.getLayer() == Layer::Bass && p.getAPVTS().state.getProperty ("cb.rollLayer").toString() == "bass"
                   && roll.getSnap() == cb::MelodySnap::ChordTones && roll.getGrid() == cb::RateDivision::Sixteenth,
               "roll2: the Bass tab persists cb.rollLayer and opens with Chord snap and the 1/16 grid");
        check (! bassNotes.empty() && lo >= roll.getBottomNote() && hi <= roll.getTopNote(),
               "roll2: a layer shown the first time centres on its notes",
               str (lo, "..", hi, " in ", roll.getBottomNote(), "..", roll.getTopNote()));
    }
    roll.setTopNote (200);
    check (roll.getTopNote() == 72, "roll2: Bass rows end at MIDI 72", str (roll.getTopNote()));
    roll.setTopNote (0);
    check (roll.getBottomNote() == 24, "roll2: Bass rows go down to MIDI 24", str (roll.getBottomNote()));
    roll.setSnap (cb::MelodySnap::Off);
    roll.setGrid (cb::RateDivision::Quarter);
    const int bassTop = roll.getTopNote();

    roll.setLayer (Layer::Arp);
    roll.setTopNote (200);
    check (roll.getTopNote() == 108, "roll2: Arp rows end at MIDI 108", str (roll.getTopNote()));
    roll.setTopNote (0);
    check (roll.getBottomNote() == 28, "roll2: Arp rows go down to MIDI 28", str (roll.getBottomNote()));

    roll.setLayer (Layer::Melody);
    check (roll.getTopNote() == 80 && roll.getSnap() == cb::MelodySnap::Scale && roll.getGrid() == cb::RateDivision::Eighth,
           "roll2: back on Melody its own top note, snap and grid are kept", str (roll.getTopNote()));
    roll.setLayer (Layer::Bass);
    check (roll.getTopNote() == bassTop && roll.getSnap() == cb::MelodySnap::Off && roll.getGrid() == cb::RateDivision::Quarter,
           "roll2: back on Bass its settings are kept");

    juce::MemoryBlock state;
    p.getStateInformation (state);
    ChordbloomProcessor q;
    q.setStateInformation (state.getData(), (int) state.getSize());
    std::unique_ptr<juce::AudioProcessorEditor> holder2 (q.createEditor());
    if (auto* ed2 = dynamic_cast<ChordbloomEditor*> (holder2.get()))
    {
        auto& r2 = ed2->getMelodyRoll();
        check (r2.getLayer() == Layer::Bass && r2.getSnap() == cb::MelodySnap::Off && r2.getGrid() == cb::RateDivision::Quarter
                   && r2.getTopNote() == bassTop,
               "roll2: the layer and its view settings come back with the plugin state");
    }
    else
    {
        check (false, "roll2: second editor");
    }
}

//==============================================================================
void testRollAutoCustom()
{
    RollRig rig;
    if (! rig.ok())
        return;

    auto& roll = *rig.roll;
    auto& p = rig.p;
    roll.setLayer (Layer::Bass);
    pumpMessages (40);

    const int depth0 = undoDepth (p);
    const auto autoBass = p.getLayerNotes (Layer::Bass);

    // Drawing on the auto bass makes it custom (one undo step); Reset returns it to auto
    {
        int slotPitch = autoBass.notes.front().note;
        for (auto& n : autoBass.notes)
            if (n.startBeat == 4.0)
                slotPitch = n.note;
        const int row = slotPitch - 3 >= roll.getBottomNote() ? slotPitch - 3 : slotPitch + 3;
        const auto pos = rig.cell (6.1, row);
        rig.click (pos, kLeft);
        const auto now = p.getLayerNotes (Layer::Bass);
        check (p.isLayerCustom (Layer::Bass) && now.notes.size() == autoBass.notes.size() + 1 && undoDepth (p) == depth0 + 1,
               "roll2: drawing on the auto bass makes it custom in one undo step", layerText (now));
        bool found = false;
        for (auto& n : now.notes)
            found = found || (n.startBeat == 6.0 && n.lengthBeats == 0.25 && std::abs (n.velocity - getParam (p, cb::ParamIDs::velocity)) < 1e-6f);
        check (found, "roll2: a new bass note is one grid step long at the velocity parameter");
        check (roll.performCommand (Command::ResetLayer) && ! p.isLayerCustom (Layer::Bass) && p.getLayerNotes (Layer::Bass) == autoBass,
               "roll2: Reset makes the bass follow the chords again");
        check (! roll.performCommand (Command::ResetLayer), "roll2: Reset does not apply to an auto layer");
    }

    // A no-op click on an auto note commits nothing
    const int depthNow = undoDepth (p);
    rig.click (rig.noteBody (0), kLeft);
    check (! p.isLayerCustom (Layer::Bass) && undoDepth (p) == depthNow && p.getLayerNotes (Layer::Bass) == autoBass,
           "roll2: a click on an auto bass note commits nothing (the layer stays auto)");

    // Humanize 1: a no-op drag does not flip the layer
    {
        setParam (p, cb::ParamIDs::humanize, 1.0f);
        pumpMessages (120);
        const int depth = undoDepth (p);
        const auto from = rig.noteBody (1);
        rig.drag (from, from.translated (1.0f, 0.0f), kLeft);
        check (! p.isLayerCustom (Layer::Bass) && undoDepth (p) == depth,
               "roll2: with humanize 1 a no-op drag keeps the bass auto (no undo step)");
        setParam (p, cb::ParamIDs::humanize, 0.0f);
        pumpMessages (120);
    }

    // Automation of arpShape mid-drag: the mouse-down working copy still wins (the arp becomes custom)
    {
        setParam (p, cb::ParamIDs::arpOn, 1.0f);
        pumpMessages (120);
        roll.setLayer (Layer::Arp);
        roll.setSnap (cb::MelodySnap::Off);
        pumpMessages (40);
        const auto before = p.getLayerNotes (Layer::Arp);
        int index = -1;
        for (int i = 0; i < (int) before.notes.size() && index < 0; ++i)
            if (before.notes[(size_t) i].note > roll.getBottomNote() + 2 && before.notes[(size_t) i].note < roll.getTopNote() - 2)
                index = i;

        if (index >= 0)
        {
            const auto grabbed = before.notes[(size_t) index];
            const auto from = rig.noteBody (index);
            const auto to = rig.cell (grabbed.startBeat + 0.1, grabbed.note + 2);
            rig.down (from, kLeft);
            rig.move ({ from.x, to.y }, kLeft);
            setParam (p, cb::ParamIDs::arpShape, 3.0f);
            pumpMessages (150);
            const bool reRendered = p.getLayerNotes (Layer::Arp) != before;
            rig.up ({ from.x, to.y }, kLeft);
            const auto after = p.getLayerNotes (Layer::Arp);

            std::multiset<std::pair<int, double>> expected, got;
            for (int i = 0; i < (int) before.notes.size(); ++i)
                if (i != index)
                    expected.insert ({ before.notes[(size_t) i].note, before.notes[(size_t) i].startBeat });
            expected.insert ({ grabbed.note + 2, grabbed.startBeat });
            for (auto& n : after.notes)
                got.insert ({ n.note, n.startBeat });

            check (reRendered && p.isLayerCustom (Layer::Arp) && got == expected,
                   "roll2: an arpShape automation mid-drag still commits the working copy (custom)",
                   str (reRendered, " ", layerText (after)));
        }
        else
        {
            check (false, "roll2: an arp note in view");
        }

        setParam (p, cb::ParamIDs::arpShape, 0.0f);
        p.resetLayer (Layer::Arp);
    }

    // Editing an off layer works; the off pill turns it on, a right-click on it changes nothing
    {
        setParam (p, cb::ParamIDs::arpOn, 0.0f);
        pumpMessages (120);
        const auto pill = roll.getLayerOffPillArea();
        check (! pill.isEmpty(), "roll2: an off layer shows the '<Layer> is off' pill");
        rig.click (pill.getCentre(), kRight);
        check (! p.isLayerOn (Layer::Arp), "roll2: a right-click on the off pill does not turn the layer on");

        const auto before = p.getLayerNotes (Layer::Arp);
        int row = roll.getTopNote() - 1;
        for (int candidate = roll.getTopNote() - 1; candidate > roll.getBottomNote(); --candidate)
        {
            bool free = true;
            for (auto& n : before.notes)
                free = free && ! (n.note == candidate && n.startBeat <= 9.1 && n.startBeat + n.lengthBeats > 8.9);
            if (free)
            {
                row = candidate;
                break;
            }
        }
        rig.click (rig.cell (9.05, row), kLeft);
        check (p.isLayerCustom (Layer::Arp) && p.getLayerNotes (Layer::Arp).notes.size() == before.notes.size() + 1,
               "roll2: drawing on a muted arp works (it becomes custom)",
               str (p.isLayerCustom (Layer::Arp), " ", before.notes.size(), " -> ", p.getLayerNotes (Layer::Arp).notes.size(), " row ", row));

        rig.click (roll.getLayerOffPillArea().getCentre(), kLeft);
        pumpMessages (60);
        check (p.isLayerOn (Layer::Arp) && roll.getLayerOffPillArea().isEmpty(), "roll2: the off pill's click turns the arp on");
        p.resetLayer (Layer::Arp);
    }

    // Auditions sound on the layer's channel (bass: ch 2, melody: ch 3)
    {
        setParam (p, cb::ParamIDs::arpOn, 0.0f);
        roll.setLayer (Layer::Bass);
        pumpMessages (60);
        Rig audio (p, 48000.0, 512, false);
        audio.run (2);
        rig.down (rig.noteBody (0), kLeft);
        const auto evs = audio.run (2);
        rig.up (rig.noteBody (0), kLeft);
        const auto offs = audio.run (2);
        const int bassNote = p.getLayerNotes (Layer::Bass).notes.front().note;
        int bassOns = 0;
        for (auto& ev : evs)
            bassOns += ev.on && ev.channel == 2 && ev.note == bassNote ? 1 : 0;
        check (bassOns == 1 && countOns (evs, 0, std::numeric_limits<juce::int64>::max(), 3) == 0,
               "roll2: grabbing a bass note auditions it on MIDI channel 2");
        check (audio.openCount() == 0, "roll2: the bass audition stops on mouse-up", str (offs.size()));

        roll.setLayer (Layer::Melody);
        rig.down (rig.cell (1.2, 74), kLeft);
        const auto evs2 = audio.run (2);
        rig.up (rig.cell (1.2, 74), kLeft);
        audio.run (2);
        std::string evText;
        for (auto& ev : evs2)
            evText += str (ev.on ? "on" : "off", " ch", ev.channel, " n", ev.note, "; ");
        check (countOns (evs2, 0, std::numeric_limits<juce::int64>::max(), 3) == 1 && countOns (evs2, 0, std::numeric_limits<juce::int64>::max(), 2) == 0,
               "roll2: a drawn melody note auditions on MIDI channel 3",
               evText + layerText (p.getMelody()) + str (" layer ", (int) roll.getLayer(), " top ", roll.getTopNote(), " pos ",
                                                          rig.cell (1.2, 74).toString(), " grid ", roll.getGridArea().toString(),
                                                          " sel ", roll.getSelection().size(), " card ", roll.getEmptyStateButtonArea().toString()));
    }
}

//==============================================================================
void testRollSelectionGestures()
{
    RollRig rig;
    if (! rig.ok())
        return;

    auto& roll = *rig.roll;
    auto& p = rig.p;
    p.setMelody (fourNotes());
    pumpMessages (40);
    roll.setSnap (cb::MelodySnap::Off);
    roll.setGrid (cb::RateDivision::Eighth);
    roll.setTopNote (75);
    auto melody = [&p] { return p.getMelody(); };

    // Ctrl + click toggles
    int depth = undoDepth (p);
    rig.click (rig.noteBody (1), kCtrl);
    check (roll.getSelection() == std::vector<int> { 1 }, "roll2: Ctrl+click adds a note to the selection", selText (roll.getSelection()));
    rig.click (rig.noteBody (2), kCtrl);
    rig.click (rig.noteBody (1), kCtrl);
    check (roll.getSelection() == std::vector<int> { 2 } && undoDepth (p) == depth,
           "roll2: Ctrl+click on a selected note removes it (no undo step)", selText (roll.getSelection()));

    // Ctrl + drag: marquee replaces the selection, also when it starts on a note; Ctrl+Shift adds
    rig.drag (rig.cell (0.2, 65), rig.cell (3.5, 59), kCtrl);
    check (roll.getSelection() == std::vector<int> { 0, 1 }, "roll2: a Ctrl+drag marquee replaces the selection",
           selText (roll.getSelection()));
    rig.drag (rig.noteBody (2), rig.cell (9.5, 74), kCtrl);
    check (roll.getSelection() == std::vector<int> { 2, 3 }, "roll2: a marquee may start on a note", selText (roll.getSelection()));
    rig.drag (rig.cell (0.2, 61), rig.cell (0.8, 59), kCtrlShift);
    check (roll.getSelection() == std::vector<int> { 0, 2, 3 } && melody() == fourNotes() && undoDepth (p) == depth,
           "roll2: Ctrl+Shift+drag adds to the selection; selecting changes no note", selText (roll.getSelection()));

    // The selection pill shows and opens the Selection section; a right-click on it does nothing
    {
        const auto pill = roll.getSelectionPillArea();
        check (! pill.isEmpty(), "roll2: the 'N selected' pill shows with a selection");
        rig.click (pill.getCentre(), kRight);
        check (! roll.getLastMenuShown().has_value() && roll.getSelection().size() == 3,
               "roll2: a right-click on the selection pill opens nothing and keeps the selection");
        rig.click (pill.getCentre(), kLeft);
        check (roll.getLastMenuShown() == Section::Selection && roll.getSelection().size() == 3,
               "roll2: the selection pill opens the Selection section");
        juce::PopupMenu::dismissAllActiveMenus();
        pumpMessages (30);
    }

    // A click on empty space with a selection only deselects
    rig.click (rig.cell (13.2, 70), kLeft);
    check (roll.getSelection().empty() && melody() == fourNotes() && undoDepth (p) == depth && roll.getSelectionPillArea().isEmpty(),
           "roll2: a click on empty space with a selection only deselects (no note drawn)");

    // Group move with the anchor's grid-rounded delta, re-identified selection, one undo step
    roll.setSelection ({ 0, 1 });
    rig.drag (rig.noteBody (0), rig.noteBody (0) + (rig.cell (1.0, 62) - rig.cell (0.0, 60)), kLeft);
    {
        const auto m = melody();
        const auto sel = roll.getSelection();
        check (m.notes.size() == 4 && m.notes[0].note == 62 && m.notes[0].startBeat == 1.0 && m.notes[1].note == 66
                   && m.notes[1].startBeat == 3.0 && undoDepth (p) == depth + 1,
               "roll2: dragging a selected note moves the whole selection in one undo step", layerText (m));
        check (sel == std::vector<int> { 0, 1 }, "roll2: the moved notes stay selected", selText (sel));
    }

    // Group clamps: at beat 0 and at the lowest pitch the whole group stops (no note folds)
    rig.drag (rig.noteBody (0), rig.noteBody (0) + (rig.cell (0.0, 60) - rig.cell (4.0, 60)), kLeft);
    {
        const auto m = melody();
        check (m.notes[0].startBeat == 0.0 && m.notes[1].startBeat == 2.0, "roll2: a group move clamps at beat 0 for the whole group",
               layerText (m));
    }
    rig.drag (rig.noteBody (0), rig.noteBody (0) + (rig.cell (0.0, 20) - rig.cell (0.0, 62)), kLeft);
    {
        const auto m = melody();
        check (m.notes[0].note == 36 && m.notes[1].note == 40, "roll2: a group move clamps at the lowest pitch keeping the shape",
               layerText (m));
        p.undo();
        roll.setSelection ({ 0, 1 });
    }

    // Group resize by the same delta
    depth = undoDepth (p);
    rig.drag (rig.noteEdge (0), rig.noteEdge (0) + (rig.cell (1.0, 60) - rig.cell (0.0, 60)), kLeft);
    {
        const auto m = melody();
        check (m.notes[0].lengthBeats == 2.0 && m.notes[1].lengthBeats == 2.0 && m.notes[2].lengthBeats == 1.0 && undoDepth (p) == depth + 1
                   && roll.getSelection() == std::vector<int> { 0, 1 },
               "roll2: dragging a selected note's edge resizes the whole selection (one undo step)", layerText (m));
    }

    // Grabbing an unselected note: it moves alone and no selection is left
    rig.drag (rig.noteBody (3), rig.noteBody (3) + (rig.cell (10.0, 72) - rig.cell (8.0, 72)), kLeft);
    {
        const auto m = melody();
        check (m.notes[3].startBeat == 10.0 && m.notes[0].startBeat == 0.0 && roll.getSelection().empty(),
               "roll2: grabbing an unselected note moves it alone and leaves no selection", layerText (m));
    }

    // Shift + drag duplicates: an unselected note -> one copy; the selection -> copies that become the selection
    depth = undoDepth (p);
    rig.drag (rig.noteBody (3), rig.noteBody (3) + (rig.cell (12.0, 72) - rig.cell (10.0, 72)), kShift);
    {
        const auto m = melody();
        check (m.notes.size() == 5 && m.notes[3].startBeat == 10.0 && m.notes[4].startBeat == 12.0 && m.notes[4].note == 72
                   && undoDepth (p) == depth + 1 && roll.getSelection().empty(),
               "roll2: Shift+drag duplicates an unselected note to the drop point", layerText (m));
    }
    roll.setSelection ({ 0, 1 });
    rig.drag (rig.noteBody (0), rig.noteBody (0) + (rig.cell (4.0, 60) - rig.cell (0.0, 60)), kShift);
    {
        const auto m = melody();
        const auto sel = roll.getSelection();
        bool copiesSelected = sel.size() == 2;
        for (int i : sel)
            copiesSelected = copiesSelected && (m.notes[(size_t) i].startBeat == 4.0 || m.notes[(size_t) i].startBeat == 6.0);
        check (m.notes.size() == 7 && copiesSelected, "roll2: Shift+drag of the selection duplicates it; the copies are selected",
               layerText (m) + selText (sel));
    }

    // Right-click on a selected note deletes only that note; the rest stays selected
    {
        const auto before = melody();
        const auto sel = roll.getSelection();
        const auto kept = before.notes[(size_t) sel[1]];
        depth = undoDepth (p);
        rig.click (rig.noteBody (sel[0]), kRight);
        const auto m = melody();
        const auto now = roll.getSelection();
        check (m.notes.size() == before.notes.size() - 1 && now.size() == 1 && m.notes[(size_t) now[0]] == kept && undoDepth (p) == depth + 1,
               "roll2: right-click on a selected note deletes only that note (one undo step)", layerText (m) + selText (now));
    }

    // Right-drag erase: one undo step for the sweep
    {
        p.setMelody (fourNotes());
        pumpMessages (30);
        depth = undoDepth (p);
        rig.drag (rig.noteBody (0), rig.noteBody (1), kRight);
        check (melody().notes.size() == 2 && undoDepth (p) == depth + 1, "roll2: a right-drag erase is one undo step", layerText (melody()));
    }

    // A gesture whose mouse-up never arrives is dropped at the next mouse-down
    {
        p.setMelody (fourNotes());
        pumpMessages (30);
        depth = undoDepth (p);
        rig.down (rig.noteBody (0), kLeft);
        rig.move (rig.noteBody (0) + (rig.cell (2.0, 64) - rig.cell (0.0, 60)), kLeft);
        rig.down (rig.cell (13.3, 70), kCtrl);
        rig.up (rig.cell (13.3, 70), kCtrl);
        check (melody() == fourNotes() && undoDepth (p) == depth, "roll2: a gesture without mouse-up is dropped at the next mouse-down");
    }

    // New notes take the last clicked / resized length and velocity
    {
        p.setMelody (makeMelody ({ melodyNote (60, 0.0, 1.5, 0.5f) }));
        pumpMessages (30);
        roll.setSelection ({});
        rig.click (rig.noteBody (0), kLeft);
        rig.click (rig.cell (6.2, 67), kLeft);
        const auto m = melody();
        check (m.notes.size() == 2 && m.notes[1].lengthBeats == 1.5 && std::abs (m.notes[1].velocity - 0.5f) < 1e-6f,
               "roll2: a new note takes the length and velocity of the last note clicked", layerText (m));
    }
}

//==============================================================================
void testRollCommands()
{
    RollRig rig;
    if (! rig.ok())
        return;

    auto& roll = *rig.roll;
    auto& p = rig.p;
    p.setMelody (fourNotes());
    pumpMessages (40);
    roll.setGrid (cb::RateDivision::Eighth);
    auto melody = [&p] { return p.getMelody(); };
    auto applies = [&roll] (Command c) { return roll.performCommand (c); };

    check (applies (Command::SelectAll) && roll.getSelection() == std::vector<int> { 0, 1, 2, 3 }, "roll2: SelectAll");
    check (! applies (Command::SelectAll), "roll2: SelectAll does not apply when everything is selected");
    check (applies (Command::SelectNone) && roll.getSelection().empty() && ! applies (Command::SelectNone), "roll2: SelectNone");

    // Copy / Cut / Paste / Paste here
    roll.setSelection ({ 1 });
    check (applies (Command::Copy) && melody() == fourNotes(), "roll2: Copy changes nothing");
    check (applies (Command::PasteHere) == false, "roll2: Paste here needs a position");
    check (roll.performCommand (Command::PasteHere, 12.3) && melody().notes.size() == 5 && melody().notes[4].startBeat == 12.0
               && melody().notes[4].note == 64,
           "roll2: Paste here inserts the clip at the clicked beat floored to the grid", layerText (melody()));
    check (roll.getSelection().size() == 1 && melody().notes[(size_t) roll.getSelection()[0]].startBeat == 12.0,
           "roll2: the pasted notes become the selection");
    roll.setSelection ({ 0 });
    check (applies (Command::Cut) && melody().notes.size() == 4 && roll.getSelection().empty(), "roll2: Cut removes the selection");
    check (applies (Command::Paste) && melody().notes.size() == 5 && melody().notes[0].startBeat == 0.0 && melody().notes[0].note == 60,
           "roll2: Paste puts the clip back at its source beat", layerText (melody()));

    // Duplicate / Delete
    p.setMelody (fourNotes());
    pumpMessages (30);
    roll.setSelection ({ 0 });
    check (applies (Command::Duplicate) && melody().notes.size() == 5 && roll.getSelection().size() == 1
               && melody().notes[(size_t) roll.getSelection()[0]].startBeat == 1.0,
           "roll2: Duplicate copies the selection by its default offset; the copy is selected", layerText (melody()));
    check (applies (Command::Delete) && melody() == fourNotes() && roll.getSelection().empty(), "roll2: Delete removes the selection");

    // Velocity presets act on the selection, or on all notes
    roll.setSelection ({ 1, 2 });
    check (applies (Command::VelocitySoft) && melody().notes[1].velocity == 0.4f && melody().notes[2].velocity == 0.4f
               && melody().notes[0].velocity == 0.8f && roll.getSelection() == std::vector<int> { 1, 2 },
           "roll2: Velocity Soft sets the selected notes to 40% (selection kept)");
    check (applies (Command::VelocityUp) && std::abs (melody().notes[1].velocity - 0.5f) < 1e-5f, "roll2: Louder +10%");
    check (applies (Command::VelocityDown) && std::abs (melody().notes[1].velocity - 0.4f) < 1e-5f, "roll2: Softer -10%");
    check (applies (Command::VelocityMedium) && std::abs (melody().notes[2].velocity - 0.65f) < 1e-5f, "roll2: Velocity Medium 65%");
    check (applies (Command::VelocityHard) && melody().notes[2].velocity == 1.0f, "roll2: Velocity Hard 100%");
    check (applies (Command::VelocityDefault) && std::abs (melody().notes[2].velocity - 0.8f) < 1e-5f, "roll2: Velocity Default 80%");
    check (! applies (Command::VelocityDefault), "roll2: a velocity preset every target already has does not apply");
    roll.setSelection ({});
    check (applies (Command::VelocitySoft) && melody().notes[0].velocity == 0.4f && melody().notes[3].velocity == 0.4f,
           "roll2: without a selection a velocity preset acts on every note");
    {
        rig.click (rig.cell (14.1, 70), kLeft);
        bool ok = false;
        for (auto& n : melody().notes)
            ok = ok || (n.startBeat == 14.0 && std::abs (n.velocity - 0.4f) < 1e-6f);
        check (ok, "roll2: a velocity edit sets the velocity of new notes", layerText (melody()));
    }

    // Transpose
    p.setMelody (fourNotes());
    pumpMessages (30);
    roll.setSelection ({ 0 });
    check (applies (Command::StepUp) && melody().notes[0].note == 62, "roll2: Up a scale step (C -> D in C major)");
    check (applies (Command::StepDown) && melody().notes[0].note == 60, "roll2: Down a scale step");
    check (applies (Command::SemitoneUp) && melody().notes[0].note == 61, "roll2: Up a semitone");
    check (applies (Command::SemitoneDown) && melody().notes[0].note == 60, "roll2: Down a semitone");
    check (applies (Command::OctaveUp) && melody().notes[0].note == 72, "roll2: Up an octave");
    check (applies (Command::OctaveDown) && melody().notes[0].note == 60 && roll.getSelection() == std::vector<int> { 0 },
           "roll2: Down an octave (selection kept)");

    // Quantize / Legato
    p.setMelody (makeMelody ({ melodyNote (60, 1.125, 0.5), melodyNote (64, 3.0, 0.5) }));
    pumpMessages (30);
    roll.setSelection ({});
    check (applies (Command::Quantize) && melody().notes[0].startBeat == 1.0, "roll2: Quantize starts to grid", layerText (melody()));
    check (applies (Command::Legato) && melody().notes[0].lengthBeats == 2.0, "roll2: Legato fills the gap", layerText (melody()));

    // Clear melody, Play (layer on / off), velocity lane and other-layer toggles
    check (applies (Command::ClearMelody) && melody().empty() && ! applies (Command::ClearMelody), "roll2: ClearMelody");
    p.undo();
    check (applies (Command::ToggleLayerOn) && ! p.isLayerOn (Layer::Melody), "roll2: ToggleLayerOn mutes the melody");
    check (applies (Command::ToggleLayerOn) && p.isLayerOn (Layer::Melody), "roll2: ToggleLayerOn turns it back on");
    check (applies (Command::ToggleVelocityLane) && roll.isVelocityLaneVisible() && applies (Command::ToggleVelocityLane)
               && ! roll.isVelocityLaneVisible(),
           "roll2: ToggleVelocityLane");
    check (applies (Command::ToggleOtherLayers) && ! roll.areOtherLayersShown() && applies (Command::ToggleOtherLayers)
               && roll.areOtherLayersShown(),
           "roll2: ToggleOtherLayers");

    // Reset this chord (bass edited in slot 1 only), from the chord-strip menu's command
    {
        roll.setLayer (Layer::Bass);
        pumpMessages (30);
        auto bass = p.getLayerNotes (Layer::Bass);
        const auto autoBass = bass;
        for (auto& n : bass.notes)
            if (n.startBeat >= 4.0 && n.startBeat < 8.0)
                n.note += 12;
        p.setLayerNotes (Layer::Bass, bass);
        pumpMessages (60);
        const auto edited = p.getEditedSlots (Layer::Bass);
        check (p.isLayerCustom (Layer::Bass) && edited.size() == 4 && ! edited[0] && edited[1], "roll2: bass edited in slot 2 only");

        const auto chordMenu = menuEntries (roll.createMenu (Section::Chord, 5.0));
        const auto chordMenuOther = menuEntries (roll.createMenu (Section::Chord, 1.0));
        check (chordMenu.size() == 1 && chordMenu[0].text == "Reset this chord" && chordMenu[0].enabled && chordMenuOther.size() == 1
                   && ! chordMenuOther[0].enabled,
               "roll2: the chord menu has one item, enabled only over an edited slot");
        check (! roll.performCommand (Command::ResetChord, 1.0) && roll.performCommand (Command::ResetChord, 5.0)
                   && ! p.isLayerCustom (Layer::Bass) && p.getLayerNotes (Layer::Bass) == autoBass,
               "roll2: Reset this chord resets that slot (the layer is auto again)");
    }

    // Paste across layers folds into the target layer's limits
    {
        roll.setLayer (Layer::Melody);
        p.setMelody (makeMelody ({ melodyNote (88, 2.0, 1.0) }));
        pumpMessages (30);
        roll.setSelection ({ 0 });
        applies (Command::Copy);
        roll.setLayer (Layer::Bass);
        check (applies (Command::Paste), "roll2: Paste on the Bass tab");
        bool folded = false;
        for (auto& n : p.getLayerNotes (Layer::Bass).notes)
            folded = folded || (n.startBeat == 2.0 && n.note == 64);
        check (p.isLayerCustom (Layer::Bass) && folded, "roll2: a melody note pasted into the bass folds by octaves into 24..72",
               layerText (p.getLayerNotes (Layer::Bass)));
        p.resetLayer (Layer::Bass);
        roll.setLayer (Layer::Melody);
    }
}

//==============================================================================
void testRollSelectionLifetime()
{
    RollRig rig;
    if (! rig.ok())
        return;

    auto& roll = *rig.roll;
    auto& p = rig.p;
    p.setMelody (fourNotes());
    pumpMessages (40);

    // Kept across a parameter-only serial change that leaves the current layer's notes alone
    roll.setSelection ({ 1, 2 });
    const auto serial = p.getLayerSerial();
    setParam (p, cb::ParamIDs::arpOn, 1.0f);
    pumpMessages (150);
    check (p.getLayerSerial() != serial && roll.getSelection() == std::vector<int> { 1, 2 },
           "roll2: the selection survives a parameter-only serial change (arpOn)");

    // Cleared after undo
    p.setMelody (makeMelody ({ melodyNote (60, 0.0, 1.0), melodyNote (64, 2.0, 1.0), melodyNote (67, 4.0, 2.0) }));
    pumpMessages (60);
    roll.setSelection ({ 0, 1 });
    p.undo();
    pumpMessages (100);
    check (roll.getSelection().empty(), "roll2: the selection is cleared after an undo");

    // Cleared after an automation re-render of an auto layer
    roll.setLayer (Layer::Arp);
    pumpMessages (60);
    roll.setSelection ({ 0, 1 });
    setParam (p, cb::ParamIDs::arpShape, 4.0f);
    pumpMessages (150);
    check (roll.getSelection().empty(), "roll2: the selection is cleared when automation re-renders the auto arp");

    // Cleared after a chord edit (an in-place chord change re-renders the auto bass)
    roll.setLayer (Layer::Bass);
    pumpMessages (60);
    roll.setSelection ({ 0 });
    auto slot = p.getProgression().slots[0];
    slot.chord = Em_;
    p.updateSlot (0, slot);
    pumpMessages (100);
    check (roll.getSelection().empty(), "roll2: the selection is cleared after a chord edit");
    setParam (p, cb::ParamIDs::arpShape, 0.0f);
}

//==============================================================================
void testRollMenus()
{
    RollRig rig;
    if (! rig.ok())
        return;

    auto& roll = *rig.roll;
    auto& p = rig.p;

    // All (empty melody): every 7.8 item, the note items disabled
    {
        const auto menu = roll.createMenu (Section::All);
        const auto e = menuEntries (menu);
        std::string missing;
        check (hasEntries (e, { "Play melody", "Clear melody", "Select all", "Select none", "Copy", "Cut", "Paste", "Duplicate",
                                "Delete", "Velocity", "Transpose", "Quantize starts to grid", "Legato (fill the gaps)", "Snap",
                                "Grid", "Velocity lane", "Show other layers and instances" },
                           missing)
                   && findEntry (e, "Paste here") == nullptr,
               "roll2: the All menu lists the layer, selection and view items", missing);
        check (findEntry (e, "Play melody")->ticked && ! findEntry (e, "Clear melody")->enabled && ! findEntry (e, "Copy")->enabled
                   && ! findEntry (e, "Delete")->enabled && findEntry (e, "Show other layers and instances")->ticked,
               "roll2: items that do not apply are disabled, not hidden");
        int headers = 0;
        for (auto& x : e)
            headers += x.header ? 1 : 0;
        check (headers == 3, "roll2: the All menu has the layer, selection and view sections", str (headers));
        const auto* snap = subMenuOf (menu, "Snap");
        const auto* grid = subMenuOf (menu, "Grid");
        check (snap != nullptr && menuEntries (*snap).size() == 3 && grid != nullptr && menuEntries (*grid).size() == 4
                   && menuEntries (*snap)[1].ticked && menuEntries (*grid)[1].ticked,
               "roll2: Snap / Grid submenus tick the current layer's settings");
    }

    p.setMelody (fourNotes());
    pumpMessages (40);

    {
        const auto menu = roll.createMenu (Section::All, 5.0);
        const auto e = menuEntries (menu);
        check (findEntry (e, "Paste here") != nullptr && findEntry (e, "Copy")->enabled && findEntry (e, "Clear melody")->enabled,
               "roll2: an empty-grid right-click menu adds Paste here");
        const auto* vel = subMenuOf (menu, "Velocity");
        const auto* tr = subMenuOf (menu, "Transpose");
        std::string missing;
        check (vel != nullptr && hasEntries (menuEntries (*vel), { "Soft 40%", "Medium 65%", "Default 80%", "Hard 100%", "Louder +10%", "Softer -10%" }, missing)
                   && tr != nullptr
                   && hasEntries (menuEntries (*tr), { "Up a scale step", "Down a scale step", "Up a semitone", "Down a semitone", "Up an octave", "Down an octave" }, missing),
               "roll2: Velocity and Transpose submenus", missing);
        check (findEntry (menuEntries (*vel), "Default 80%")->ticked && ! findEntry (menuEntries (*vel), "Default 80%")->enabled,
               "roll2: the preset every note already has is ticked (and disabled)");
    }

    {
        roll.setSelection ({ 0, 2 });
        const auto e = menuEntries (roll.createMenu (Section::Selection));
        std::string missing;
        check (hasEntries (e, { "Select all", "Select none", "Copy", "Cut", "Paste", "Duplicate", "Delete", "Velocity", "Transpose",
                                "Quantize starts to grid", "Legato (fill the gaps)" }, missing)
                   && findEntry (e, "Play melody") == nullptr && findEntry (e, "Snap") == nullptr && e[0].header
                   && e[0].text.containsIgnoreCase ("2 notes"),
               "roll2: the Selection section", missing + e[0].text.toStdString());
    }

    {
        const auto e = menuEntries (roll.createMenu (Section::Layer, -1.0, Layer::Arp));
        check (e.size() == 3 && e[0].header && e[0].text.startsWith ("Arp") && e[0].text.contains ("auto")
                   && findEntry (e, "Play arp") != nullptr && ! findEntry (e, "Play arp")->ticked
                   && findEntry (e, "Reset arp to follow the chords") != nullptr && ! findEntry (e, "Reset arp to follow the chords")->enabled,
               "roll2: the Layer section of another tab (Arp: auto, off)");
    }

    {
        const auto e = menuEntries (roll.createMenu (Section::Velocity));
        std::string missing;
        check (hasEntries (e, { "Soft 40%", "Medium 65%", "Default 80%", "Hard 100%", "Louder +10%", "Softer -10%", "Velocity lane" }, missing),
               "roll2: the Velocity section", missing);
    }

    // Entry points: toolbar "...", a tab right-click (no switch), right-click on empty grid, on the strip, in the lane
    {
        roll.getToolbar().onMoreClicked ({ 100, 100, 30, 20 });
        check (roll.getLastMenuShown() == Section::All, "roll2: the toolbar's ... opens the All menu");
        juce::PopupMenu::dismissAllActiveMenus();

        roll.getToolbar().onTabMenu (Layer::Bass);
        check (roll.getLastMenuShown() == Section::Layer && roll.getLayer() == Layer::Melody,
               "roll2: a tab right-click opens that layer's section without switching tabs");
        juce::PopupMenu::dismissAllActiveMenus();

        roll.setSelection ({});
        const auto before = p.getMelody();
        const int depth = undoDepth (p);
        rig.click (rig.cell (13.3, roll.getTopNote() - 2), kRight);
        check (roll.getLastMenuShown() == Section::All && p.getMelody() == before, "roll2: a right-click on empty grid opens the menu");
        juce::PopupMenu::dismissAllActiveMenus();

        const auto strip = roll.getChordStripArea();
        check (! strip.isEmpty() && strip.getBottom() <= roll.getGridArea().getY(), "roll2: the chord strip sits above the grid");
        rig.click ({ strip.getX() + 20.0f, strip.getCentreY() }, kRight);
        check (roll.getLastMenuShown() == Section::Chord && p.getMelody() == before && undoDepth (p) == depth,
               "roll2: a chord-strip right-click opens the Chord menu and never acts as a left click");
        juce::PopupMenu::dismissAllActiveMenus();
        rig.click ({ strip.getX() + 20.0f, strip.getCentreY() }, kLeft);
        check (p.getMelody() == before && undoDepth (p) == depth, "roll2: a left click on the chord strip does nothing");

        roll.setVelocityLaneVisible (true);
        const auto lane = roll.getVelocityLaneArea();
        rig.click ({ lane.getRight() - 4.0f, lane.getCentreY() }, kRight);
        check (roll.getLastMenuShown() == Section::Velocity && p.getMelody() == before, "roll2: a velocity-lane right-click opens Velocity");
        juce::PopupMenu::dismissAllActiveMenus();
        roll.setVelocityLaneVisible (false);
        pumpMessages (30);
    }

    // Other-layer outlines only while that layer is on (and the toggle is on)
    {
        p.setLayerOn (Layer::Bass, false);
        setParam (p, cb::ParamIDs::arpOn, 0.0f);
        pumpMessages (150);
        roll.setTopNote (72);
        const int shownOff = roll.getNumOtherLayerNotesShown();
        setParam (p, cb::ParamIDs::arpOn, 1.0f);
        pumpMessages (150);
        const int shownOn = roll.getNumOtherLayerNotesShown();
        roll.setOtherLayersShown (false);
        const int hidden = roll.getNumOtherLayerNotesShown();
        roll.setOtherLayersShown (true);
        check (shownOff == 0 && shownOn > 0 && hidden == 0, "roll2: other-layer outlines show only while that layer is on",
               str (shownOff, " ", shownOn, " ", hidden));
    }
}

//==============================================================================
void testRollEmptyCardAndLane()
{
    RollRig rig;
    if (! rig.ok())
        return;

    auto& roll = *rig.roll;
    auto& p = rig.p;

    // Empty melody: the card's button is the only part that captures clicks
    {
        const auto button = roll.getEmptyStateButtonArea();
        check (! button.isEmpty() && roll.getGridArea().contains (button), "roll2: an empty melody shows the Generate card button");
        const auto below = button.getBottomLeft().translated (4.0f, 12.0f);
        rig.click (below, kLeft);
        check (p.getMelody().notes.size() == 1 && roll.getEmptyStateButtonArea().isEmpty(),
               "roll2: a click on the card outside its button draws a note");
        p.undo();
        pumpMessages (40);

        const bool realToolbar = ! roll.getToolbar().getItemBounds (RollToolbarItem::TabMelody).isEmpty();
        const int depth = undoDepth (p);
        rig.click (roll.getEmptyStateButtonArea().getCentre(), kLeft);
        pumpMessages (40);

        if (realToolbar)
        {
            // Read the generator flag first: undoDepth undoes and redoes, which clears it (final-spec 6.2).
            const bool fromGenerator = p.isMelodyFromGenerator();
            check (! p.getMelody().empty() && fromGenerator && undoDepth (p) == depth + 1,
                   "roll2: the card's Generate button generates a melody (one undo step)");
        }
        else
            std::cout << "NOTE roll2: the card button calls RollToolbar::generateNewMelody (a WP0 stub here; checked after WP6)\n";

        p.setMelody ({});
        pumpMessages (30);
        rig.click (roll.getEmptyStateButtonArea().getCentre(), kRight);
        check (roll.getLastMenuShown() == Section::All && p.getMelody().empty(), "roll2: a right-click on the card button never generates");
        juce::PopupMenu::dismissAllActiveMenus();
    }

    // Arp / Bass custom with no notes: a message card, no button
    {
        roll.setLayer (Layer::Bass);
        p.setLayerNotes (Layer::Bass, {});
        pumpMessages (40);
        check (p.isLayerCustom (Layer::Bass) && p.getLayerNotes (Layer::Bass).empty() && roll.getEmptyStateButtonArea().isEmpty(),
               "roll2: an empty custom bass shows no Generate button");
        p.resetLayer (Layer::Bass);
        roll.setLayer (Layer::Melody);
    }

    // Velocity lane: stalk drag sets one note, or offsets the selection; one undo step; auto layers become custom
    {
        p.setMelody (makeMelody ({ melodyNote (60, 0.0, 1.0, 0.8f), melodyNote (64, 2.0, 1.0, 0.6f), melodyNote (67, 4.0, 1.0, 0.5f) }));
        pumpMessages (30);
        const auto gridBefore = roll.getGridArea();
        roll.setVelocityLaneVisible (true);
        const auto lane = roll.getVelocityLaneArea();
        check (! lane.isEmpty() && lane.getY() > roll.getGridArea().getBottom() && roll.getGridArea().getHeight() < gridBefore.getHeight()
                   && (bool) p.getAPVTS().state.getProperty ("cb.rollVelLane"),
               "roll2: the velocity lane sits under the grid (remembered in cb.rollVelLane)");
        check (lane.getHeight() >= 24.0f && lane.getHeight() <= 40.0f, "roll2: the lane is 24..40 px tall", str (lane.getHeight()));

        const auto inner = lane.reduced (0.0f, 4.0f);
        auto yFor = [&inner] (float v) { return inner.getBottom() - v * inner.getHeight(); };
        auto stalkX = [&] (double beat) { return std::round (roll.getCellCentre (beat, 60).x) + 1.0f; };

        int depth = undoDepth (p);
        rig.drag ({ stalkX (2.0) + 2.0f, yFor (0.6f) }, { stalkX (2.0) + 2.0f, yFor (0.3f) }, kLeft);
        auto m = p.getMelody();
        check (std::abs (m.notes[1].velocity - 0.3f) < 0.06f && m.notes[0].velocity == 0.8f && undoDepth (p) == depth + 1,
               "roll2: dragging a stalk sets that note's velocity in one undo step", layerText (m));

        roll.setSelection ({ 0, 2 });
        depth = undoDepth (p);
        rig.drag ({ stalkX (0.0), yFor (0.8f) }, { stalkX (0.0), yFor (1.0f) }, kLeft);
        m = p.getMelody();
        check (m.notes[0].velocity > 0.95f && std::abs (m.notes[2].velocity - 0.7f) < 0.06f && std::abs (m.notes[1].velocity - 0.3f) < 0.06f
                   && undoDepth (p) == depth + 1 && roll.getSelection() == std::vector<int> { 0, 2 },
               "roll2: dragging a selected stalk offsets the whole selection", layerText (m));

        rig.click ({ stalkX (4.0), yFor (0.2f) }, kLeft);
        check (p.getMelody() == m, "roll2: a click on a stalk without dragging changes nothing");

        roll.setLayer (Layer::Bass);
        pumpMessages (30);
        const auto bass = p.getLayerNotes (Layer::Bass);
        const auto x = std::round (roll.getCellCentre (bass.notes[0].startBeat, 40).x) + 1.0f;
        rig.drag ({ x, yFor (bass.notes[0].velocity) }, { x, yFor (0.2f) }, kLeft);
        check (p.isLayerCustom (Layer::Bass) && std::abs (p.getLayerNotes (Layer::Bass).notes[0].velocity - 0.2f) < 0.06f,
               "roll2: a velocity drag on the auto bass makes it custom");
        p.resetLayer (Layer::Bass);
        roll.setLayer (Layer::Melody);
        roll.setVelocityLaneVisible (false);
    }

    // Layout at the minimum and maximum editor sizes: the grid, pills and card stay inside the card
    for (auto size : { juce::Point<int> (ChordbloomEditor::kMinWidth, ChordbloomEditor::kMinHeight),
                       juce::Point<int> (ChordbloomEditor::kMaxWidth, ChordbloomEditor::kMaxHeight) })
    {
        rig.ed->setSize (size.x, size.y);
        pumpMessages (40);
        p.setMelody ({});
        p.setLayerOn (Layer::Melody, false);
        pumpMessages (60);
        roll.setVelocityLaneVisible (true);
        const auto gridArea = roll.getGridArea();
        const auto button = roll.getEmptyStateButtonArea();
        const auto offPill = roll.getLayerOffPillArea();
        const auto lane = roll.getVelocityLaneArea();
        const auto inside = roll.getLocalBounds().toFloat();
        check (! gridArea.isEmpty() && gridArea.getHeight() >= 60.0f && gridArea.contains (button) && gridArea.contains (offPill)
                   && ! button.intersects (offPill) && inside.contains (lane) && roll.getToolbar().getHeight() == (size.y < 600 ? 24 : 26),
               "roll2: layout at " + std::to_string (size.x) + "x" + std::to_string (size.y),
               str (gridArea.toString(), " | ", button.toString(), " | ", offPill.toString(), " | ", lane.toString()));
        p.setLayerOn (Layer::Melody, true);
        roll.setVelocityLaneVisible (false);
    }
}

//==============================================================================
void testRollPaintPerformance()
{
    RollRig rig (ChordbloomEditor::kDefaultWidth, ChordbloomEditor::kDefaultHeight, {});
    if (! rig.ok())
        return;

    auto& roll = *rig.roll;
    auto& p = rig.p;
    cb::Progression prog;
    for (int i = 0; i < 16; ++i)
    {
        cb::ProgressionSlot s;
        s.chord = (i % 4 == 0 ? C_ : i % 4 == 1 ? Am_ : i % 4 == 2 ? F_ : G_);
        s.lengthBeats = 16;
        prog.slots.push_back (s);
    }
    p.setProgression (prog);

    cb::Melody arp;
    for (int i = 0; i < 2048; ++i)
        arp.notes.push_back (melodyNote (60 + (i * 5) % 14, i * 0.125, 0.1, 0.75f));
    setParam (p, cb::ParamIDs::arpOn, 1.0f);
    p.setLayerNotes (Layer::Arp, arp);
    pumpMessages (100);
    roll.setLayer (Layer::Arp);
    roll.setSnap (cb::MelodySnap::Off);
    roll.setTopNote (75);
    roll.setVelocityLaneVisible (true);
    roll.setSelection ({ 0, 1, 2, 3, 4, 5, 6, 7 });
    check (p.getLayerNotes (Layer::Arp).notes.size() == 2048, "roll2: a 2048-note custom arp");

    juce::Image image (juce::Image::ARGB, roll.getWidth(), roll.getHeight(), true);
    double best = 1.0e9;
    for (int run = 0; run < 5; ++run)
    {
        juce::Graphics g (image);
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        roll.paintEntireComponent (g, true);
        best = std::min (best, juce::Time::getMillisecondCounterHiRes() - t0);
    }
    std::cout << "INFO roll2: paint with a 2048-note arp " << best << " ms\n";
    check (best < 40.0, "roll2: the roll paints a 2048-note layer in < 40 ms", str (best, " ms"));

    // Hit testing and a full drag over the dense layer stay correct
    roll.setSelection ({});
    const int depth = undoDepth (p);
    rig.drag (rig.noteBody (0), rig.noteBody (0) + juce::Point<float> (0.0f, -roll.getCellCentre (0.0, 61).y + roll.getCellCentre (0.0, 60).y), kLeft);
    check (undoDepth (p) == depth + 1 && p.getLayerNotes (Layer::Arp).notes.size() == 2048,
           "roll2: a drag in the dense arp is one undo step and keeps every note");
}

//==============================================================================
// Review fixes: vertical Shift+drag duplicates, edge-click jitter, cancelled duplicates, outside changes during a
// no-op gesture, the note cap, clicks through the hint pill, and async menus acting on the notes they showed.
bool hasNote (const cb::Melody& m, int note, double start)
{
    for (auto& n : m.notes)
        if (n.note == note && std::abs (n.startBeat - start) < 1e-9)
            return true;
    return false;
}

int indexOfNote (const cb::Melody& m, int note, double start)
{
    for (int i = 0; i < (int) m.notes.size(); ++i)
        if (m.notes[(size_t) i].note == note && std::abs (m.notes[(size_t) i].startBeat - start) < 1e-9)
            return i;
    return -1;
}

// The open PopupMenu window (async menus are resolved by its key handling: down arrow + return pick the first item).
juce::Component* openMenuWindow()
{
    auto& desktop = juce::Desktop::getInstance();

    for (int i = desktop.getNumComponents(); --i >= 0;)
        if (auto* c = desktop.getComponent (i))
            if (juce::String (typeid (*c).name()).contains ("MenuWindow") && c->isVisible())
                return c;

    return nullptr;
}

void testRollReviewFixes()
{
    RollRig rig;
    if (! rig.ok())
        return;

    auto& roll = *rig.roll;
    auto& p = rig.p;
    roll.setSnap (cb::MelodySnap::Off);
    roll.setGrid (cb::RateDivision::Eighth);
    p.setMelody (fourNotes());
    pumpMessages (40);
    roll.setTopNote (75);
    auto melody = [&p] { return p.getMelody(); };
    const float fourRows = roll.getCellCentre (0.0, 60).y - roll.getCellCentre (0.0, 64).y;

    // Shift+drag straight up (same beat): a copy four rows above, the original stays
    {
        roll.setSelection ({});
        const int depth = undoDepth (p);
        const auto from = rig.noteBody (0);
        rig.drag (from, from.translated (0.0f, -fourRows), kShift);
        const auto m = melody();
        check (m.notes.size() == 5 && hasNote (m, 60, 0.0) && hasNote (m, 64, 0.0) && undoDepth (p) == depth + 1
                   && roll.getSelection().empty(),
               "roll3: Shift+drag straight up duplicates an unselected note (the original stays, nothing selected)",
               layerText (m) + selText (roll.getSelection()));
    }
    {
        p.setMelody (fourNotes());
        pumpMessages (30);
        roll.setSelection ({ 1 });   // 64 @ 2
        const auto from = rig.noteBody (1);
        rig.drag (from, from.translated (0.0f, -fourRows), kShift);
        const auto m = melody();
        const int copy = indexOfNote (m, 68, 2.0);
        check (m.notes.size() == 5 && hasNote (m, 64, 2.0) && copy >= 0 && roll.getSelection() == std::vector<int> { copy },
               "roll3: Shift+drag straight up of a selected note: the original stays, the copy is selected",
               layerText (m) + selText (roll.getSelection()));
    }

    // A Shift+drag that ends where it started (dragged back, or jitter below half a grid step) keeps the selection
    {
        p.setMelody (fourNotes());
        pumpMessages (30);
        roll.setSelection ({ 1, 2 });
        const int depth = undoDepth (p);
        const auto from = rig.noteBody (1);
        rig.down (from, kShift);
        rig.move (from + (rig.cell (4.0, 64) - rig.cell (2.0, 64)), kShift);
        rig.move (from.translated (1.5f, 1.0f), kShift);
        rig.up (from.translated (1.5f, 1.0f), kShift);
        check (melody() == fourNotes() && undoDepth (p) == depth && roll.getSelection() == std::vector<int> { 1, 2 },
               "roll3: a cancelled Shift+drag changes nothing and keeps the selection", selText (roll.getSelection()));
    }

    // An outside change during a no-op press (here: the melody replaced) clears the selection at mouse-up
    {
        p.setMelody (fourNotes());
        pumpMessages (30);
        roll.setSelection ({ 1, 2 });
        const auto replaced = makeMelody ({ melodyNote (64, 2.0, 1.0), melodyNote (67, 4.0, 1.0), melodyNote (71, 6.0, 1.0) });
        const auto at = rig.noteBody (1);
        rig.down (at, kLeft);
        p.setMelody (replaced);
        pumpMessages (60);
        rig.up (at, kLeft);
        check (melody() == replaced && roll.getSelection().empty(),
               "roll3: a no-op press during which the notes changed from outside commits nothing and clears the selection",
               layerText (melody()) + selText (roll.getSelection()));
    }

    // Clicks pass through the transient hint pill (it has no action): a right-click under it deletes the note
    {
        const int top = roll.getTopNote();
        p.setMelody (makeMelody ({ melodyNote (top, 15.0, 0.75) }));
        pumpMessages (30);
        roll.setSelection ({});
        roll.getToolbar().onHint ("A long transient hint over the top right corner of the grid, for the test");
        rig.click (rig.noteBody (0), kRight);
        check (melody().empty(), "roll3: a right-click on a note under the hint pill deletes it", layerText (melody()));
    }

    // At the note cap nothing is evicted: drawing, Shift+drag, Duplicate and Paste are refused
    {
        cb::Melody full;
        for (int i = 0; i < 512; ++i)
            full.notes.push_back (melodyNote (64 + i % 8, i * 0.125, 0.125));
        p.setMelody (full);
        pumpMessages (30);
        roll.setTopNote (80);
        roll.setSelection ({});
        const auto atCap = melody();
        const int depth = undoDepth (p);
        rig.click (rig.cell (3.3, 77), kLeft);
        rig.drag (rig.noteBody (0), rig.noteBody (0) + (rig.cell (2.0, 64) - rig.cell (0.0, 64)), kShift);
        roll.setSelection ({ 0 });
        const bool dup = roll.performCommand (Command::Duplicate);
        const bool copied = roll.performCommand (Command::Copy);
        const bool pasted = roll.performCommand (Command::Paste);
        const auto menu = menuEntries (roll.createMenu (Section::Selection));
        const auto* dupItem = findEntry (menu, "Duplicate");
        const auto* pasteItem = findEntry (menu, "Paste");
        check (atCap.notes.size() == 512 && melody() == atCap && undoDepth (p) == depth && ! dup && copied && ! pasted
                   && dupItem != nullptr && ! dupItem->enabled && pasteItem != nullptr && ! pasteItem->enabled,
               "roll3: a full melody (512 notes) refuses draw, Shift+drag, Duplicate and Paste instead of evicting notes",
               str (melody().notes.size(), " dup ", dup, " paste ", pasted));
        roll.setSelection ({});
        roll.setTopNote (75);
    }

    // Async menus: the command acts on the notes the menu was opened for, never on every note of the layer
    {
        roll.setVelocityLaneVisible (true);
        pumpMessages (20);
        auto chooseFirstItem = [&] (const cb::Melody& changeTo)
        {
            const auto lane = roll.getVelocityLaneArea();
            // The window opens synchronously; it is driven at once (a menu of a background process may be dismissed
            // by its own timer). The outside change lands before the item is resolved, as an automation re-render
            // while the menu is open would; resolving it re-syncs the layer, which used to clear the selection.
            rig.click ({ lane.getRight() - 30.0f, lane.getCentreY() }, kRight);   // the Velocity section, "Soft 40%" first
            juce::Component::SafePointer<juce::Component> window (openMenuWindow());

            if (window == nullptr)
                return false;

            p.setMelody (changeTo);
            window->keyPressed (juce::KeyPress (juce::KeyPress::downKey));

            if (window != nullptr)
                window->keyPressed (juce::KeyPress (juce::KeyPress::returnKey));

            pumpMessages (40);
            return true;
        };

        // Velocity 0.5 everywhere: whichever velocity item the menu highlights first, it changes the notes it acts on.
        auto atHalf = [] (cb::Melody m)
        {
            for (auto& n : m.notes)
                n.velocity = 0.5f;
            return m;
        };

        p.setMelody (atHalf (fourNotes()));
        pumpMessages (30);
        roll.setSelection ({ 1, 2 });
        const auto gone = atHalf (makeMelody ({ melodyNote (60, 0.0, 1.0), melodyNote (65, 2.0, 1.0), melodyNote (72, 8.0, 1.0) }));
        const bool shown1 = chooseFirstItem (gone);
        check (shown1 && melody() == gone && roll.getSelection().empty(),
               "roll3: a menu command whose selected notes are gone is refused (it does not act on every note)",
               str (shown1, " ", layerText (melody())));

        p.setMelody (atHalf (fourNotes()));
        pumpMessages (30);
        roll.setSelection ({ 1, 2 });
        auto added = fourNotes();
        added.notes.push_back (melodyNote (70, 12.0, 1.0));
        const bool shown2 = chooseFirstItem (atHalf (added));
        const auto m = melody();
        bool onlyThose = m.notes.size() == 5;
        for (auto& n : m.notes)
            onlyThose = onlyThose && (std::abs (n.velocity - 0.5f) > 1e-6f) == ((n.note == 64 && n.startBeat == 2.0) || (n.note == 67 && n.startBeat == 4.0));
        check (shown2 && onlyThose, "roll3: a menu command acts on the notes the menu showed, found again after an outside change",
               str (shown2, " ", layerText (m)));
        juce::PopupMenu::dismissAllActiveMenus();
        roll.setVelocityLaneVisible (false);
        pumpMessages (20);
    }

    // Edge-click jitter on the auto arp (1/16 notes at gate 0.9 on the 1/16 grid) commits nothing
    {
        roll.setLayer (Layer::Arp);
        pumpMessages (60);
        const auto arp = p.getLayerNotes (Layer::Arp);
        int index = -1;
        for (int i = 0; i < (int) arp.notes.size() && index < 0; ++i)
        {
            const auto& n = arp.notes[(size_t) i];
            if (n.lengthBeats < 0.25 - 1e-9 && n.note > roll.getBottomNote() && n.note < roll.getTopNote() && n.startBeat < 12.0)
                index = i;
        }

        if (index >= 0 && roll.getGrid() == cb::RateDivision::Sixteenth)
        {
            const int depth = undoDepth (p);
            const auto e = rig.noteEdge (index);
            rig.down (e, kLeft);
            rig.move (e.translated (0.0f, 2.0f), kLeft);
            rig.move (e.translated (1.0f, 2.0f), kLeft);
            rig.up (e.translated (1.0f, 2.0f), kLeft);
            check (! p.isLayerCustom (Layer::Arp) && undoDepth (p) == depth && p.getLayerNotes (Layer::Arp) == arp,
                   "roll3: a shaky click on a short auto arp note's edge commits nothing (the arp stays auto)",
                   str (p.isLayerCustom (Layer::Arp), " ", undoDepth (p) - depth));

            // A real resize by one grid step still works
            const auto grabbed = arp.notes[(size_t) index];
            const auto e2 = rig.noteEdge (index);
            rig.drag (e2, e2 + (rig.cell (grabbed.startBeat + 0.5, grabbed.note) - rig.cell (grabbed.startBeat + 0.25, grabbed.note)), kLeft);
            const auto now = p.getLayerNotes (Layer::Arp);
            const int at = indexOfNote (now, grabbed.note, grabbed.startBeat);
            check (p.isLayerCustom (Layer::Arp) && at >= 0 && std::abs (now.notes[(size_t) at].lengthBeats - 0.5) < 1e-9,
                   "roll3: dragging a short note's edge one grid step on resizes it to the grid", layerText (now));
            p.resetLayer (Layer::Arp);
        }
        else
        {
            check (false, "roll3: a short auto arp note in view on the 1/16 grid", str (index, " grid ", (int) roll.getGrid()));
        }
        roll.setLayer (Layer::Melody);
    }
}

//==============================================================================
// Final review fixes: hidden velocity stalks, async menu toggles and stale layers, full refreshes only for what the
// roll shows, and no syncing while the roll is hidden.
void testRollFinalReviewFixes()
{
    RollRig rig;
    if (! rig.ok())
        return;

    auto& roll = *rig.roll;
    auto& p = rig.p;
    roll.setSnap (cb::MelodySnap::Off);
    roll.setGrid (cb::RateDivision::Eighth);
    roll.setTopNote (75);
    pumpMessages (30);

    // Notes past the visible beats (an 8-bar melody under a 4-bar progression) have no stalk: they are neither drawn
    // nor grabbed, and they never win over a visible stalk at the lane's right edge.
    {
        auto parkedMelody = [] (std::initializer_list<cb::MelodyNote> extra)
        {
            cb::Melody m = makeMelody (extra);
            for (int b = 16; b < 32; b += 2)
                m.notes.push_back (melodyNote (67, (double) b, 1.0, 0.9f));
            return cb::sanitiseMelody (m);
        };

        p.setMelody (parkedMelody ({ melodyNote (60, 0.0, 1.0, 0.5f) }));
        pumpMessages (40);
        roll.setVelocityLaneVisible (true);
        pumpMessages (20);
        const auto lane = roll.getVelocityLaneArea();
        const auto inner = lane.reduced (0.0f, 4.0f);
        auto yFor = [&inner] (float v) { return inner.getBottom() - v * inner.getHeight(); };
        const auto before = p.getMelody();
        int depth = undoDepth (p);
        rig.drag ({ lane.getRight() - 4.0f, yFor (0.9f) }, { lane.getRight() - 4.0f, yFor (0.2f) }, kLeft);
        check (before.notes.size() == 9 && p.getMelody() == before && undoDepth (p) == depth,
               "roll4: a drag on the empty right edge of the lane never edits a note past the visible beats",
               layerText (p.getMelody()));

        // A visible note on the last 1/16 of the grid: its stalk sits where the hidden ones would clamp.
        p.setMelody (parkedMelody ({ melodyNote (64, 15.9375, 0.0625, 0.3f) }));
        pumpMessages (40);
        const auto m0 = p.getMelody();
        const int visible = indexOfNote (m0, 64, 15.9375);
        const float x = std::min (lane.getRight() - 4.0f, std::round (roll.getCellCentre (15.9375, 60).x) + 1.0f);
        depth = undoDepth (p);
        rig.drag ({ x, yFor (0.85f) }, { x, yFor (0.5f) }, kLeft);
        const auto m1 = p.getMelody();
        bool hiddenKept = m1.notes.size() == m0.notes.size();
        for (auto& n : m1.notes)
            hiddenKept = hiddenKept && (n.startBeat < 16.0 || n.velocity == 0.9f);
        check (visible >= 0 && hiddenKept && std::abs (m1.notes[(size_t) visible].velocity - 0.5f) < 0.06f
                   && undoDepth (p) == depth + 1,
               "roll4: a press on the last beat's stalk edits that visible note, not a hidden one", layerText (m1));
        roll.setVelocityLaneVisible (false);
        pumpMessages (20);
    }

    // Async menu toggles set the opposite of the tick the menu showed, not of a state that changed while it was open
    {
        p.setMelody (fourNotes());
        setParam (p, cb::ParamIDs::arpOn, 0.0f);
        pumpMessages (60);
        roll.getToolbar().onTabMenu (Layer::Arp);   // "Play arp" (unticked) first
        juce::Component::SafePointer<juce::Component> window (openMenuWindow());
        const bool shown = window != nullptr;
        setParam (p, cb::ParamIDs::arpOn, 1.0f);    // automation turns the arp on while the menu is open
        if (window != nullptr)
            window->keyPressed (juce::KeyPress (juce::KeyPress::downKey));
        if (window != nullptr)
            window->keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
        pumpMessages (60);
        check (shown && p.isLayerOn (Layer::Arp),
               "roll4: Play arp picked from an unticked menu turns the arp on, even when automation turned it on meanwhile");
        juce::PopupMenu::dismissAllActiveMenus();
        setParam (p, cb::ParamIDs::arpOn, 0.0f);
        pumpMessages (40);

        roll.setVelocityLaneVisible (true);
        pumpMessages (20);
        const auto lane = roll.getVelocityLaneArea();
        rig.click ({ lane.getRight() - 30.0f, lane.getCentreY() }, kRight);   // Velocity section: "Velocity lane" (ticked) last
        juce::Component::SafePointer<juce::Component> laneMenu (openMenuWindow());
        const bool laneShown = laneMenu != nullptr && roll.getLastMenuShown() == Section::Velocity;
        roll.setVelocityLaneVisible (false);   // e.g. a state load hides the lane while the menu is open
        if (laneMenu != nullptr)
            laneMenu->keyPressed (juce::KeyPress (juce::KeyPress::upKey));
        if (laneMenu != nullptr)
            laneMenu->keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
        pumpMessages (40);
        check (laneShown && ! roll.isVelocityLaneVisible(),
               "roll4: Velocity lane picked from a ticked menu hides the lane, even when it was hidden meanwhile");
        juce::PopupMenu::dismissAllActiveMenus();
        roll.setVelocityLaneVisible (false);
        pumpMessages (20);
    }

    // A menu opened on one layer never pastes into another layer that became current meanwhile
    {
        p.setMelody (makeMelody ({ melodyNote (88, 2.0, 1.0) }));
        pumpMessages (30);
        roll.setLayer (Layer::Melody);
        roll.setSelection ({ 0 });
        roll.performCommand (Command::Copy);
        p.setMelody (fourNotes());
        roll.setLayer (Layer::Bass);
        pumpMessages (40);

        // The Selection pill's menu: Select all, Select none, Copy, Cut, Paste (five down-arrow presses).
        auto pasteFromPill = [&] (bool switchLayer)
        {
            roll.setSelection ({ 0 });
            const auto pill = roll.getSelectionPillArea();
            if (pill.isEmpty())
                return false;
            rig.click (pill.getCentre(), kLeft);
            juce::Component::SafePointer<juce::Component> menu (openMenuWindow());
            if (menu == nullptr)
                return false;
            if (switchLayer)
                roll.setLayer (Layer::Melody);   // as a state load or host undo would switch it
            for (int k = 0; k < 5 && menu != nullptr; ++k)
                menu->keyPressed (juce::KeyPress (juce::KeyPress::downKey));
            if (menu != nullptr)
                menu->keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
            pumpMessages (40);
            juce::PopupMenu::dismissAllActiveMenus();
            return true;
        };

        const auto melodyBefore = p.getMelody();
        const int depth = undoDepth (p);
        const bool shown = pasteFromPill (true);
        check (shown && p.getMelody() == melodyBefore && ! p.isLayerCustom (Layer::Bass) && undoDepth (p) == depth,
               "roll4: Paste from a menu opened on Bass is refused once Melody became current (nothing pasted anywhere)",
               layerText (p.getMelody()));

        roll.setLayer (Layer::Bass);
        pumpMessages (30);
        const bool shown2 = pasteFromPill (false);
        check (shown2 && p.isLayerCustom (Layer::Bass) && p.getMelody() == melodyBefore,
               "roll4: the same menu pastes into Bass while Bass stays current (control)");
        p.resetLayer (Layer::Bass);
        roll.setLayer (Layer::Melody);
        pumpMessages (30);
    }

    // Full refreshes only for what the roll shows: a Velocity re-render of the off arp and the out-of-view bass is not
    // one (it used to repaint the whole roll at 30 Hz during a knob drag); a bass change in view is.
    {
        p.setMelody (fourNotes());
        setParam (p, cb::ParamIDs::arpOn, 0.0f);
        setParam (p, cb::ParamIDs::bassMode, 1.0f);   // Root
        pumpMessages (60);
        roll.setTopNote (96);
        pumpMessages (40);

        const auto bassBefore = p.getLayerNotes (Layer::Bass);
        bool outOfView = ! bassBefore.notes.empty();
        for (auto& n : bassBefore.notes)
            outOfView = outOfView && n.note < roll.getBottomNote() - 2;

        const int r0 = roll.getNumSurfaceRefreshes();
        for (float v : { 0.4f, 0.6f, 0.5f })
        {
            setParam (p, cb::ParamIDs::velocity, v);
            pumpMessages (80);
        }
        check (outOfView && p.getLayerNotes (Layer::Bass) != bassBefore && roll.getNumSurfaceRefreshes() == r0,
               "roll4: a Velocity change that re-renders only the off arp and the out-of-view bass refreshes nothing",
               str (outOfView, " +", roll.getNumSurfaceRefreshes() - r0));

        roll.setTopNote (36);   // clamped: the lowest Melody rows, where the Root bass sits
        pumpMessages (40);
        int inView = 0;
        for (auto& n : p.getLayerNotes (Layer::Bass).notes)
            inView += n.note <= roll.getTopNote() && n.note >= roll.getBottomNote() ? 1 : 0;
        const int r1 = roll.getNumSurfaceRefreshes();
        setParam (p, cb::ParamIDs::velocity, 0.8f);
        pumpMessages (80);
        check (inView > 0 && roll.getNumSurfaceRefreshes() == r1,
               "roll4: a velocity-only re-render of bass outlines in view refreshes nothing (outlines ignore velocity)",
               str (inView, " +", roll.getNumSurfaceRefreshes() - r1));
        setParam (p, cb::ParamIDs::bassMode, (float) cb::BassMode::Octaves);
        pumpMessages (80);
        check (roll.getNumSurfaceRefreshes() > r1, "roll4: a bass change in view refreshes the roll");
        const int r2 = roll.getNumSurfaceRefreshes();
        p.setMelody (makeMelody ({ melodyNote (55, 1.0, 1.0) }));
        pumpMessages (60);
        check (roll.getNumSurfaceRefreshes() > r2, "roll4: a change of the current layer refreshes the roll");
        setParam (p, cb::ParamIDs::bassMode, 1.0f);
        setParam (p, cb::ParamIDs::velocity, 0.75f);
        roll.setTopNote (75);
        pumpMessages (60);
    }

    // Hidden (the Chords view): nothing is synced; showing the roll again resyncs it
    {
        rig.ed->setMelodyViewVisible (false);
        pumpMessages (40);
        const int h0 = roll.getNumSurfaceRefreshes();
        p.setMelody (fourNotes());
        setParam (p, cb::ParamIDs::bassMode, (float) cb::BassMode::Octaves);
        pumpMessages (120);
        const int hidden = roll.getNumSurfaceRefreshes() - h0;
        rig.ed->setMelodyViewVisible (true);
        pumpMessages (40);
        check (hidden == 0 && roll.getNumSurfaceRefreshes() > h0,
               "roll4: the hidden roll does not sync or repaint; showing it resyncs it", str (hidden));
        setParam (p, cb::ParamIDs::bassMode, 1.0f);
        pumpMessages (40);
    }

    // Final gate: the hover name pill names the note a click would write. With Chord snap the pointer on A4 over
    // C major previews G4 or C5, and the pill must say that pitch, not the pointer's row.
    {
        roll.setLayer (Layer::Melody);
        p.setMelody ({});
        roll.setTopNote (75);
        pumpMessages (40);
        const auto at = rig.cell (1.0, 69);

        roll.setSnap (cb::MelodySnap::Off);
        rig.surface->mouseMove (rig.event (at, juce::ModifierKeys(), at));
        check (roll.getHoverLabelRow() == 69, "roll4: with Snap off the hover pill names the pointer's row",
               str (roll.getHoverLabelRow()));

        roll.setSnap (cb::MelodySnap::ChordTones);
        rig.surface->mouseMove (rig.event (at + juce::Point<float> (1.0f, 0.0f), juce::ModifierKeys(), at));
        const int shown = roll.getHoverLabelRow();
        rig.click (at + juce::Point<float> (1.0f, 0.0f), kLeft);
        pumpMessages (20);
        const auto& m = p.getMelody();
        const int written = m.notes.size() == 1 ? m.notes[0].note : -1;
        check ((shown == 67 || shown == 72) && shown == written,
               "roll4: with Chord snap the hover pill names the snapped pitch a click writes", str (shown, " vs ", written));
        p.setMelody ({});
        rig.surface->mouseExit (rig.event (at, juce::ModifierKeys(), at));
        pumpMessages (20);
    }
}
} // namespace

void cbtest::runRollSelfTests()
{
    testMelodyRollEditing();
    testRollStructureAndLayers();
    testRollAutoCustom();
    testRollSelectionGestures();
    testRollCommands();
    testRollSelectionLifetime();
    testRollMenus();
    testRollEmptyCardAndLane();
    testRollPaintPerformance();
    testRollReviewFixes();
    testRollFinalReviewFixes();
}
