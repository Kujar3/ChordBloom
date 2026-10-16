#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "plugin/PluginProcessor.h"

// Bottom-left panel. Three labelled groups with APVTS attachments:
//   Rhythm:  pattern (combo), rate (segmented 1/4 1/8 1/16 1/8T), swing (knob), gate (knob)
//   Voicing: style (segmented Close/Open/Spread), voice leading (toggle), octave (-3..+3 stepper), bass mode (combo)
//   Feel:    velocity (knob), humanize (knob), scale lock (toggle), chord trigger (toggle) + 7ths (toggle)
class ControlsPanel : public juce::Component
{
public:
    explicit ControlsPanel (ChordbloomProcessor&);
    ~ControlsPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
