#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>

#include "plugin/PluginProcessor.h"

// Harmonic guide keyboard (3 octaves, C3..B5 = MIDI 48..83, auto-shifted to contain the display chord's notes).
//   - scale tones: subtly tinted keys + small dot; tonic marked
//   - chord tones of the display chord: function-coloured key fill
//   - sounding notes (processor.getSoundingNotes): bright accent glow
//   - click a key: processor.startPreviewNote / stopPreview on release
// Runs its own ~30 Hz timer to poll sounding notes; repaints only on change.
class KeyboardGuide : public juce::Component
{
public:
    explicit KeyboardGuide (ChordbloomProcessor&);
    ~KeyboardGuide() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void setDisplayChord (std::optional<cb::Chord>);
    void refresh();   // key changed

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
