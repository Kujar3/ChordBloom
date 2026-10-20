#include "ui/ControlsPanel.h"

#include "ui/Theme.h"

namespace
{
namespace Col = cb::ui::Colours;
using APVTS = juce::AudioProcessorValueTreeState;

const juce::String minusSign (juce::CharPointer_UTF8 ("\xe2\x88\x92"));

juce::RangedAudioParameter& param (APVTS& apvts, const char* id)
{
    auto* p = apvts.getParameter (id);
    jassert (p != nullptr);
    return *p;
}

juce::StringArray choiceNames (APVTS& apvts, const char* id)
{
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (id)))
        return choice->choices;

    return {};
}

void paintInset (juce::Graphics& g, juce::Rectangle<float> b, float radius)
{
    g.setColour (Col::background.brighter (0.035f));
    g.fillRoundedRectangle (b, radius);
    g.setColour (Col::outline.withAlpha (0.9f));
    g.drawRoundedRectangle (b.reduced (0.5f), radius, 1.0f);
}

//==============================================================================
// Segmented selector bound to a choice parameter.
class Segmented final : public juce::Component, public juce::SettableTooltipClient
{
public:
    Segmented (juce::RangedAudioParameter& p, juce::StringArray segmentLabels, const juce::String& accessibleTitle)
        : labels (std::move (segmentLabels)),
          attachment (p, [this] (float v)
                      {
                          selected = juce::roundToInt (v);
                          repaint();

                          if (onValueChange != nullptr)
                              onValueChange();

                          if (auto* handler = getAccessibilityHandler())
                              handler->notifyAccessibilityEvent (juce::AccessibilityEvent::valueChanged);
                      })
    {
        setTitle (accessibleTitle);
        setDescription ("Choose one of: " + labels.joinIntoString (", "));
        attachment.sendInitialUpdate();
    }

    int getSelectedIndex() const noexcept { return selected; }
    std::function<void()> onValueChange;   // after the parameter changed (message thread)

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        struct Value final : public juce::AccessibilityTextValueInterface
        {
            explicit Value (Segmented& s) : owner (s) {}
            bool isReadOnly() const override { return false; }
            juce::String getCurrentValueAsString() const override { return owner.labels[owner.selected]; }

            void setValueAsString (const juce::String& newValue) override
            {
                const int index = owner.labels.indexOf (newValue.trim(), true);

                if (index >= 0)
                    owner.select (index);
            }

            Segmented& owner;
        };

        return std::make_unique<juce::AccessibilityHandler> (
            *this, juce::AccessibilityRole::comboBox,
            juce::AccessibilityActions().addAction (juce::AccessibilityActionType::press,
                                                    [this] { select ((selected + 1) % juce::jmax (1, labels.size())); }),
            juce::AccessibilityHandler::Interfaces { std::make_unique<Value> (*this) });
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        const float radius = juce::jmin (6.0f, b.getHeight() * 0.5f);
        paintInset (g, b, radius);

        const int n = labels.size();

        if (n == 0)
            return;

        const float segW = (b.getWidth() - 4.0f) / (float) n;
        const auto f = cb::ui::font (juce::jmin (11.5f, b.getHeight() * 0.48f), 600);
        g.setFont (f);

        for (int i = 0; i < n; ++i)
        {
            const auto seg = juce::Rectangle<float> (b.getX() + 2.0f + segW * (float) i, b.getY() + 2.0f,
                                                     segW, b.getHeight() - 4.0f);
            const bool isSelected = i == selected;
            const bool isHover = i == hover && isEnabled();

            if (isSelected)
            {
                g.setGradientFill (juce::ColourGradient::vertical (Col::panelHover.brighter (0.06f), seg.getY(),
                                                                   Col::panelHover.darker (0.04f), seg.getBottom()));
                g.fillRoundedRectangle (seg, radius - 1.5f);
                g.setColour (Col::accent.withAlpha (0.35f));
                g.drawRoundedRectangle (seg.reduced (0.5f), radius - 1.5f, 1.0f);
            }
            else if (isHover)
            {
                g.setColour (Col::panelRaised.withAlpha (0.8f));
                g.fillRoundedRectangle (seg, radius - 1.5f);
            }
            else if (i > 0 && i - 1 != selected)
            {
                g.setColour (Col::outline.withAlpha (0.55f));
                g.fillRect (juce::Rectangle<float> (seg.getX() - 0.5f, seg.getY() + seg.getHeight() * 0.28f,
                                                    1.0f, seg.getHeight() * 0.44f));
            }

            g.setColour (isSelected ? Col::accent : (isHover ? Col::text : Col::textDim));
            g.drawFittedText (labels[i], seg.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.75f);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override { setHover (indexAt (e.position.x)); }
    void mouseExit (const juce::MouseEvent&) override { setHover (-1); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())   // right-click never changes the parameter
            return;

        select (indexAt (e.position.x));
    }

private:
    void select (int index)
    {
        if (index >= 0 && index < labels.size() && index != selected)
            attachment.setValueAsCompleteGesture ((float) index);
    }

    int indexAt (float x) const
    {
        const int n = labels.size();

        if (n == 0)
            return -1;

        const float segW = ((float) getWidth() - 4.0f) / (float) n;
        return juce::jlimit (0, n - 1, (int) ((x - 2.0f) / segW));
    }

    void setHover (int index)
    {
        if (hover != index)
        {
            hover = index;
            repaint();
        }
    }

    juce::StringArray labels;
    int selected = 0;
    int hover = -1;
    juce::ParameterAttachment attachment;
};

//==============================================================================
// [ - | value | + ] stepper bound to an int parameter (octave).
class Stepper final : public juce::Component, public juce::SettableTooltipClient
{
public:
    Stepper (juce::RangedAudioParameter& p, const juce::String& accessibleTitle)
        : attachment (p, [this] (float v)
                      {
                          value = juce::roundToInt (v);
                          repaint();

                          if (auto* handler = getAccessibilityHandler())
                              handler->notifyAccessibilityEvent (juce::AccessibilityEvent::valueChanged);
                      })
    {
        const auto& range = p.getNormalisableRange();
        minValue = juce::roundToInt (range.start);
        maxValue = juce::roundToInt (range.end);
        setTitle (accessibleTitle);
        attachment.sendInitialUpdate();
    }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        struct Value final : public juce::AccessibilityRangedNumericValueInterface
        {
            explicit Value (Stepper& s) : owner (s) {}
            bool isReadOnly() const override { return false; }
            double getCurrentValue() const override { return owner.value; }
            void setValue (double newValue) override { owner.step (juce::roundToInt (newValue) - owner.value); }
            AccessibleValueRange getRange() const override { return { { (double) owner.minValue, (double) owner.maxValue }, 1.0 }; }

            Stepper& owner;
        };

        return std::make_unique<juce::AccessibilityHandler> (
            *this, juce::AccessibilityRole::slider, juce::AccessibilityActions(),
            juce::AccessibilityHandler::Interfaces { std::make_unique<Value> (*this) });
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        const float radius = juce::jmin (6.0f, b.getHeight() * 0.5f);
        paintInset (g, b, radius);

        const float zoneW = juce::jmin (b.getHeight(), b.getWidth() / 3.0f);
        const auto minusZone = b.withWidth (zoneW).reduced (2.0f);
        const auto plusZone = b.withTrimmedLeft (b.getWidth() - zoneW).reduced (2.0f);

        paintZone (g, minusZone, -1, value > minValue, radius);
        paintZone (g, plusZone, +1, value < maxValue, radius);

        const juce::String text = value > 0 ? "+" + juce::String (value)
                                            : (value < 0 ? minusSign + juce::String (-value) : juce::String ("0"));
        g.setFont (cb::ui::font (juce::jmin (13.0f, b.getHeight() * 0.52f), 600));
        g.setColour (value != 0 ? Col::accent : Col::text);
        g.drawText (text, b.reduced (zoneW, 0.0f), juce::Justification::centred, false);
    }

    void mouseMove (const juce::MouseEvent& e) override { setHover (zoneAt (e.position.x)); }

    void mouseExit (const juce::MouseEvent&) override
    {
        setHover (0);
        wheelAccum = 0.0f;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())   // right-click never changes the parameter
            return;

        if (const int dir = zoneAt (e.position.x); dir != 0)
            step (dir);
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu() && zoneAt (e.position.x) == 0)
            attachment.setValueAsCompleteGesture (0.0f);
    }

    // Accumulates wheel motion so that touchpads and smooth-scrolling / hi-res wheels (many tiny events per gesture)
    // move one octave per notch-worth of travel instead of one octave per event. A classic notched wheel sends
    // ~0.234 per notch (120 units), just above the threshold, so it still steps once per notch.
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        if (wheel.isInertial)
            return;

        float delta = std::abs (wheel.deltaY) > std::abs (wheel.deltaX) ? wheel.deltaY : wheel.deltaX;

        if (wheel.isReversed)
            delta = -delta;

        if (delta == 0.0f)
            return;

        const auto now = juce::Time::getMillisecondCounter();

        // Direction change or a pause restarts the accumulation (leftovers never carry into the next gesture).
        if ((delta > 0.0f) != (wheelAccum > 0.0f) || now - lastWheelMs > 400)
            wheelAccum = 0.0f;

        lastWheelMs = now;
        wheelAccum += delta;

        if (std::abs (wheelAccum) < kWheelStep)
            return;

        step (wheelAccum > 0.0f ? 1 : -1);
        wheelAccum = 0.0f;
    }

private:
    void paintZone (juce::Graphics& g, juce::Rectangle<float> zone, int dir, bool active, float radius) const
    {
        if (active && hoverZone == dir)
        {
            g.setColour (Col::panelHover);
            g.fillRoundedRectangle (zone, radius - 1.5f);
        }

        const auto c = zone.getCentre();
        const float s = juce::jmin (4.0f, zone.getHeight() * 0.2f);
        juce::Path p;
        p.startNewSubPath (c.x - s, c.y);
        p.lineTo (c.x + s, c.y);

        if (dir > 0)
        {
            p.startNewSubPath (c.x, c.y - s);
            p.lineTo (c.x, c.y + s);
        }

        g.setColour (active ? (hoverZone == dir ? Col::text : Col::textDim) : Col::textFaint.withAlpha (0.6f));
        g.strokePath (p, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    int zoneAt (float x) const
    {
        const float zoneW = juce::jmin ((float) getHeight(), (float) getWidth() / 3.0f);

        if (x < zoneW)
            return -1;

        if (x > (float) getWidth() - zoneW)
            return 1;

        return 0;
    }

    void setHover (int zone)
    {
        if (hoverZone != zone)
        {
            hoverZone = zone;
            repaint();
        }
    }

    void step (int delta)
    {
        const int newValue = juce::jlimit (minValue, maxValue, value + delta);

        if (newValue != value)
            attachment.setValueAsCompleteGesture ((float) newValue);
    }

    static constexpr float kWheelStep = 0.2f;

    int value = 0, minValue = -3, maxValue = 3;
    int hoverZone = 0;
    float wheelAccum = 0.0f;
    juce::uint32 lastWheelMs = 0;
    juce::ParameterAttachment attachment;
};

//==============================================================================
// Compact rotary knob with a caption that turns into the value readout on hover / drag.
class Knob final : public juce::Component, private juce::Slider::Listener
{
public:
    Knob (juce::RangedAudioParameter& p, const juce::String& captionText,
          std::function<juce::String (double)> formatter, const juce::String& tooltip)
        : caption (captionText), format (std::move (formatter))
    {
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
        slider.setMouseDragSensitivity (160);
        slider.setScrollWheelEnabled (true);
        slider.setTitle (captionText);
        slider.setDescription (tooltip);
        setTooltipText (tooltip);
        slider.setWantsKeyboardFocus (false);
        slider.setMouseClickGrabsKeyboardFocus (false);
        addAndMakeVisible (slider);

        attachment = std::make_unique<juce::SliderParameterAttachment> (p, slider);
        slider.addListener (this);
        slider.addMouseListener (this, false);
    }

    ~Knob() override
    {
        slider.removeMouseListener (this);
        slider.removeListener (this);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        captionArea = b.removeFromBottom (15);
        b.removeFromBottom (2);
        const int size = juce::jmax (8, juce::jmin (b.getWidth() - 8, b.getHeight(), 68));
        slider.setBounds (b.withSizeKeepingCentre (size, size));
    }

    void paint (juce::Graphics& g) override
    {
        const bool showValue = hovering || dragging;
        g.setFont (cb::ui::font (11.5f, showValue ? 600 : 400));
        g.setColour (showValue ? Col::accent.brighter (0.2f) : Col::textDim);
        g.drawText (showValue ? format (slider.getValue()) : caption, captionArea, juce::Justification::centred, true);
    }

    void mouseEnter (const juce::MouseEvent&) override { hovering = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hovering = false; repaint(); }

    // `note` (may be empty) is appended to the tooltip, e.g. why the control has no effect right now.
    void setTooltipText (const juce::String& tooltip, const juce::String& note = {})
    {
        auto text = tooltip + " (double-click to reset)";

        if (note.isNotEmpty())
            text << "\n\n" << note;

        slider.setTooltip (text);
    }

private:
    void sliderValueChanged (juce::Slider*) override { repaint (captionArea); }
    void sliderDragStarted (juce::Slider*) override { dragging = true; repaint (captionArea); }
    void sliderDragEnded (juce::Slider*) override { dragging = false; repaint (captionArea); }

    cb::ui::LeftClickOnly<juce::Slider> slider;
    std::unique_ptr<juce::SliderParameterAttachment> attachment;
    juce::String caption;
    std::function<juce::String (double)> format;
    juce::Rectangle<int> captionArea;
    bool hovering = false, dragging = false;
};

juce::String percent (double v) { return juce::String (juce::roundToInt (v * 100.0)) + "%"; }

void addBassModeItems (juce::PopupMenu& menu, const juce::StringArray& names, int tickedId)
{
    // Headings "Classic" (Off..Pulse) and "Patterns (fixed grooves)"; item ids are index + 1 (final-spec 7.11).
    for (int i = 0; i < names.size(); ++i)
    {
        if (i == 0)
            menu.addSectionHeader ("Classic");
        else if (i == (int) cb::BassMode::Offbeat8)
            menu.addSectionHeader ("Patterns (fixed grooves)");

        menu.addItem (i + 1, names[i], true, i + 1 == tickedId);
    }
}

//==============================================================================
// The Bass combo (final-spec 7.11): showPopup builds the menu itself, so a real user pick is known without any flag
// (JUCE runs the attachment's listener before onChange, and a dismissed popup fires no onChange). The callback hands a
// pick to onUserPick; result 0 (dismissed) does nothing. Plain onChange (automation, state loads) never resets.
class BassCombo final : public juce::ComboBox
{
public:
    std::function<void (cb::BassMode)> onUserPick;
    juce::StringArray modeNames;

    void showPopup() override
    {
        if (! isEnabled())
            return;

        juce::PopupMenu menu;
        addBassModeItems (menu, modeNames, getSelectedId());
        menu.setLookAndFeel (&getLookAndFeel());

        juce::Component::SafePointer<BassCombo> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMinimumWidth (getWidth()),
                            [safe] (int result)
                            {
                                if (safe == nullptr)
                                    return;

                                safe->hidePopup();

                                if (result > 0 && result <= cb::numBassModes && safe->onUserPick != nullptr)
                                    safe->onUserPick ((cb::BassMode) (result - 1));
                            });
    }
};

// Small "edited" pencil and "muted" crossed speaker after the Bass caption.
void drawPencilGlyph (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour colour)
{
    const float s = juce::jmin (r.getWidth(), r.getHeight());
    const auto c = r.getCentre();
    juce::Path body;
    body.addRoundedRectangle (-s * 0.24f, -s * 0.17f, s * 0.80f, s * 0.34f, s * 0.07f);
    body.addTriangle (-s * 0.30f, -s * 0.17f, -s * 0.30f, s * 0.17f, -s * 0.62f, 0.0f);
    body.applyTransform (juce::AffineTransform::rotation (-juce::MathConstants<float>::pi * 0.25f).translated (c.x, c.y));
    g.setColour (colour);
    g.fillPath (body);
}

void drawMutedGlyph (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour colour)
{
    const float s = juce::jmin (r.getWidth(), r.getHeight());
    const auto c = r.getCentre();
    juce::Path speaker;
    speaker.startNewSubPath (c.x - s * 0.50f, c.y - s * 0.17f);
    speaker.lineTo (c.x - s * 0.28f, c.y - s * 0.17f);
    speaker.lineTo (c.x - s * 0.02f, c.y - s * 0.44f);
    speaker.lineTo (c.x - s * 0.02f, c.y + s * 0.44f);
    speaker.lineTo (c.x - s * 0.28f, c.y + s * 0.17f);
    speaker.lineTo (c.x - s * 0.50f, c.y + s * 0.17f);
    speaker.closeSubPath();
    g.setColour (colour);
    g.fillPath (speaker);

    juce::Path cross;
    const float x0 = c.x + s * 0.14f, x1 = c.x + s * 0.50f, y0 = c.y - s * 0.18f, y1 = c.y + s * 0.18f;
    cross.startNewSubPath (x0, y0);
    cross.lineTo (x1, y1);
    cross.startNewSubPath (x1, y0);
    cross.lineTo (x0, y1);
    g.strokePath (cross, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}
} // namespace

//==============================================================================
struct ControlsPanel::Impl
{
    Impl (ControlsPanel& o, ChordbloomProcessor& p)
        : owner (o),
          processor (p),
          apvts (p.getAPVTS()),
          rate (param (apvts, cb::ParamIDs::rate), choiceNames (apvts, cb::ParamIDs::rate), "Rate"),
          style (param (apvts, cb::ParamIDs::voicing), choiceNames (apvts, cb::ParamIDs::voicing), "Voicing style"),
          octave (param (apvts, cb::ParamIDs::octave), "Octave"),
          swing (param (apvts, cb::ParamIDs::swing), "Swing", percent, swingTip),
          gate (param (apvts, cb::ParamIDs::gate), "Gate", percent,
                "Gate: how long each note sounds, relative to its step (short = staccato, full = legato)"),
          velocity (param (apvts, cb::ParamIDs::velocity), "Velocity",
                    [] (double v) { return juce::String (juce::jlimit (1, 127, juce::roundToInt (v * 127.0))); },
                    "Velocity of the generated notes (shown as MIDI velocity 1-127)"),
          humanize (param (apvts, cb::ParamIDs::humanize), "Humanize", percent,
                    "Humanize: subtle, repeatable timing and velocity variation so the part feels played")
    {
        namespace id = cb::ParamIDs;

        pattern.addItemList (choiceNames (apvts, id::pattern), 1);
        bass.modeNames = choiceNames (apvts, id::bassMode);

        for (int i = 0; i < bass.modeNames.size(); ++i)
        {
            if (i == 0)
                bass.addSectionHeading ("Classic");
            else if (i == (int) cb::BassMode::Offbeat8)
                bass.addSectionHeading ("Patterns (fixed grooves)");

            bass.addItem (bass.modeNames[i], i + 1);
        }

        bass.onUserPick = [this] (cb::BassMode mode) { owner.pickBassModeAsUser (mode); };
        patternAtt = std::make_unique<APVTS::ComboBoxAttachment> (apvts, id::pattern, pattern);
        bassAtt = std::make_unique<APVTS::ComboBoxAttachment> (apvts, id::bassMode, bass);

        voiceLeadingAtt = std::make_unique<APVTS::ButtonAttachment> (apvts, id::voiceLeading, voiceLeading);
        scaleLockAtt = std::make_unique<APVTS::ButtonAttachment> (apvts, id::scaleLock, scaleLock);
        chordTriggerAtt = std::make_unique<APVTS::ButtonAttachment> (apvts, id::chordTrigger, chordTrigger);

        sevenths.setClickingTogglesState (true);
        sevenths.getProperties().set (cb::ui::kPropChip, true);
        seventhsAtt = std::make_unique<APVTS::ButtonAttachment> (apvts, id::triggerSevenths, sevenths);

        chordTrigger.onStateChange = [this] { updateSeventhsLook(); };
        updateSeventhsLook();

        pattern.setTooltip ("Rhythm pattern for playback and MIDI export: held chords, strums, pulses, arpeggios or broken chords");
        style.setTooltip ("Voicing: Close keeps the notes compact, Open spreads them (drop-2), Spread gives wide pad-like chords");
        updateBassLook();
        octave.setTooltip ("Octave of the chords (the bass stays in its own low range, below the chords). "
                           "Scroll or click - / +, double-click to reset");
        voiceLeading.setTooltip ("Voice leading: picks inversions automatically so each chord moves smoothly to the next");
        scaleLock.setTooltip ("Scale lock: notes you play on your MIDI keyboard snap to the nearest note of the key");
        chordTrigger.setTooltip ("Chord trigger: white keys C to B on your MIDI keyboard play the key's chords I to VII "
                                 "(black keys pass through)");
        sevenths.setTooltip ("Chord trigger plays seventh chords instead of triads");

        voiceLeading.setTitle ("Voice leading");
        scaleLock.setTitle ("Scale lock");
        chordTrigger.setTitle ("Chord trigger");
        pattern.setTitle ("Rhythm pattern");
        bass.setTitle ("Bass line");
        sevenths.setTitle ("Chord trigger sevenths");

        // Rate and swing only matter for some patterns / bass lines: dim them (with a tooltip saying why) otherwise.
        pattern.onChange = [this] { updateRateSwingLook(); };
        bass.onChange = [this] { updateRateSwingLook(); };
        rate.onValueChange = [this] { updateRateSwingLook(); };
        updateRateSwingLook();

        for (juce::Component* c : std::initializer_list<juce::Component*> {
                 &pattern, &rate, &swing, &gate, &style, &bass, &octave, &voiceLeading,
                 &velocity, &humanize, &scaleLock, &chordTrigger, &sevenths })
        {
            c->setWantsKeyboardFocus (false);
            c->setMouseClickGrabsKeyboardFocus (false);
            owner.addAndMakeVisible (c);
        }
    }

    void updateSeventhsLook()
    {
        sevenths.setAlpha (chordTrigger.getToggleState() ? 1.0f : 0.5f);
    }

    // I12: with the Block / Strum patterns and a held bass (Off, Root, Root + 5th) nothing runs on the rate grid, so Rate
    // and Swing have no audible effect; 1/8T is already a shuffle and is never swung. Offbeat sits on the "&" of each
    // beat whatever the rate, and only swings on the 1/8 grid.
    void updateRateSwingLook()
    {
        using P = cb::RhythmPattern;
        using B = cb::BassMode;

        const auto pat = (P) juce::jlimit (0, cb::numRhythmPatterns - 1, pattern.getSelectedItemIndex());
        const auto bassMode = (B) juce::jlimit (0, cb::numBassModes - 1, bass.getSelectedItemIndex());
        const auto rateDiv = (cb::RateDivision) juce::jlimit (0, cb::numRateDivisions - 1, rate.getSelectedIndex());

        const bool patternOnGrid = ! (pat == P::Block || pat == P::Strum || pat == P::Offbeat);
        const bool bassOnGrid = bassMode == B::Octaves || bassMode == B::Pulse;
        const bool triplet = rateDiv == cb::RateDivision::EighthTriplet;

        // Phase 2 (final-spec 7.11): the chord Rate is the groove grid the arp and the pattern basses swing on.
        const auto* arpParam = apvts.getRawParameterValue (cb::ParamIDs::arpOn);
        const bool arpOn = arpParam != nullptr && arpParam->load() >= 0.5f;
        const bool groove = arpOn || cb::isBassPattern (bassMode);

        const bool rateActive = patternOnGrid || bassOnGrid || groove;
        const bool swingActive = ((patternOnGrid || bassOnGrid) && ! triplet)
                              || (pat == P::Offbeat && rateDiv == cb::RateDivision::Eighth)
                              || (groove && ! triplet);

        juce::String rateNote, swingNote;

        if (! rateActive)
        {
            rateNote = pat == P::Offbeat
                         ? "No effect with Offbeat (it always plays on the \"&\"), except that swing needs 1/8."
                         : "No effect right now: " + juce::String (cb::rhythmPatternName (pat))
                               + " holds each chord and the bass is held too. Try Pulse, an Arp or Broken, "
                                 "or the Octaves / Pulse bass.";
        }

        if (! swingActive)
        {
            if (triplet && (patternOnGrid || bassOnGrid || groove))
                swingNote = "No effect at 1/8T: triplets are already a shuffle.";
            else if (pat == P::Offbeat && ! bassOnGrid)
                swingNote = "With Offbeat, swing only works at rate 1/8.";
            else
                swingNote = "No effect right now: " + juce::String (cb::rhythmPatternName (pat))
                              + " holds each chord and the bass is held too. Try Pulse, an Arp or Broken, "
                                "or the Octaves / Pulse bass.";
        }

        rate.setTooltip (juce::String (rateTip) + "\n" + grooveTip + (rateNote.isNotEmpty() ? "\n\n" + rateNote : juce::String()));
        swing.setTooltipText (swingTip, swingNote);

        rate.setAlpha (rateActive ? 1.0f : kInactiveAlpha);
        swing.setAlpha (swingActive ? 1.0f : kInactiveAlpha);
        rateInactive = ! rateActive;
        owner.repaint();   // the "Rate" caption dims with the control
    }

    // Caption glyphs and the combo's tooltip words (final-spec 7.11).
    void updateBassLook()
    {
        const auto stateText = bassStateText();
        bassEdited = stateText.contains ("edited");
        bassMuted = stateText.contains ("muted");

        juce::String tip ("Bass line, sent on MIDI channel 2. The patterns are fixed grooves (they ignore Rate).");

        if (bassEdited || bassMuted)
        {
            tip << "\n\nBass: " << stateText << ".";

            if (bassEdited)
                tip << " Your notes in the Roll play instead of the pattern; picking a mode here resets them (Undo brings "
                       "them back).";
            if (bassMuted)
                tip << " Muted: turn it on with the dot on the Roll's Bass tab.";
        }

        if (bass.getTooltip() != tip)
            bass.setTooltip (tip);

        if (bassEdited != shownEdited || bassMuted != shownMuted)
        {
            shownEdited = bassEdited;
            shownMuted = bassMuted;
            owner.repaint (bassCaption);
        }
    }

    juce::String bassStateText() const
    {
        const bool edited = processor.isLayerCustom (cb::Layer::Bass);
        const auto* bassOn = apvts.getRawParameterValue (cb::ParamIDs::bassOn);
        const bool muted = bassOn != nullptr && bassOn->load() < 0.5f;

        if (edited && muted)
            return "edited, muted";
        if (edited)
            return "edited";
        return muted ? "muted" : "";
    }

    bool bassEdited = false, bassMuted = false, shownEdited = false, shownMuted = false;
    juce::Rectangle<int> bassCaption;

    static constexpr float kInactiveAlpha = 0.45f;
    static constexpr const char* grooveTip = "Rate also sets the swing grid of the arp and the bass patterns.";
    static constexpr const char* rateTip = "Step length for pulses, arpeggios and moving bass lines (1/8T = eighth-note triplets)";
    static constexpr const char* swingTip = "Swing: delays every second step for a shuffled groove";
    bool rateInactive = false;

    struct Caption
    {
        juce::Rectangle<int> area;
        juce::String text;
    };

    ControlsPanel& owner;
    ChordbloomProcessor& processor;
    APVTS& apvts;

    juce::ComboBox pattern;
    BassCombo bass;
    Segmented rate, style;
    Stepper octave;
    Knob swing, gate, velocity, humanize;
    cb::ui::LeftClickOnly<juce::ToggleButton> voiceLeading { "Voice leading" }, scaleLock { "Scale lock" },
                                              chordTrigger { "Chord trigger" };
    cb::ui::LeftClickOnly<juce::TextButton> sevenths { "7ths" };

    std::unique_ptr<APVTS::ComboBoxAttachment> patternAtt, bassAtt;
    std::unique_ptr<APVTS::ButtonAttachment> voiceLeadingAtt, scaleLockAtt, chordTriggerAtt, seventhsAtt;

    std::vector<Caption> captions;
    std::array<juce::Rectangle<int>, 3> titleAreas;
    std::array<int, 2> dividerX {};
    int dividerTop = 0, dividerBottom = 0;
};

//==============================================================================
ControlsPanel::ControlsPanel (ChordbloomProcessor& p) : impl (std::make_unique<Impl> (*this, p)) {}
ControlsPanel::~ControlsPanel() = default;

void ControlsPanel::paint (juce::Graphics& g)
{
    auto& d = *impl;
    static const char* titles[] = { "Rhythm", "Voicing", "Feel" };

    for (size_t i = 0; i < d.titleAreas.size(); ++i)
        cb::ui::drawSectionTitle (g, titles[i], d.titleAreas[i]);

    g.setFont (cb::ui::font (12.0f));

    for (const auto& c : d.captions)
    {
        const bool dim = d.rateInactive && c.text == "Rate";
        g.setColour (dim ? Col::textDim.withMultipliedAlpha (Impl::kInactiveAlpha + 0.1f) : Col::textDim);
        g.drawText (c.text, c.area, juce::Justification::centredLeft, true);

        // Bass: a pencil while the bass layer is custom, a crossed speaker while bassOn is off.
        if (c.text == "Bass" && (d.bassEdited || d.bassMuted))
        {
            // 7 px glyphs on an 8 px pitch: "Bass" + both glyphs fit the narrowest caption column (42 px).
            const float glyphsW = (d.bassEdited ? 8.0f : 0.0f) + (d.bassMuted ? 8.0f : 0.0f);
            float x = juce::jmin ((float) c.area.getX() + cb::ui::textWidth (cb::ui::font (12.0f), c.text) + 2.5f,
                                  (float) c.area.getRight() - glyphsW - 1.0f);
            const float cy = (float) c.area.getCentreY();

            if (d.bassEdited)
            {
                drawPencilGlyph (g, { x, cy - 3.5f, 7.0f, 7.0f }, Col::textDim);
                x += 8.0f;
            }

            if (d.bassMuted)
                drawMutedGlyph (g, { x, cy - 3.5f, 7.0f, 7.0f }, Col::subdominant.withAlpha (0.85f));
        }
    }

    for (int x : d.dividerX)
    {
        auto grad = juce::ColourGradient::vertical (Col::outline.withAlpha (0.0f), (float) d.dividerTop,
                                                    Col::outline.withAlpha (0.0f), (float) d.dividerBottom);
        grad.addColour (0.2, Col::outline.withAlpha (0.85f));
        grad.addColour (0.8, Col::outline.withAlpha (0.85f));
        g.setGradientFill (grad);
        g.fillRect (juce::Rectangle<float> ((float) x, (float) d.dividerTop, 1.0f, (float) (d.dividerBottom - d.dividerTop)));
    }
}

void ControlsPanel::resized()
{
    auto& d = *impl;
    d.captions.clear();

    auto area = getLocalBounds().reduced (16, 12);
    const int gutter = juce::jlimit (20, 32, area.getWidth() / 24);
    const int groupW = (area.getWidth() - 2 * gutter) / 3;

    d.dividerTop = 12;
    d.dividerBottom = getHeight() - 12;

    std::array<juce::Rectangle<int>, 3> groups;

    for (size_t i = 0; i < groups.size(); ++i)
    {
        groups[i] = area.removeFromLeft (groupW);

        if (i < 2)
        {
            d.dividerX[i] = area.getX() + gutter / 2;
            area.removeFromLeft (gutter);
        }
    }

    for (size_t i = 0; i < groups.size(); ++i)
    {
        d.titleAreas[i] = groups[i].removeFromTop (14);
        groups[i].removeFromTop (10);
    }

    // Three aligned rows across the groups: A (combo / selector), B (combo / selector), C (knobs).
    // Spacing grows smoothly from the minimum editor height (H ~ 118) to the maximum (H ~ 224).
    const int H = groups[0].getHeight();
    const float grow = juce::jlimit (0.0f, 1.0f, (float) (H - 118) / 106.0f);
    const int rowH = juce::jlimit (24, 32, juce::roundToInt ((float) H * 0.21f));
    const int rowGap = juce::roundToInt (6.0f + 6.0f * grow);
    const int knobGap = juce::roundToInt (8.0f + 10.0f * grow);
    const int knobH = juce::jlimit (36, 92, H - 2 * rowH - rowGap - knobGap);
    const int usedH = 2 * rowH + rowGap + knobGap + knobH;
    const int offset = juce::jmax (0, (H - usedH) / 3);

    const int capW = juce::jlimit (42, 58, juce::roundToInt ((float) groupW * 0.27f));

    auto withCaption = [&d, capW] (juce::Rectangle<int> row, const juce::String& text)
    {
        d.captions.push_back ({ row.removeFromLeft (capW), text });
        return row;
    };

    struct Rows { juce::Rectangle<int> a, b, c; };

    auto rowsFor = [&] (juce::Rectangle<int> group)
    {
        auto content = group.withTrimmedTop (offset);
        Rows rows;
        rows.a = content.removeFromTop (rowH);
        content.removeFromTop (rowGap);
        rows.b = content.removeFromTop (rowH);
        content.removeFromTop (knobGap);
        rows.c = content.removeFromTop (knobH);
        return rows;
    };

    // Rhythm
    {
        auto rows = rowsFor (groups[0]);
        d.pattern.setBounds (withCaption (rows.a, "Pattern"));
        d.rate.setBounds (withCaption (rows.b, "Rate"));
        auto c = rows.c;
        d.swing.setBounds (c.removeFromLeft (c.getWidth() / 2));
        d.gate.setBounds (c);
    }

    // Voicing
    {
        auto rows = rowsFor (groups[1]);
        d.style.setBounds (withCaption (rows.a, "Style"));
        d.bass.setBounds (withCaption (rows.b, "Bass"));
        d.bassCaption = d.captions.back().area;

        auto c = rows.c;
        const int subH = juce::jmin (rowH, (c.getHeight() - 4) / 2);
        const int subGap = juce::jlimit (2, rowGap, c.getHeight() - 2 * subH);
        auto octaveRow = withCaption (c.removeFromTop (subH), "Octave");
        d.octave.setBounds (octaveRow.withWidth (juce::jmin (octaveRow.getWidth(), 96)));
        c.removeFromTop (subGap);
        auto vlRow = c.removeFromTop (subH);
        d.voiceLeading.setBounds (vlRow.withHeight (juce::jmin (24, vlRow.getHeight())).withY (vlRow.getY() + juce::jmax (0, (vlRow.getHeight() - 24) / 2)));
        d.voiceLeading.changeWidthToFitText();
    }

    // Feel
    {
        auto rows = rowsFor (groups[2]);
        auto toggleRow = [] (juce::Rectangle<int> row) { return row.withSizeKeepingCentre (row.getWidth(), juce::jmin (24, row.getHeight())); };

        d.scaleLock.setBounds (toggleRow (rows.a));
        d.scaleLock.changeWidthToFitText();

        // "Chord trigger" toggle with its "7ths" chip right after the label
        auto b = toggleRow (rows.b);
        const int chipW = 40;
        d.chordTrigger.setBounds (b);
        d.chordTrigger.changeWidthToFitText();
        const int toggleW = juce::jmin (d.chordTrigger.getWidth(), b.getWidth() - chipW - 8);
        d.chordTrigger.setBounds (b.removeFromLeft (toggleW));
        b.removeFromLeft (8);
        d.sevenths.setBounds (b.removeFromLeft (chipW).withSizeKeepingCentre (chipW, 22));

        auto c = rows.c;
        d.velocity.setBounds (c.removeFromLeft (c.getWidth() / 2));
        d.humanize.setBounds (c);
    }
}

//==============================================================================
// Phase 2 (final-spec 7.11): the caption glyphs and the Rate / Swing rule follow parameter-only changes too (the editor
// calls this on change messages and whenever the layer serial moves).
void ControlsPanel::refresh()
{
    impl->updateBassLook();
    impl->updateRateSwingLook();
}

// final-spec 7.11 order: the parameter first (host gesture), then beginUserLayerPick for a mode other than Off.
void ControlsPanel::pickBassModeAsUser (cb::BassMode mode)
{
    const int index = juce::jlimit (0, cb::numBassModes - 1, (int) mode);

    if (auto* p = impl->apvts.getParameter (cb::ParamIDs::bassMode))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 ((float) index));
        p->endChangeGesture();
    }

    if (index != (int) cb::BassMode::Off)
        impl->processor.beginUserLayerPick (cb::Layer::Bass);
}

juce::String ControlsPanel::getBassStateText() const { return impl->bassStateText(); }
