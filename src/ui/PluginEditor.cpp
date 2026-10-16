#include "ui/PluginEditor.h"

#include "ui/ChordPaletteView.h"
#include "ui/ControlsPanel.h"
#include "ui/DragPayload.h"
#include "ui/ExportPanel.h"
#include "ui/HeaderBar.h"
#include "ui/KeyboardGuide.h"
#include "ui/ProgressionLane.h"

namespace
{
constexpr int kHeaderHeight = 56;
constexpr int kMargin = 12;     // editor edge -> panels
constexpr int kGap = 12;        // between panels

// Room left around the editor for the host's plug-in window frame (FL Studio's wrapper caption + toolbar) and the
// window edges, when the editor size is fitted to a screen.
constexpr int kScreenMarginW = 40;
constexpr int kScreenMarginH = 90;

//==============================================================================
// B26: the exact notes a chord-trigger key is playing. Uses ChordbloomProcessor::getTriggeredNotes() when the processor
// provides one (detected at compile time); otherwise the sounding notes that belong to the triggered chord - in
// chord-trigger mode those are the voice-led, octave-shifted notes actually sent.
template <typename P>
auto exactTriggeredNotes (const P& p, int) -> decltype (std::vector<int> (p.getTriggeredNotes()), std::optional<std::vector<int>>())
{
    return std::vector<int> (p.getTriggeredNotes());
}

template <typename P>
std::optional<std::vector<int>> exactTriggeredNotes (const P&, long)
{
    return std::nullopt;
}

std::vector<int> triggeredNotes (const ChordbloomProcessor& p, const cb::Chord& chord)
{
    if (auto exact = exactTriggeredNotes (p, 0))
        return *exact;

    std::array<bool, 12> inChord {};

    for (int pitchClass : chord.pitchClasses())
        inChord[(size_t) cb::pc (pitchClass)] = true;

    const auto sounding = p.getSoundingNotes();
    std::vector<int> notes;

    for (int n = 0; n < 128; ++n)
        if (sounding[(size_t) n] && inChord[(size_t) (n % 12)])
            notes.push_back (n);

    return notes;
}

juce::Rectangle<int> primaryUserArea()
{
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        return display->userBounds.getLargestIntegerWithin();

    return {};
}

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
    // Both the default and a remembered size are fitted to the screen (I18: 1100x690 does not fit a 1080p screen at
    // 150% scaling). The primary display is used here; fitToCurrentDisplay() re-checks once the window is on screen.
    const auto saved = processor.getSavedEditorSize();
    const bool hasSaved = saved.x > 0 && saved.y > 0;
    const auto initial = clampSizeToScreen (hasSaved ? saved : juce::Point<int> (kDefaultWidth, kDefaultHeight),
                                            primaryUserArea());
    setSize (initial.x, initial.y);

    setResizable (true, true);
    setResizeLimits (kMinWidth, kMinHeight, kMaxWidth, kMaxHeight);
    persistSize = true;

    // B10: nothing in the editor takes keyboard focus, so FL Studio keeps its spacebar and shortcuts. Clicks on the
    // editor background do not start a focus search either. Re-applied on refreshes for components created later.
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    cb::ui::neverTakeKeyboardFocus (*this);

    setTitle ("Chordbloom");

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
    // Below 600 px height (small screens / high scaling) the margins and minimum panel heights tighten a little.
    const bool compact = getHeight() < 600;
    const int margin = compact ? 10 : kMargin;
    const int gap = compact ? 10 : kGap;

    auto r = getLocalBounds();
    header->setBounds (r.removeFromTop (kHeaderHeight));
    r.reduce (margin, margin);

    // Vertical split: palette gets what the lane and the bottom row leave.
    const int contentH = r.getHeight() - 2 * gap;
    const int bottomH = juce::jlimit (compact ? 156 : 166, 270, juce::roundToInt ((float) contentH * 0.31f));
    const int laneH = juce::jlimit (compact ? 106 : 112, 196, juce::roundToInt ((float) contentH * 0.23f));

    paletteArea = r.removeFromTop (contentH - bottomH - laneH);
    r.removeFromTop (gap);
    laneArea = r.removeFromTop (laneH);
    r.removeFromTop (gap);

    auto bottom = r;
    const int rightW = juce::jlimit (330, 600, juce::roundToInt ((float) bottom.getWidth() * 0.37f));
    auto right = bottom.removeFromRight (rightW);
    bottom.removeFromRight (gap);
    controlsArea = bottom;

    const int exportH = juce::jlimit (52, 64, juce::roundToInt ((float) right.getHeight() * 0.26f));
    exportArea = right.removeFromBottom (exportH);
    right.removeFromBottom (gap);
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
    cb::ui::neverTakeKeyboardFocus (*this);   // children may have created new widgets
}

void ChordbloomEditor::timerCallback()
{
    pushChords (false);

    if (externalDropPending && ! juce::ModifierKeys::getCurrentModifiersRealtime().isAnyMouseButtonDown())
    {
        externalDropPending = false;
        exportPanel->showPlaysAlongHint();
    }

    // Once a second: make sure no widget created since (inline buttons, pills) can take keyboard focus.
    if (--focusSweepCountdown <= 0)
    {
        focusSweepCountdown = 30;
        cb::ui::neverTakeKeyboardFocus (*this);
    }
}

//==============================================================================
juce::Point<int> ChordbloomEditor::clampSizeToScreen (juce::Point<int> requested, juce::Rectangle<int> screenUserArea)
{
    int w = requested.x, h = requested.y;

    if (! screenUserArea.isEmpty())
    {
        w = juce::jmin (w, screenUserArea.getWidth() - kScreenMarginW);
        h = juce::jmin (h, screenUserArea.getHeight() - kScreenMarginH);
    }

    // The resizable minimum wins on very small screens (the host can still move the window).
    return { juce::jlimit (kMinWidth, kMaxWidth, w), juce::jlimit (kMinHeight, kMaxHeight, h) };
}

void ChordbloomEditor::parentHierarchyChanged()
{
    AudioProcessorEditor::parentHierarchyChanged();

    // The first time the editor is on screen, check that it fits the display it actually opened on (which may not be
    // the primary one). Done asynchronously, outside the host's window-attach call.
    if (! fittedToDisplay && getPeer() != nullptr)
    {
        fittedToDisplay = true;
        juce::Component::SafePointer<ChordbloomEditor> safeThis (this);
        juce::MessageManager::callAsync ([safeThis]
        {
            if (safeThis != nullptr)
                safeThis->fitToCurrentDisplay();
        });
    }
}

void ChordbloomEditor::fitToCurrentDisplay()
{
    if (getPeer() == nullptr || ! isShowing())
        return;

    const auto screenBounds = getScreenBounds();
    const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (screenBounds);

    if (display == nullptr || screenBounds.isEmpty() || getWidth() <= 0 || getHeight() <= 0)
        return;

    // The host may scale the editor (transform): measure the screen area in editor units.
    const auto userArea = display->userBounds;
    const double sx = (double) screenBounds.getWidth() / (double) getWidth();
    const double sy = (double) screenBounds.getHeight() / (double) getHeight();
    const juce::Rectangle<int> areaInEditorUnits (0, 0, (int) (userArea.getWidth() / juce::jmax (0.1, sx)),
                                                  (int) (userArea.getHeight() / juce::jmax (0.1, sy)));

    const auto fitted = clampSizeToScreen ({ getWidth(), getHeight() }, areaInEditorUnits);

    if (fitted.x < getWidth() || fitted.y < getHeight())
        setSize (juce::jmin (fitted.x, getWidth()), juce::jmin (fitted.y, getHeight()));
}

//==============================================================================
void ChordbloomEditor::setHelpVisible (bool shouldBeVisible)
{
    header->setHelpVisible (shouldBeVisible);
}

bool ChordbloomEditor::isHelpVisible() const
{
    return header->isHelpVisible();
}

bool ChordbloomEditor::shouldDropFilesWhenDraggedExternally (const juce::DragAndDropTarget::SourceDetails& details,
                                                            juce::StringArray& files, bool& canMoveFiles)
{
    namespace drag = cb::ui::drag;

    juce::File file;

    if (drag::isChordPayload (details.description))
    {
        file = processor.writeChordMidiToTempFile (drag::chordFromPayload (details.description));
    }
    else if (drag::isSlotPayload (details.description))
    {
        const int slot = drag::slotFromPayload (details.description);

        if (slot >= 0 && slot < (int) processor.getProgression().slots.size())
            file = processor.writeSlotMidiToTempFile (slot);
    }

    if (! file.existsAsFile())
        return false;

    // The OS drag loop swallows the mouse-up that would end a pad / block preview held since the press.
    processor.stopPreview();

    // I2: once the file has been dropped (mouse released), remind the user that Chordbloom still plays along with
    // FL (the export panel decides whether that applies and throttles it). Checked by the 30 Hz timer.
    externalDropPending = true;

    files.add (file.getFullPathName());
    canMoveFiles = false;
    return true;
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
        displayNotes = triggeredNotes (processor, *sounding);   // B26: the voice-led, octave-shifted notes played
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
