#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "plugin/PluginProcessor.h"

// Top bar: logo/wordmark, key root + scale selectors, preview sound (on/off, tone, volume), MIDI out toggle,
// sync-to-host toggle, undo / redo. Uses APVTS attachments; calls processor.undo()/redo().
// Round 2 (ui-shell): the sync toggle is labelled "Play with FL", a "MIDI out part" chip (Chords + Bass / Chords / Bass)
// sits next to MIDI out, and a "?" button opens a help popover over the editor.
class HeaderBar : public juce::Component
{
public:
    explicit HeaderBar (ChordbloomProcessor&);
    ~HeaderBar() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void refresh();   // called by the editor on processor change (e.g. undo/redo enablement)

    //==============================================================================
    // Help popover ("?" button). It is added to the header's parent component (the editor) so it can float over the
    // whole UI; clicking anywhere outside the card (or its close button) dismisses it. (Added by ui-shell, round 2.)
    void setHelpVisible (bool shouldBeVisible);
    bool isHelpVisible() const;

    // The help card on its own at its ideal height for `width` (0 = default width). Used by the snapshot tool.
    static std::unique_ptr<juce::Component> createHelpContent (int width = 0);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
