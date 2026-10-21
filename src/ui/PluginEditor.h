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
class MelodyRoll;

/**
    Layout at the default 1100 x 690 (resizable 960x560 .. 1600x1000, size persisted in the processor):

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

    Melody view (added with the melody roll): a "Chords | Melody" switch at the top-right of the palette card (in the
    free last cell of the Triads row) swaps the ChordPaletteView for the MelodyRoll in the same area, so the roll sits
    right above the lane and its beats line up with the chord blocks. The choice is remembered in the plugin state.

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
    MelodyRoll& getMelodyRoll() noexcept;
    ExportPanel& getExportPanel() noexcept;       // (phase 2)
    ControlsPanel& getControlsPanel() noexcept;   // (phase 2)
    HeaderBar& getHeaderBar() noexcept;           // (phase 2)

    // Melody view (the view switch calls this too). Remembered in the APVTS state tree.
    void setMelodyViewVisible (bool shouldShowMelody);
    bool isMelodyViewVisible() const noexcept;

    // Recomputes the display / context / sounding chords and pushes them to the children right away
    // (normally done by the 30 Hz timer; used by the snapshot tool where timers do not run).
    void updateChordDisplays();

    // Help popover of the header's "?" button (snapshot / test hook; added by ui-shell, round 2).
    void setHelpVisible (bool shouldBeVisible);
    bool isHelpVisible() const;

    // Pad / lane-block drags that leave the plugin window become MIDI file drags (a chord payload -> a one-chord clip,
    // a slot payload -> that slot's clip), so a single chord can be dropped straight into FL's playlist / piano roll.
    bool shouldDropFilesWhenDraggedExternally (const juce::DragAndDropTarget::SourceDetails&, juce::StringArray& files,
                                               bool& canMoveFiles) override;

    // Initial / restored editor size: `requested` clamped to the resizable range and to the given screen area (the
    // display's user area, minus room for the host's plug-in window frame), so the editor always fits on screen.
    // An empty area only applies the resizable range. (Added by ui-shell, round 2; public for tests.)
    static juce::Point<int> clampSizeToScreen (juce::Point<int> requested, juce::Rectangle<int> screenUserArea);

    static constexpr int kDefaultWidth = 1100, kDefaultHeight = 690;
    static constexpr int kMinWidth = 960, kMinHeight = 560;   // 560 (was 600): fits a 1080p screen at 150% scaling
    static constexpr int kMaxWidth = 1600, kMaxHeight = 1000;

    void parentHierarchyChanged() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void fitToCurrentDisplay();

    void refreshChildren();
    void pushChords (bool force);
    void layoutViewSwitch();
    void updateViewSwitch();

    juce::Rectangle<int> paletteArea, laneArea, controlsArea, keyboardArea, exportArea;
    std::optional<cb::Chord> lastSounding, lastDisplay, lastContext;
    std::vector<int> lastDisplayNotes;
    bool chordsPushed = false;
    bool persistSize = false;   // resized() stores the size in the processor only after the constructor restored it
    int focusSweepCountdown = 0;
    bool fittedToDisplay = false;   // fitToCurrentDisplay() ran once the editor was on screen
    bool externalDropPending = false;   // a pad / block went out as a MIDI file: show the "plays along" hint on release
    uint64_t lastLayerSerial = 0;       // (phase 2) getLayerSerial() last pushed to the layer UI (the timer polls it)

    ChordbloomProcessor& processor;
    cb::ui::LookAndFeel lookAndFeel;

    std::unique_ptr<HeaderBar> header;
    std::unique_ptr<ChordPaletteView> palette;
    std::unique_ptr<ProgressionLane> lane;
    std::unique_ptr<ControlsPanel> controls;
    std::unique_ptr<KeyboardGuide> keyboard;
    std::unique_ptr<ExportPanel> exportPanel;
    std::unique_ptr<MelodyRoll> melodyRoll;
    std::unique_ptr<juce::Component> viewSwitch;   // "Chords | Melody" (PluginEditor.cpp)
    bool melodyView = false;
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChordbloomEditor)
};
