#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>

#include "plugin/PluginProcessor.h"

/**
    The chord palette: one labelled row per PaletteSection, up to 8 pads per row.

    Pad: rounded tile tinted with the chord's function colour; big chord name ("Am7"), small roman numeral ("vi7").
    Tooltip = PaletteChord::hint + what a click / double-click / drag does.
    Interaction:
      - mouse down: processor.startPreviewChord(chord); mouse up: processor.stopPreview()
      - double-click (exactly the 2nd click on the same pad; 3rd/4th clicks of a fast run are ignored):
        adds the chord AFTER the lane's selected slot when one is selected, else appends; the lane selection then
        moves onto the new chord so the suggestion glow continues from it. (The lane is found through the parent
        ChordbloomEditor::getProgressionLane(); without an editor parent the chord is appended.)
      - a small "+" affordance appears on hover; clicking it (press and release on it, no drag) adds the chord the
        same way as a double-click
      - drag (> 6 px): starts a drag with cb::ui::drag::makeChordPayload(chord) onto the ProgressionLane; the drag
        may leave the plugin window (allowDraggingToExternalWindows = true) so the editor can drop it into the DAW
        as a MIDI file (ChordbloomEditor::shouldDropFilesWhenDraggedExternally)
    Suggestions: given a context chord, each pad gets a glow ring whose strength is transitionScore(context, pad)
    (only scores >= 0.35 drawn). With no context and an empty progression the glow shows cb::openingScore (good
    first chords). The top 3 suggestions (3 different roots) get a small dot marker. A small legend in a free
    corner of the grid explains the glow.
    The pad matching the "sounding chord" is drawn lit/pressed.
    Accessibility: every pad is exposed as an accessible button (title = chord name, press = add the chord).
*/
class ChordPaletteView : public juce::Component
{
public:
    explicit ChordPaletteView (ChordbloomProcessor&);
    ~ChordPaletteView() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void refresh();                                         // palette or key changed: rebuild pads
    void setContextChord (std::optional<cb::Chord>);        // suggestion source
    void setSoundingChord (std::optional<cb::Chord>);       // highlight

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
