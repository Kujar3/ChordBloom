#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "plugin/PluginProcessor.h"

// Bottom-left panel. Three labelled groups with APVTS attachments:
//   Rhythm:  pattern (combo), rate (segmented 1/4 1/8 1/16 1/8T), swing (knob), gate (knob)
//   Voicing: style (segmented Close/Open/Spread), voice leading (toggle), octave (-3..+3 stepper), bass mode (combo)
//   Feel:    velocity (knob), humanize (knob), scale lock (toggle), chord trigger (toggle) + 7ths (toggle)
// (Phase 2, final-spec 7.11) The Bass combo lists the 15 modes under the headings "Classic" (Off..Pulse) and
// "Patterns (fixed grooves)" (item ids index + 1). Its showPopup is overridden: it builds that PopupMenu itself and shows
// it with showMenuAsync; in the callback, result 0 does nothing, any other result sets bassMode with a host gesture and
// then, for a mode other than Off, calls processor.beginUserLayerPick (Bass) (a custom bass resets, bassOn turns on).
// No flag survives a dismissed popup, so onChange (automation, state loads) never resets anything. The caption stays
// "Bass", followed by small glyphs (a pencil while the bass layer is custom, a crossed speaker while bassOn is off);
// the words go into the combo's tooltip. The editor calls refresh() on every change message and whenever
// processor.getLayerSerial changes (its 30 Hz timer), so the glyphs and the Rate / Swing dimming (final-spec 7.11)
// follow parameter-only changes (automation, LED clicks) too.
class ControlsPanel : public juce::Component
{
public:
    explicit ControlsPanel (ChordbloomProcessor&);
    ~ControlsPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void refresh();                           // (phase 2) processor changed: Bass glyphs "edited" / "muted"
    void pickBassModeAsUser (cb::BassMode);   // (phase 2) test hook: exactly what choosing it in the Bass popup does
    juce::String getBassStateText() const;    // (phase 2) test hook: "", "edited", "muted" or "edited, muted"

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
