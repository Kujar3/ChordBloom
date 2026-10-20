#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/Layers.h"
#include "core/MelodyGen.h"
#include "plugin/PluginProcessor.h"

// Items of the roll toolbar (test / snapshot hooks for RollToolbar::getItemBounds).
enum class RollToolbarItem
{
    Tabs, TabMelody, TabArp, TabBass, LedMelody, LedArp, LedBass,
    Generate, GenerateMenu,                  // Melody tab: the split button's body and its "style v" part
    BrowserPrev, BrowserName, BrowserNext,   // Arp / Bass tab: "< pattern v >"
    Reset,                                   // Arp / Bass tab while the layer is custom
    Snap, Grid, VelocityLane, More,
    NumItems
};

/**
    The roll's toolbar row (phase 2), a child of MelodyRoll, which places it left of the editor's view switch.
    Left to right: layer tabs (LED, name, edited mark), the layer's main control (Melody: "Generate" split button and
    generator menu; Arp / Bass: "< pattern v >" browser and pattern menu, plus a Reset pill while the layer is custom),
    Snap segments, Grid segments, velocity-lane toggle, "..." button. Shrinks in levels 0..5 (final-spec 7.2); the first
    level that fits wins. Generating, pattern picks (the parameter with a host gesture, then beginUserLayerPick), LED
    clicks and Reset go straight to the processor; per-layer view settings and selection commands belong to MelodyRoll
    (callbacks). Never takes keyboard focus; right-clicks never act as left clicks (a right-click on a tab calls
    onTabMenu, elsewhere nothing happens).
    Every item is a region painted and hit-tested by RollToolbar itself (no child components), so tests send mouse
    events to the toolbar at getItemBounds (item).getCentre(); menus anchor with
    PopupMenu::Options::withTargetScreenArea (localAreaToGlobal (getItemBounds (item))).
*/
class RollToolbar : public juce::Component
{
public:
    explicit RollToolbar (ChordbloomProcessor&);
    ~RollToolbar() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void refresh();                    // processor / layer state changed (cheap; MelodyRoll calls it freely)
    void setLayer (cb::Layer);         // which layer's controls to show
    void setSnap (cb::MelodySnap);     // reflect the current layer's view settings
    void setGrid (cb::RateDivision);
    void setVelocityLaneOn (bool);

    std::function<void (cb::Layer)> onLayerChosen;
    std::function<void (cb::Layer)> onTabMenu;                    // right-click on a tab: that layer's menu section
    std::function<void (cb::MelodySnap)> onSnapChosen;
    std::function<void (cb::RateDivision)> onGridChosen;
    std::function<void (bool)> onVelocityLaneToggled;
    std::function<void (juce::Rectangle<int> screenArea)> onMoreClicked;  // MelodyRoll opens its menu (All) there
    std::function<void (const juce::String& message)> onHint;     // MelodyRoll shows it as a transient pill
    std::function<void()> onMelodyGenerated;                      // after a successful generate (MelodyRoll recentres)

    // Actions, also the test / snapshot hooks
    void generateNewMelody();                                   // the Generate body click (random seeds, up to 8 tries)
    bool generateMelody (std::uint32_t seed);                   // the current settings with exactly this seed
    cb::MelodyGenSettings getGenerateSettings() const;          // from cb.gen.* (defaults: final-spec 3.3)
    void setGenerateSettings (const cb::MelodyGenSettings&);    // as picking in the generator menu (final-spec 7.4)
    void stepPattern (int direction);                           // as clicking < (-1) or > (+1) on the current layer
    int getLayoutLevel() const noexcept;                        // 0 = everything .. 5 = most compact
    juce::Rectangle<int> getItemBounds (RollToolbarItem) const; // toolbar coordinates; empty when hidden

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
