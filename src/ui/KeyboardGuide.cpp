#include "ui/KeyboardGuide.h"

// STUB - to be implemented

struct KeyboardGuide::Impl
{
};

KeyboardGuide::KeyboardGuide (ChordbloomProcessor&) : impl (std::make_unique<Impl>()) {}
KeyboardGuide::~KeyboardGuide() = default;
void KeyboardGuide::paint (juce::Graphics&) {}
void KeyboardGuide::resized() {}
void KeyboardGuide::setDisplayChord (std::optional<cb::Chord>) {}
void KeyboardGuide::refresh() {}
