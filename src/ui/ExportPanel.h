#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "plugin/PluginProcessor.h"

// "Drag to DAW" panel: three draggable chips (Chords, Bass, Both). Dragging a chip writes
// processor.writeMidiToTempFile(part) and starts juce::DragAndDropContainer::performExternalDragDropOfFiles
// so the clip can be dropped onto FL Studio's playlist / channel rack / piano roll.
// Plus a "Save .mid..." button (async FileChooser -> processor.saveMidiFile). Chips are disabled when the
// progression is empty.
class ExportPanel : public juce::Component
{
public:
    explicit ExportPanel (ChordbloomProcessor&);
    ~ExportPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void refresh();   // progression changed (enable/disable chips)

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
