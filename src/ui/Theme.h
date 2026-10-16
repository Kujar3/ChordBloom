#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/Theory.h"

// Visual language for Chordbloom. Dark slate UI that sits comfortably next to FL Studio's blue-grey,
// soft rounded surfaces, one accent colour per harmonic function.
namespace cb::ui
{
namespace Colours
{
    inline const juce::Colour background   { 0xff1a1f26 };   // editor background
    inline const juce::Colour panel        { 0xff222932 };   // section panels
    inline const juce::Colour panelRaised  { 0xff2a323c };   // pads, chips, inputs
    inline const juce::Colour panelHover   { 0xff323b47 };
    inline const juce::Colour outline      { 0xff38424e };
    inline const juce::Colour text         { 0xffe9eef3 };
    inline const juce::Colour textDim      { 0xff93a1af };
    inline const juce::Colour textFaint    { 0xff5f6c79 };
    inline const juce::Colour accent       { 0xff7ad3b8 };   // brand mint (playhead, focus, primary buttons)

    inline const juce::Colour tonic        { 0xff58c9a9 };   // teal
    inline const juce::Colour subdominant  { 0xfff1b562 };   // amber
    inline const juce::Colour dominant     { 0xfff0766f };   // coral
    inline const juce::Colour borrowed     { 0xffaa8cf2 };   // lavender
    inline const juce::Colour secondary    { 0xff62a8f2 };   // sky
    inline const juce::Colour chromatic    { 0xff9aa5b1 };   // grey
}

juce::Colour functionColour (cb::HarmonicFunction);

// Inter from embedded binary data. weight: 400 (regular), 600 (semibold), 700 (bold). Falls back to the system sans.
juce::Font font (float height, int weight = 400);

constexpr float kCornerRadius = 8.0f;

class LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel();
    ~LookAndFeel() override;

    // Implementations live in Theme.cpp (combo boxes, popup menus, toggle buttons, text buttons, rotary + linear
    // sliders, labels, tooltips, scrollbars) - all restyled to the palette above.
    juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override;
};

} // namespace cb::ui
