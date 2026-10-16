#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "plugin/PluginProcessor.h"
#include "ui/Theme.h"

class HeaderBar;
class ChordPaletteView;
class ProgressionLane;
class ControlsPanel;
class KeyboardGuide;
class ExportPanel;

/**
    Layout at the default 1100 x 690 (resizable 960x600 .. 1600x1000, size persisted in the processor):

      +--------------------------------------------------------------------------------+
      | HeaderBar (56): logo, key + scale, sound tone/volume/on, MIDI out, undo/redo    |
      +--------------------------------------------------------------------------------+
      | ChordPaletteView: 5 labelled rows (Triads, Sevenths, Color, Borrowed, Secondary)|
      |   of up to 8 pads each                                                          |
      +--------------------------------------------------------------------------------+
      | ProgressionLane: play/stop, beat ruler, chord blocks sized by length, playhead  |
      +--------------------------------------------------+-----------------------------+
      | ControlsPanel: rhythm + voicing + bass + feel     | KeyboardGuide               |
      |                                                  | ExportPanel (drag chips)    |
      +--------------------------------------------------+-----------------------------+

    The editor is the DragAndDropContainer for pad -> lane drags and does external file drags for MIDI export.
    It listens to the processor (ChangeListener) and runs a 30 Hz timer that works out the "display chord"
    (preview chord, else the playing slot's chord, else the selected slot's chord) and the "context chord" for
    suggestions (selected slot's chord, else the last slot's chord, else none) and forwards them to children.
*/
class ChordbloomEditor : public juce::AudioProcessorEditor,
                         public juce::DragAndDropContainer,
                         private juce::ChangeListener,
                         private juce::Timer
{
public:
    explicit ChordbloomEditor (ChordbloomProcessor&);
    ~ChordbloomEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    ChordbloomProcessor& getProcessor() noexcept { return processor; }

    // Snapshot / test hooks
    ProgressionLane& getProgressionLane() noexcept;
    ChordPaletteView& getPaletteView() noexcept;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    ChordbloomProcessor& processor;
    cb::ui::LookAndFeel lookAndFeel;

    std::unique_ptr<HeaderBar> header;
    std::unique_ptr<ChordPaletteView> palette;
    std::unique_ptr<ProgressionLane> lane;
    std::unique_ptr<ControlsPanel> controls;
    std::unique_ptr<KeyboardGuide> keyboard;
    std::unique_ptr<ExportPanel> exportPanel;
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChordbloomEditor)
};
