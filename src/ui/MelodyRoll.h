#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "plugin/PluginProcessor.h"

/**
    Melody "harmonic guide" piano roll. The editor shows it in place of the chord palette (Melody view); it paints its
    own card, like the palette.

    Layout (inside a 12 x 10 px padding):
      - Toolbar row: MELODY title + note count, Snap (Off / Scale / Chord), Grid (1/4 1/8 1/16 1/8T), a "Play" switch
        (melodyOn parameter), Clear, and a "Ghosts" chip while other Chordbloom instances have melodies. Items drop
        their labels / shrink to fit; setToolbarRightInset() keeps room on the right for the editor's view switch.
      - Chord strip: the name of each progression chord in its function colour, above the beats it covers.
      - Grid: one row per MIDI note (about 20 rows tall, scrolled with the mouse wheel; C rows labelled "C5" with FL's
        octave numbers like the keyboard guide), beats left to right. The grid's x range and beat scale are exactly
        the progression lane's track (12 px inset, max (16 beats, the progression rounded up to whole bars)), so the
        chord blocks below line up with the notes above them.

    Harmonic guide shading:
      - scale tones of the key get a soft tint, black-key rows are a little darker
      - over each chord's beats its chord tones are tinted with the chord's function colour, the root more strongly
      - past the end of the progression the grid is dimmed (notes there are kept but do not play)
      - notes are coloured by what they are at their start: the chord's function colour for chord tones, light for
        other scale tones, grey for notes outside the key; brightness follows velocity
      - while playing: a playhead, and the notes that are sounding glow
      - ghost notes (ChordbloomProcessor::getGhostMelodies): other instances' melodies as faint outlines, repeated
        every loop of that instance's progression

    Editing (all edits go through processor.setMelody, one undo step per gesture, committed on mouse up):
      - left-click empty space: draws a note in the grid cell under the mouse (pitch snapped by the Snap mode against
        the chord there, start on the grid line at or before the mouse), using the last length set by a resize
        (initially one beat); keep dragging to move it
      - drag a note: moves it by whole grid steps and rows (pitch re-snapped at the new position); the pitch is
        auditioned (processor.startPreviewMelodyNote) whenever it changes
      - drag a note's right edge: resizes it to the grid (at least one grid step)
      - hold Alt while drawing / moving / resizing: no grid (fine 1/48-beat steps)
      - right-click a note: deletes it; right-drag deletes every note it touches; right-click on empty space opens a
        menu (Snap, Grid, Octave up / down, Clear melody)
      - mouse wheel: scrolls the pitch range
      - nothing in the roll takes keyboard focus (FL keeps its shortcuts)
    Snap mode, grid, the visible pitch range and the ghost toggle are remembered in the APVTS state tree (plugin state).
*/
class MelodyRoll : public juce::Component
{
public:
    explicit MelodyRoll (ChordbloomProcessor&);
    ~MelodyRoll() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void refresh();   // progression, melody, key or parameters changed (the editor calls it on every processor change)

    // Pixels at the right end of the toolbar row left free for a control the editor places there (the view switch).
    void setToolbarRightInset (int pixels);

    //==============================================================================
    // Settings (also reachable from the toolbar / menu)
    cb::MelodySnap getSnap() const noexcept;
    void setSnap (cb::MelodySnap);
    cb::RateDivision getGrid() const noexcept;
    void setGrid (cb::RateDivision);

    // Visible pitch range: the highest fully visible row is getTopNote(); setTopNote clamps so the grid never scrolls
    // past kMelodyHighestNote / kMelodyLowestNote.
    int getTopNote() const noexcept;
    int getBottomNote() const noexcept;   // lowest row that is at least half visible
    void setTopNote (int midiNote);

    //==============================================================================
    // Test / snapshot hooks (local coordinates)
    juce::Rectangle<float> getGridArea() const noexcept;
    juce::Point<float> getCellCentre (double beat, int midiNote) const;   // centre of the row at that beat
    juce::Rectangle<float> getNoteBounds (int noteIndex) const;           // index into processor.getMelody().notes
    int getNumGhostNotesShown() const noexcept;                           // ghost notes inside the visible area

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
