#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "plugin/PluginProcessor.h"

// "Drag to DAW" panel: three draggable chips (Chords, Bass, Both). Dragging a chip writes
// processor.writeMidiToTempFile(part) and starts juce::DragAndDropContainer::performExternalDragDropOfFiles
// so the clip can be dropped onto FL Studio's playlist / channel rack / piano roll.
// Plus a "Save .mid..." button (async FileChooser -> processor.saveMidiFile). Chips are disabled when the
// progression is empty. With Bass mode Off the Bass chip refuses to drag (and says why) and "Both" carries chords only.
// After a chip was dropped outside the plugin while Play with FL (syncToHost) is on, a clickable toast
// "Dropped into FL? Chordbloom still plays along" offers to turn syncToHost off.
// Nothing in the panel takes keyboard focus.
class ExportPanel : public juce::Component
{
public:
    explicit ExportPanel (ChordbloomProcessor&);
    ~ExportPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void refresh();   // progression changed (enable/disable chips)

    // Added in round 2 (I2): shows the "Dropped into FL? Chordbloom still plays along - click to mute" toast when
    // syncToHost is on (otherwise does nothing; throttled to once a minute). The editor calls this when a pad or lane
    // block was dragged out of the plugin as a MIDI file (shouldDropFilesWhenDraggedExternally returned true).
    void showPlaysAlongHint();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
