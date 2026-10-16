#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "plugin/PluginProcessor.h"

/**
    Timeline of the progression.
      - Header: round play/stop button (processor.setLocalPlayback), loop length readout ("4 bars"), a "Playing with FL"
        pill while the progression loops along with the host (click = turn Play with FL / syncToHost off),
        a "Move chords to <key> (+n)" pill when processor.bestTranspositionForKey() != 0, a "Spark" split pill
        (click = cb::sparkProgression with a new seed; chevron / right-click = Mood, Chords, Beats per chord; the
        settings are remembered in the APVTS state tree), a "Presets" menu (cb::presetsFor grouped by mood) and Clear.
        Everything that replaces the progression goes through processor.setProgression / transposeProgression (undoable).
      - Beat ruler with bar numbers; chord blocks whose width is proportional to lengthBeats (lane scales to fit
        max(totalBeats, 16) beats). Block shows chord name, roman numeral, function colour bar, length.
      - Playhead line while playing (processor.getPlaybackBeat), playing block glows.
      - Click selects a block (and previews it while held); double-click does nothing more (safe for repeated taps).
        Remove with the hover "x" or right-click > Delete. A small marker after the selected block shows where the
        palette adds chords (after the selection).
      - Drag blocks to reorder (internal drag with makeSlotPayload; the drag may also leave the plugin window so the
        editor can drop the slot into the DAW as MIDI). Dropping a palette pad near a block edge inserts there (an
        insertion caret shows where); dropping it on the middle of a block replaces that block's chord, keeping its
        length and octave. Empty lane shows a friendly hint plus Spark / Presets buttons.
      - Right-click block: PopupMenu with Change to (same-root variants), Suggested here, Length (1,2,3,4,6,8,12,16
        beats), Inversion (Auto, Root, 1st, 2nd, 3rd), Octave (-2..+2), Transpose all, Duplicate, Delete.
        Right-click empty space / header: Transpose, Move to key, Spark, Presets, Clear.
      - Mouse wheel over a block changes its length by 1 beat; dragging the block's right edge resizes by whole beats.
      - Nothing in the lane takes keyboard focus (the host keeps its shortcuts).
*/
class ProgressionLane : public juce::Component,
                        public juce::DragAndDropTarget
{
public:
    explicit ProgressionLane (ChordbloomProcessor&);
    ~ProgressionLane() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void refresh();                         // progression changed
    int getSelectedSlot() const noexcept;   // -1 when none
    void setSelectedSlot (int index);
    void setSelectedSlot (int index, bool notify);   // added in round 2: notify = true also fires onSelectionChanged
    std::function<void (int)> onSelectionChanged;

    // juce::DragAndDropTarget
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override;
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
