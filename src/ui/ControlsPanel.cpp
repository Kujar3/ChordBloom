#include "ui/ControlsPanel.h"

// STUB - to be implemented

struct ControlsPanel::Impl
{
};

ControlsPanel::ControlsPanel (ChordbloomProcessor&) : impl (std::make_unique<Impl>()) {}
ControlsPanel::~ControlsPanel() = default;
void ControlsPanel::paint (juce::Graphics&) {}
void ControlsPanel::resized() {}
