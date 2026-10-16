#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>

#include "plugin/PluginProcessor.h"

/**
    The chord palette: one labelled row per PaletteSection, up to 8 pads per row.

    Pad: rounded tile tinted with the chord's function colour; big chord name ("Am7"), small roman numeral ("vi7").
    Tooltip = PaletteChord::hint.
    Interaction:
      - mouse down: processor.startPreviewChord(chord); mouse up: processor.stopPreview()
      - double-click: processor.addChord(chord) (append)
      - drag (> 6 px): starts an internal drag with cb::ui::drag::makeChordPayload(chord) onto the ProgressionLane
      - a small "+" affordance appears on hover; clicking it appends the chord
    Suggestions: given a context chord, each pad gets a glow ring whose strength is transitionScore(context, pad)
    (only scores >= 0.35 drawn; top 3 get a small dot marker). No context -> no glow.
    The pad matching the "sounding chord" is drawn lit/pressed.
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
