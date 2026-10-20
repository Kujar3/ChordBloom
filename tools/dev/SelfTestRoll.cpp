#include "SelfTestUtil.h"

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
        auto* clear = dynamic_cast<juce::Button*> (findComponentByTitle (roll, "Clear melody"));
        if (clear != nullptr)
            clear->triggerClick();
        pumpMessages (40);
        ed->updateChordDisplays();
        check (clear != nullptr && p.getMelody().empty(), "roll: Clear empties the melody");
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
} // namespace

void cbtest::runRollSelfTests()
{
    testMelodyRollEditing();
}
