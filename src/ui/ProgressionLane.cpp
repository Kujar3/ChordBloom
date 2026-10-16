#include "ui/ProgressionLane.h"

// STUB - to be implemented (owner: ui-chords agent)

struct ProgressionLane::Impl
{
    int selected = -1;
};

ProgressionLane::ProgressionLane (ChordbloomProcessor&) : impl (std::make_unique<Impl>()) {}
ProgressionLane::~ProgressionLane() = default;

void ProgressionLane::paint (juce::Graphics&) {}
void ProgressionLane::resized() {}
void ProgressionLane::refresh() {}
int ProgressionLane::getSelectedSlot() const noexcept { return impl->selected; }
void ProgressionLane::setSelectedSlot (int index) { impl->selected = index; }

bool ProgressionLane::isInterestedInDragSource (const SourceDetails&) { return false; }
void ProgressionLane::itemDragEnter (const SourceDetails&) {}
void ProgressionLane::itemDragMove (const SourceDetails&) {}
void ProgressionLane::itemDragExit (const SourceDetails&) {}
void ProgressionLane::itemDropped (const SourceDetails&) {}
