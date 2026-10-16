#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "plugin/PluginProcessor.h"

/**
    Timeline of the progression.
      - Left: round play/stop button (processor.setLocalPlayback), loop length readout ("4 bars"), clear button.
      - Beat ruler with bar numbers; chord blocks whose width is proportional to lengthBeats (lane scales to fit
        max(totalBeats, 16) beats). Block shows chord name, roman numeral, function colour bar, length.
      - Playhead line while playing (processor.getPlaybackBeat), playing block glows.
      - Click selects a block (and previews it while held); double-click removes it.
      - Drag blocks to reorder (internal drag with makeSlotPayload); dropping palette pads inserts at the drop position
        (an insertion caret shows where). Empty lane shows a friendly hint ("Drag chords here or double-click a pad").
      - Right-click block: PopupMenu with Length (1,2,3,4,6,8,12,16 beats), Inversion (Auto, Root, 1st, 2nd, 3rd),
        Octave (-2..+2), Duplicate, Delete.
      - Mouse wheel over a block changes its length by 1 beat; dragging the block's right edge resizes by whole beats.
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
