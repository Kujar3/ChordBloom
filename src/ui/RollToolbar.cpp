#include "ui/RollToolbar.h"

// Roll toolbar (final-spec 7.2-7.5). WP0 STUB (WP6 implements it): an empty component whose hooks return defaults.
// MelodyRoll creates it but does not add it as a child until WP5.

struct RollToolbar::Impl
{
    explicit Impl (ChordbloomProcessor& p) : processor (p) {}

    ChordbloomProcessor& processor;
    cb::Layer layer = cb::Layer::Melody;
    cb::MelodyGenSettings settings;
};

RollToolbar::RollToolbar (ChordbloomProcessor& p) : impl (std::make_unique<Impl> (p))
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
}

RollToolbar::~RollToolbar() = default;

void RollToolbar::paint (juce::Graphics&) {}
void RollToolbar::resized() {}

void RollToolbar::refresh() {}
void RollToolbar::setLayer (cb::Layer layer) { impl->layer = layer; }
void RollToolbar::setSnap (cb::MelodySnap) {}
void RollToolbar::setGrid (cb::RateDivision) {}
void RollToolbar::setVelocityLaneOn (bool) {}

void RollToolbar::generateNewMelody() {}
bool RollToolbar::generateMelody (std::uint32_t) { return false; }
cb::MelodyGenSettings RollToolbar::getGenerateSettings() const { return impl->settings; }
void RollToolbar::setGenerateSettings (const cb::MelodyGenSettings& s) { impl->settings = s; }
void RollToolbar::stepPattern (int) {}
int RollToolbar::getLayoutLevel() const noexcept { return 0; }
juce::Rectangle<int> RollToolbar::getItemBounds (RollToolbarItem) const { return {}; }
