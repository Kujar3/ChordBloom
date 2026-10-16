#include "ui/ExportPanel.h"

// STUB - to be implemented

struct ExportPanel::Impl
{
};

ExportPanel::ExportPanel (ChordbloomProcessor&) : impl (std::make_unique<Impl>()) {}
ExportPanel::~ExportPanel() = default;
void ExportPanel::paint (juce::Graphics&) {}
void ExportPanel::resized() {}
void ExportPanel::refresh() {}
