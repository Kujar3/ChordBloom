#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "plugin/PluginProcessor.h"

// Top bar: logo/wordmark, key root + scale selectors, preview sound (on/off, tone, volume), MIDI out toggle,
// sync-to-host toggle, undo / redo. Uses APVTS attachments; calls processor.undo()/redo().
class HeaderBar : public juce::Component
{
public:
    explicit HeaderBar (ChordbloomProcessor&);
    ~HeaderBar() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void refresh();   // called by the editor on processor change (e.g. undo/redo enablement)

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
