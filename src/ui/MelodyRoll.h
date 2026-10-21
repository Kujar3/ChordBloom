#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "plugin/PluginProcessor.h"
#include "ui/RollToolbar.h"

/**
    "Harmonic guide" piano roll of the note layers (phase 2: Melody, Arp, Bass; cb::Layer). The editor shows it in
    place of the chord palette (the "Roll" view); it paints its own card, like the palette. Child 0 is always the grid
    surface (tests send mouse events to getChildComponent (0)); the RollToolbar is added after it.

    Layout (inside a 12 x 10 px padding):
      - Toolbar row: the RollToolbar (final-spec 7.2): layer tabs with play LED and edited mark, Generate (Melody) or
        the "< pattern v >" browser + Reset pill (Arp / Bass), Snap, Grid, velocity-lane toggle and the "..." menu.
        setToolbarRightInset() keeps room on the right for the editor's view switch; the toolbar lays out in the rest.
        (Phase 1.5 had a MELODY title, Play, Clear and Ghosts here: Play is the tab LED, Clear melody and the ghost
        toggle live in the "..." menu.)
      - Chord strip: the name of each progression chord in its function colour, above the beats it covers; an accent
        dot after every slot whose custom notes differ from the auto pattern (getEditedSlots); right-click a name:
        "Reset this chord".
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
      - (phase 2) the current layer's notes on top: auto notes at 70% alpha, custom notes solid, a muted layer at 55%;
        the other layers of this instance as outlines in cb::ui::layerColour (45% alpha, 6% fill) under them, only
        while that layer is on (processor.isLayerOn). One toggle ("Show other layers and instances", cb.melodyGhosts;
        areOtherLayersShown) hides both kinds of ghosts.
      - row range and setTopNote clamping follow cb::layerLimits of the current layer (Melody 36..96, Arp 28..108,
        Bass 24..72); each layer keeps its own snap, grid and top note (cb.<layer>Snap / Grid / TopNote)

    Editing (final-spec 7.7; every gesture works on a working copy of processor.getLayerNotes (getLayer()) taken at
    mouse-down and commits one processor.setLayerNotes on mouse-up only when it changed, so one undo step per gesture;
    editing an auto Arp / Bass makes it custom; every operation is a cb::NoteEdit function):
      - left-click empty space without a selection: draws a note in the grid cell under the mouse (pitch snapped by
        the Snap mode against the chord there, start on the grid line at or before the mouse), with the length and
        velocity of the last note clicked, drawn, resized or velocity-edited in this layer (initially Melody 1 beat /
        0.8, Arp / Bass one grid step / the velocity parameter); keep dragging to move it. With a selection the click
        only deselects.
      - drag an unselected note: moves it alone (the selection is cleared and the note does not stay selected); drag
        a selected note: moves the whole selection (group-clamped, re-snapped per note). The pitch is auditioned
        (processor.startPreviewLayerNote) whenever it changes (a group move auditions the anchor note only).
      - drag a note's right edge: resizes it, or the whole selection when it is selected (at least one grid step)
      - Ctrl + click (isCommandDown): toggles the note in the selection; Ctrl + drag (> 3 px, from anywhere): marquee
        that replaces the selection, Ctrl + Shift + drag adds to it; Shift + drag: duplicates the note / selection to
        the drop point
      - hold Alt while drawing / moving / resizing: no grid (fine 1/48-beat steps)
      - right-click a note: deletes that note only; right-drag deletes every note it touches; right-click on empty
        space opens the roll menu (with Paste here and Reset this chord)
      - menus (toolbar "...", empty-grid right-click, the "N selected" pill, a tab right-click, the velocity lane,
        a chord-strip right-click): one builder (createMenu), items map to Command / performCommand or, for Snap /
        Grid, to setSnap / setGrid; items that do not apply are disabled
      - velocity lane (setVelocityLaneVisible, cb.rollVelLane): one stalk per note; dragging a stalk sets that note's
        velocity, or offsets the whole selection when the note is selected; one undo step per drag
      - pills at the grid's top-right: transient hints, "N selected · ...", "<Layer> is off · Turn on"; an empty Melody
        shows a centred "Generate a melody" card: only its button (getEmptyStateButtonArea) captures clicks, anywhere
        else on the card a click draws or deselects as usual
      - mouse wheel: scrolls the pitch range
      - nothing in the roll takes keyboard focus (FL keeps its shortcuts); right-click never acts as a left click
    Selection is UI state: never undoable, re-identified (cb::findNotes) after the roll's own commits, cleared when
    processor.getLayerSerial() changes from outside a gesture and the current layer's notes differ from the ones the
    selection was made on (the serial also moves on parameter-only changes). The roll's 30 Hz timer polls the serial
    and refreshes itself and its toolbar when it changes. Copy / Cut / Paste use one process-wide cb::NoteClip.
    Snap modes, grids, the visible pitch ranges, the current layer, the velocity lane and the ghost toggle are remembered
    in the APVTS state tree (plugin state).
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
    int getNumGhostNotesShown() const noexcept;   // other instances' ghost notes inside the visible area (not this
                                                  // instance's other layers: getNumOtherLayerNotesShown)

    //==============================================================================
    // (Phase 2) getSnap / setSnap / getGrid / setGrid / getTopNote / setTopNote / getNoteBounds / getCellCentre act on
    // the current layer (identical to before while Melody is shown); getNoteBounds indexes
    // processor.getLayerNotes (getLayer()).notes; setTopNote clamps to cb::layerLimits (getLayer()).
    cb::Layer getLayer() const noexcept;
    void setLayer (cb::Layer);                          // remembered as cb.rollLayer
    std::vector<int> getSelection() const;              // ascending indices into processor.getLayerNotes (getLayer()).notes
    void setSelection (const std::vector<int>&);

    enum class Command
    {
        SelectAll, SelectNone, Copy, Cut, Paste, PasteHere, Duplicate, Delete, Quantize, Legato,
        VelocitySoft, VelocityMedium, VelocityDefault, VelocityHard, VelocityUp, VelocityDown,
        StepUp, StepDown, SemitoneUp, SemitoneDown, OctaveUp, OctaveDown,
        ResetLayer, ResetChord, ClearMelody,
        ToggleLayerOn,                        // "Play <layer>" (ticked while isLayerOn): setLayerOn (layer, ! isLayerOn)
        ToggleVelocityLane, ToggleOtherLayers // VIEW: setVelocityLaneVisible / setOtherLayersShown (! current)
    };
    // As chosen from a menu. atBeat: the position for PasteHere and ResetChord (ignored otherwise). false when the
    // command does not apply (then it is disabled in the menus). Note commands act on the selection, or on every note
    // of the layer when nothing is selected. Layer commands act on the current layer.
    bool performCommand (Command, double atBeat = -1.0);

    // "Show other layers and instances" (cb.melodyGhosts): this instance's other layers and other instances' melodies.
    bool areOtherLayersShown() const noexcept;
    void setOtherLayersShown (bool);

    // Menu sections (final-spec 7.8). createMenu builds exactly what the entry point shows: All = the whole menu (toolbar
    // "..."; an empty-grid right-click is All with atBeat >= 0, adding Paste here / Reset this chord), Selection = the
    // selection pill, Layer = a tab right-click (`layer`; the current layer when nullopt), Velocity = a right-click in
    // the velocity lane, Chord = a chord-strip right-click at atBeat ("Reset this chord"). Every shown menu goes
    // through createMenu, and getLastMenuShown reports the last section opened (nullopt before any): test hooks for
    // the async PopupMenus.
    enum class MenuSection { All, Selection, Layer, Velocity, Chord };
    juce::PopupMenu createMenu (MenuSection, double atBeat = -1.0, std::optional<cb::Layer> layer = std::nullopt);
    std::optional<MenuSection> getLastMenuShown() const noexcept;
    juce::Rectangle<float> getChordStripArea() const noexcept;   // the chord names row above the grid
    int getNumOtherLayerNotesShown() const noexcept;             // this instance's other-layer outlines in view
    // Test hook: how many times refresh() / the serial poll laid out and repainted the whole surface. Only a change the
    // roll shows counts: of another layer only its outlines in view (not a velocity-only re-render, not a layer that is
    // off or out of view); nothing is polled or synced while the roll is hidden (the Chords view, a minimised window).
    int getNumSurfaceRefreshes() const noexcept;
    // Test hook: the row the hover name pill marks (-1 off the grid). While a draw preview shows it is the preview's
    // pitch, so with Snap on the name matches the note a click writes, not the pointer's row.
    int getHoverLabelRow() const noexcept;

    bool isVelocityLaneVisible() const noexcept;
    void setVelocityLaneVisible (bool);                               // remembered as cb.rollVelLane
    juce::Rectangle<float> getVelocityLaneArea() const noexcept;      // empty when hidden
    juce::Rectangle<float> getEmptyStateButtonArea() const noexcept;  // "Generate a melody" card button; empty when hidden
    juce::Rectangle<float> getSelectionPillArea() const noexcept;     // "N selected" pill; empty without a selection
    juce::Rectangle<float> getLayerOffPillArea() const noexcept;      // "<Layer> is off · Turn on"; empty when hidden
    RollToolbar& getToolbar() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
