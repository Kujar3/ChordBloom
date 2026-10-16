#include "ui/Theme.h"

#include "ChordbloomBinary.h"

// STUB - to be implemented (owner: ui-shell agent)

namespace cb::ui
{
juce::Colour functionColour (cb::HarmonicFunction f)
{
    switch (f)
    {
        case cb::HarmonicFunction::Tonic:       return Colours::tonic;
        case cb::HarmonicFunction::Subdominant: return Colours::subdominant;
        case cb::HarmonicFunction::Dominant:    return Colours::dominant;
        case cb::HarmonicFunction::Borrowed:    return Colours::borrowed;
        case cb::HarmonicFunction::Secondary:   return Colours::secondary;
        case cb::HarmonicFunction::Chromatic:   break;
    }
    return Colours::chromatic;
}

juce::Font font (float height, int) { return juce::Font (juce::FontOptions (height)); }

LookAndFeel::LookAndFeel() {}
LookAndFeel::~LookAndFeel() = default;

juce::Typeface::Ptr LookAndFeel::getTypefaceForFont (const juce::Font& f) { return LookAndFeel_V4::getTypefaceForFont (f); }
} // namespace cb::ui
