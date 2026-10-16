#include "ui/ChordPaletteView.h"

// STUB - to be implemented

struct ChordPaletteView::Impl
{
};

ChordPaletteView::ChordPaletteView (ChordbloomProcessor&) : impl (std::make_unique<Impl>()) {}
ChordPaletteView::~ChordPaletteView() = default;
void ChordPaletteView::paint (juce::Graphics&) {}
void ChordPaletteView::resized() {}
void ChordPaletteView::refresh() {}
void ChordPaletteView::setContextChord (std::optional<cb::Chord>) {}
void ChordPaletteView::setSoundingChord (std::optional<cb::Chord>) {}
