#include "ui/PluginEditor.h"

#include "ui/ChordPaletteView.h"
#include "ui/ControlsPanel.h"
#include "ui/ExportPanel.h"
#include "ui/HeaderBar.h"
#include "ui/KeyboardGuide.h"
#include "ui/ProgressionLane.h"

// STUB - to be implemented (owner: ui-shell agent)

ChordbloomEditor::ChordbloomEditor (ChordbloomProcessor& p)
    : AudioProcessorEditor (p), processor (p)
{
    header = std::make_unique<HeaderBar> (p);
    palette = std::make_unique<ChordPaletteView> (p);
    lane = std::make_unique<ProgressionLane> (p);
    controls = std::make_unique<ControlsPanel> (p);
    keyboard = std::make_unique<KeyboardGuide> (p);
    exportPanel = std::make_unique<ExportPanel> (p);

    for (juce::Component* c : std::initializer_list<juce::Component*> { header.get(), palette.get(), lane.get(),
                                                                        controls.get(), keyboard.get(), exportPanel.get() })
        addAndMakeVisible (c);

    setSize (1100, 690);
}

ChordbloomEditor::~ChordbloomEditor() = default;

void ChordbloomEditor::paint (juce::Graphics& g) { g.fillAll (cb::ui::Colours::background); }

void ChordbloomEditor::resized()
{
    // Rough placeholder layout (ui-shell agent replaces this)
    auto r = getLocalBounds();
    header->setBounds (r.removeFromTop (56));
    r.reduce (10, 8);
    palette->setBounds (r.removeFromTop (300));
    r.removeFromTop (8);
    lane->setBounds (r.removeFromTop (150));
    r.removeFromTop (8);
    auto right = r.removeFromRight (440);
    controls->setBounds (r);
    exportPanel->setBounds (right.removeFromBottom (48));
    keyboard->setBounds (right);
}

ProgressionLane& ChordbloomEditor::getProgressionLane() noexcept { return *lane; }
ChordPaletteView& ChordbloomEditor::getPaletteView() noexcept { return *palette; }

void ChordbloomEditor::changeListenerCallback (juce::ChangeBroadcaster*) {}
void ChordbloomEditor::timerCallback() {}
