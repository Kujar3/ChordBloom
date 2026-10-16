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
    Segmented (juce::RangedAudioParameter& p, juce::StringArray segmentLabels)
        : labels (std::move (segmentLabels)),
          attachment (p, [this] (float v) { selected = juce::roundToInt (v); repaint(); })
    {
        attachment.sendInitialUpdate();
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
        const int index = indexAt (e.position.x);

        if (index >= 0 && index != selected)
            attachment.setValueAsCompleteGesture ((float) index);
    }

private:
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
    explicit Stepper (juce::RangedAudioParameter& p)
        : attachment (p, [this] (float v) { value = juce::roundToInt (v); repaint(); })
    {
        const auto& range = p.getNormalisableRange();
        minValue = juce::roundToInt (range.start);
        maxValue = juce::roundToInt (range.end);
        attachment.sendInitialUpdate();
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
    void mouseExit (const juce::MouseEvent&) override { setHover (0); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (const int dir = zoneAt (e.position.x); dir != 0)
            step (dir);
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (zoneAt (e.position.x) == 0)
            attachment.setValueAsCompleteGesture (0.0f);
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        const float delta = std::abs (wheel.deltaY) > std::abs (wheel.deltaX) ? wheel.deltaY : wheel.deltaX;

        if (delta != 0.0f)
            step (delta > 0.0f ? 1 : -1);
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

    int value = 0, minValue = -3, maxValue = 3;
    int hoverZone = 0;
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
        slider.setTooltip (tooltip + " (double-click to reset)");
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

private:
    void sliderValueChanged (juce::Slider*) override { repaint (captionArea); }
    void sliderDragStarted (juce::Slider*) override { dragging = true; repaint (captionArea); }
    void sliderDragEnded (juce::Slider*) override { dragging = false; repaint (captionArea); }

    juce::Slider slider;
    std::unique_ptr<juce::SliderParameterAttachment> attachment;
    juce::String caption;
    std::function<juce::String (double)> format;
    juce::Rectangle<int> captionArea;
    bool hovering = false, dragging = false;
};

juce::String percent (double v) { return juce::String (juce::roundToInt (v * 100.0)) + "%"; }
} // namespace

//==============================================================================
struct ControlsPanel::Impl
{
    Impl (ControlsPanel& o, ChordbloomProcessor& p)
        : owner (o),
          apvts (p.getAPVTS()),
          rate (param (apvts, cb::ParamIDs::rate), choiceNames (apvts, cb::ParamIDs::rate)),
          style (param (apvts, cb::ParamIDs::voicing), choiceNames (apvts, cb::ParamIDs::voicing)),
          octave (param (apvts, cb::ParamIDs::octave)),
          swing (param (apvts, cb::ParamIDs::swing), "Swing", percent,
                 "Swing: delays every second step for a shuffled groove"),
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
        bass.addItemList (choiceNames (apvts, id::bassMode), 1);
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
        rate.setTooltip ("Step length for pulses, arpeggios and moving bass lines (1/8T = eighth-note triplets)");
        style.setTooltip ("Voicing: Close keeps the notes compact, Open spreads them (drop-2), Spread gives wide pad-like chords");
        bass.setTooltip ("Bass line, sent on MIDI channel 2");
        octave.setTooltip ("Octave of the chords (the bass follows). Scroll or click - / +, double-click to reset");
        voiceLeading.setTooltip ("Voice leading: picks inversions automatically so each chord moves smoothly to the next");
        scaleLock.setTooltip ("Scale lock: notes you play on your MIDI keyboard snap to the nearest note of the key");
        chordTrigger.setTooltip ("Chord trigger: white keys C to B on your MIDI keyboard play the key's chords I to VII");
        sevenths.setTooltip ("Chord trigger plays seventh chords instead of triads");

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

    struct Caption
    {
        juce::Rectangle<int> area;
        juce::String text;
    };

    ControlsPanel& owner;
    APVTS& apvts;

    juce::ComboBox pattern, bass;
    Segmented rate, style;
    Stepper octave;
    Knob swing, gate, velocity, humanize;
    juce::ToggleButton voiceLeading { "Voice leading" }, scaleLock { "Scale lock" }, chordTrigger { "Chord trigger" };
    juce::TextButton sevenths { "7ths" };

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
    g.setColour (Col::textDim);

    for (const auto& c : d.captions)
        g.drawText (c.text, c.area, juce::Justification::centredLeft, true);

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
