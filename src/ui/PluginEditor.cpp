#include "ui/PluginEditor.h"

#include "ui/ChordPaletteView.h"
#include "ui/ControlsPanel.h"
#include "ui/ExportPanel.h"
#include "ui/HeaderBar.h"
#include "ui/KeyboardGuide.h"
#include "ui/ProgressionLane.h"

namespace
{
constexpr int kHeaderHeight = 56;
constexpr int kMargin = 12;     // editor edge -> panels
constexpr int kGap = 12;        // between panels

// Open Chordbloom editors in this plug-in binary (message thread only), so the default look-and-feel can be handed
// to a remaining editor when one closes.
juce::Array<ChordbloomEditor*>& liveEditors()
{
    static juce::Array<ChordbloomEditor*> editors;
    return editors;
}
}

ChordbloomEditor::ChordbloomEditor (ChordbloomProcessor& p)
    : AudioProcessorEditor (p), processor (p)
{
    setLookAndFeel (&lookAndFeel);

    // Popup menus / alert windows created without an explicit look-and-feel (e.g. context menus) use the default
    // one: make that ours while this editor is open (per plug-in binary, so other plug-ins are unaffected).
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
    liveEditors().add (this);

    setOpaque (true);

    header = std::make_unique<HeaderBar> (p);
    palette = std::make_unique<ChordPaletteView> (p);
    lane = std::make_unique<ProgressionLane> (p);
    controls = std::make_unique<ControlsPanel> (p);
    keyboard = std::make_unique<KeyboardGuide> (p);
    exportPanel = std::make_unique<ExportPanel> (p);

    for (juce::Component* c : std::initializer_list<juce::Component*> { header.get(), palette.get(), lane.get(),
                                                                        controls.get(), keyboard.get(), exportPanel.get() })
        addAndMakeVisible (c);

    lane->onSelectionChanged = [this] (int) { pushChords (false); };

    tooltipWindow = std::make_unique<juce::TooltipWindow> (this, 700);
    tooltipWindow->setOpaque (false);   // rounded bubble

    // Size first, constrainer second: setResizeLimits() calls setBoundsConstrained (getBounds()), which would clamp
    // the initial 0x0 bounds to the minimum size and (through resized()) overwrite the remembered size before it is
    // read. resized() only persists sizes once construction is complete.
    const auto saved = processor.getSavedEditorSize();
    const bool hasSaved = saved.x > 0 && saved.y > 0;
    setSize (hasSaved ? juce::jlimit (kMinWidth, kMaxWidth, saved.x) : kDefaultWidth,
             hasSaved ? juce::jlimit (kMinHeight, kMaxHeight, saved.y) : kDefaultHeight);

    setResizable (true, true);
    setResizeLimits (kMinWidth, kMinHeight, kMaxWidth, kMaxHeight);
    persistSize = true;

    processor.addChangeListener (this);
    refreshChildren();
    pushChords (true);
    startTimerHz (30);
}

ChordbloomEditor::~ChordbloomEditor()
{
    stopTimer();
    processor.removeChangeListener (this);
    juce::PopupMenu::dismissAllActiveMenus();

    tooltipWindow.reset();

    liveEditors().removeAllInstancesOf (this);

    if (&juce::LookAndFeel::getDefaultLookAndFeel() == &lookAndFeel)
        juce::LookAndFeel::setDefaultLookAndFeel (liveEditors().isEmpty() ? nullptr
                                                                         : &liveEditors().getLast()->getLookAndFeel());

    setLookAndFeel (nullptr);
}

//==============================================================================
void ChordbloomEditor::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setGradientFill (juce::ColourGradient::vertical (cb::ui::Colours::background.brighter (0.03f), b.getY(),
                                                       cb::ui::Colours::background.darker (0.08f), b.getBottom()));
    g.fillRect (b);

    // Soft contact shadow under every card; the palette, lane and export panels paint their own card surface.
    for (const auto& area : { paletteArea, laneArea, controlsArea, keyboardArea, exportArea })
    {
        if (area.isEmpty())
            continue;

        const auto r = area.toFloat().reduced (0.5f);
        g.setColour (juce::Colours::black.withAlpha (0.16f));
        g.fillRoundedRectangle (r.translated (0.0f, 2.0f).expanded (0.5f), cb::ui::kCornerRadius + 0.5f);
        g.setColour (juce::Colours::black.withAlpha (0.22f));
        g.fillRoundedRectangle (r.translated (0.0f, 1.0f), cb::ui::kCornerRadius);
    }

    for (const auto& area : { controlsArea, keyboardArea })
        if (! area.isEmpty())
            cb::ui::drawPanel (g, area.toFloat());
}

void ChordbloomEditor::resized()
{
    auto r = getLocalBounds();
    header->setBounds (r.removeFromTop (kHeaderHeight));
    r.reduce (kMargin, kMargin);

    // Vertical split: palette gets what the lane and the bottom row leave.
    const int contentH = r.getHeight() - 2 * kGap;
    const int bottomH = juce::jlimit (166, 270, juce::roundToInt ((float) contentH * 0.31f));
    const int laneH = juce::jlimit (112, 196, juce::roundToInt ((float) contentH * 0.23f));

    paletteArea = r.removeFromTop (contentH - bottomH - laneH);
    r.removeFromTop (kGap);
    laneArea = r.removeFromTop (laneH);
    r.removeFromTop (kGap);

    auto bottom = r;
    const int rightW = juce::jlimit (330, 600, juce::roundToInt ((float) bottom.getWidth() * 0.37f));
    auto right = bottom.removeFromRight (rightW);
    bottom.removeFromRight (kGap);
    controlsArea = bottom;

    const int exportH = juce::jlimit (52, 64, juce::roundToInt ((float) right.getHeight() * 0.26f));
    exportArea = right.removeFromBottom (exportH);
    right.removeFromBottom (kGap);
    keyboardArea = right;

    palette->setBounds (paletteArea);
    lane->setBounds (laneArea);
    controls->setBounds (controlsArea);
    keyboard->setBounds (keyboardArea);
    exportPanel->setBounds (exportArea);

    if (persistSize)
        processor.setSavedEditorSize ({ getWidth(), getHeight() });
}

ProgressionLane& ChordbloomEditor::getProgressionLane() noexcept { return *lane; }
ChordPaletteView& ChordbloomEditor::getPaletteView() noexcept { return *palette; }

//==============================================================================
void ChordbloomEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshChildren();
    pushChords (true);
}

void ChordbloomEditor::timerCallback()
{
    pushChords (false);
}

void ChordbloomEditor::updateChordDisplays()
{
    refreshChildren();
    pushChords (true);
}

void ChordbloomEditor::refreshChildren()
{
    header->refresh();
    palette->refresh();
    lane->refresh();
    keyboard->refresh();
    exportPanel->refresh();
}

void ChordbloomEditor::pushChords (bool force)
{
    const auto& slots = processor.getProgression().slots;
    const int numSlots = (int) slots.size();

    auto slotChord = [&slots, numSlots] (int index) -> std::optional<cb::Chord>
    {
        if (index >= 0 && index < numSlots)
            return slots[(size_t) index].chord;

        return std::nullopt;
    };

    // Sounding: the chord being auditioned, else the chord held in chord-trigger mode, else the slot playing back.
    // displaySlot remembers which progression slot (if any) the displayed chord comes from, so the keyboard can show
    // the exact voice-led notes rather than a generic voicing.
    auto sounding = processor.getPreviewChord();
    std::vector<int> displayNotes;
    int displaySlot = -1;

    if (sounding.has_value())
    {
        displayNotes = processor.getPreviewNotes();
    }
    else if (const int degree = processor.getTriggeredDegree(); degree >= 0 && degree < 7)
    {
        const bool sevenths = processor.getAPVTS().getRawParameterValue (cb::ParamIDs::triggerSevenths)->load() >= 0.5f;
        sounding = cb::diatonicChord (processor.getKey(), degree, sevenths);
    }
    else
    {
        displaySlot = processor.getPlayingSlot();
        sounding = slotChord (displaySlot);
    }

    // Display (keyboard guide): sounding, else the selected slot.
    const int selected = lane->getSelectedSlot();
    const auto display = sounding.has_value() ? sounding : slotChord (selected);

    if (! sounding.has_value())
        displaySlot = selected;

    if (display.has_value() && displaySlot >= 0 && displaySlot < numSlots)
    {
        const auto voicings = processor.getProgressionVoicings();

        if (displaySlot < (int) voicings.size())
            displayNotes = voicings[(size_t) displaySlot];
    }

    // Context (suggestions): the selected slot, else the last slot.
    auto context = slotChord (selected);

    if (! context.has_value() && numSlots > 0)
        context = slots.back().chord;

    force = force || ! chordsPushed;
    chordsPushed = true;

    if (force || sounding != lastSounding)
    {
        lastSounding = sounding;
        palette->setSoundingChord (sounding);
    }

    if (force || context != lastContext)
    {
        lastContext = context;
        palette->setContextChord (context);
    }

    if (force || display != lastDisplay || displayNotes != lastDisplayNotes)
    {
        lastDisplay = display;
        lastDisplayNotes = displayNotes;
        keyboard->setDisplay (display, displayNotes);
    }
}
