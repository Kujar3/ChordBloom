#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "plugin/PluginProcessor.h"

// "Drag to DAW" panel (phase 2, final-spec 7.10). Caption row: "DRAG TO DAW" + a detail line (dropped when narrow;
// status toasts replace the caption text only) and a "Save v" pill on the right. Below it five equal draggable chips:
// Chords (ch 1), Bass (ch 2), Melody (ch 3), Arp (ch 4), All (one track per playing part). Dragging a chip writes
// processor.writeMidiToTempFile (part) and starts juce::DragAndDropContainer::performExternalDragDropOfFiles, so the
// clip can be dropped onto FL Studio's playlist / channel rack / piano roll.
//  - Chips are disabled when the progression is empty. Bass: an OFF badge (chips narrower than 70 px: no badge, the
//    label dimmed and a slash over the grip instead) and a refusal (saying why) when its content is empty (auto with
//    bassMode Off); the tooltip adds ", muted in Chordbloom" while bassOn is off. Melody: enabled
//    once a melody note starts inside the loop. Arp: enabled with chords, exports while muted. All: the tooltip lists
//    the tracks. Grip colours: Chords tonic, Bass / Melody / Arp cb::ui::layerColour, All accent.
//  - Right-click a chip: one item "Save <Part> as .mid...". The Save menu: All parts, Chords + bass (ExportPart::Both,
//    no chip), Chords only, Bass only, Melody only, Arp only (async FileChooser -> processor.saveMidiFile).
// After a chip was dropped outside the plugin while Play with FL (syncToHost) is on, a clickable toast
// "Dropped into FL? Chordbloom still plays along" offers to turn syncToHost off.
// Nothing in the panel takes keyboard focus; a right-click never acts as a left click.
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

    // (Added in round 3, F12.) A pad / block drag that may have left the window as a MIDI file just ended (mouse
    // released): shows the hint above only when the release happened outside the plug-in window, like the chips do.
    // (A drag cancelled with Escape outside the window still counts as a drop: JUCE does not report the OS result.)
    void notifyExternalDragFinished();

    // (Phase 2) Test hooks, panel coordinates; empty when there is no such chip (Both has no chip).
    juce::Rectangle<int> getChipBounds (ChordbloomProcessor::ExportPart) const;
    juce::Rectangle<int> getSaveButtonBounds() const;
    // (Phase 2) The menus exactly as the Save pill and a chip right-click show them (test hooks for the async menus).
    juce::PopupMenu createSaveMenu() const;
    juce::PopupMenu createChipMenu (ChordbloomProcessor::ExportPart) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
