#include "ui/HeaderBar.h"

// STUB - to be implemented

struct HeaderBar::Impl
{
};

HeaderBar::HeaderBar (ChordbloomProcessor&) : impl (std::make_unique<Impl>()) {}
HeaderBar::~HeaderBar() = default;
void HeaderBar::paint (juce::Graphics&) {}
void HeaderBar::resized() {}
void HeaderBar::refresh() {}
