#include "ui/ExportPanel.h"

#include "ui/Theme.h"

#include <cmath>

namespace
{
namespace Col = cb::ui::Colours;
using Part = ChordbloomProcessor::ExportPart;

float widthOf (const juce::Font& f, const juce::String& s) { return juce::GlyphArrangement::getStringWidth (f, s); }

juce::String utf8 (const char* s) { return juce::String (juce::CharPointer_UTF8 (s)); }

juce::File& lastSaveDirectory()   // message thread only; remembered while the plugin stays loaded
{
    static juce::File dir;
    return dir;
}

void noFocus (juce::Component& c)
{
    c.setWantsKeyboardFocus (false);
    c.setMouseClickGrabsKeyboardFocus (false);
}

void paintCard (juce::Graphics& g, juce::Rectangle<float> r)
{
    r = r.reduced (0.5f);
    g.setGradientFill (juce::ColourGradient::vertical (Col::panel.brighter (0.035f), r.getY(), Col::panel, r.getBottom()));
    g.fillRoundedRectangle (r, cb::ui::kCornerRadius);
    g.setColour (Col::outline.withAlpha (0.55f));
    g.drawRoundedRectangle (r, cb::ui::kCornerRadius, 1.0f);
}

// Six-dot grip: the universal "you can drag this" sign. 8 px wide.
void drawGrip (juce::Graphics& g, juce::Point<float> centre, juce::Colour colour)
{
    g.setColour (colour);

    for (int col = 0; col < 2; ++col)
        for (int row = 0; row < 3; ++row)
            g.fillEllipse (centre.x - 3.0f + (float) col * 4.5f - 1.1f, centre.y - 4.5f + (float) row * 4.5f - 1.1f, 2.2f, 2.2f);
}

// Arrow into a tray (download / save icon).
void drawSaveIcon (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour colour)
{
    g.setColour (colour);
    const float cx = r.getCentreX();
    juce::Path arrow;
    arrow.startNewSubPath (cx, r.getY());
    arrow.lineTo (cx, r.getBottom() - 3.5f);
    arrow.startNewSubPath (cx - 3.0f, r.getBottom() - 6.5f);
    arrow.lineTo (cx, r.getBottom() - 3.5f);
    arrow.lineTo (cx + 3.0f, r.getBottom() - 6.5f);
    arrow.startNewSubPath (r.getX(), r.getBottom() - 2.5f);
    arrow.lineTo (r.getX(), r.getBottom());
    arrow.lineTo (r.getRight(), r.getBottom());
    arrow.lineTo (r.getRight(), r.getBottom() - 2.5f);
    g.strokePath (arrow, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void drawChevron (juce::Graphics& g, juce::Point<float> c, juce::Colour colour)
{
    juce::Path p;
    p.startNewSubPath (c.x - 3.0f, c.y - 1.4f);
    p.lineTo (c.x, c.y + 1.6f);
    p.lineTo (c.x + 3.0f, c.y - 1.4f);
    g.setColour (colour);
    g.strokePath (p, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

// Speaker with a cross (mute).
void drawMuteIcon (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour colour)
{
    const float s = std::min (r.getWidth(), r.getHeight());
    const auto c = r.getCentre();
    juce::Path speaker;
    speaker.startNewSubPath (c.x - s * 0.48f, c.y - s * 0.16f);
    speaker.lineTo (c.x - s * 0.28f, c.y - s * 0.16f);
    speaker.lineTo (c.x - s * 0.04f, c.y - s * 0.40f);
    speaker.lineTo (c.x - s * 0.04f, c.y + s * 0.40f);
    speaker.lineTo (c.x - s * 0.28f, c.y + s * 0.16f);
    speaker.lineTo (c.x - s * 0.48f, c.y + s * 0.16f);
    speaker.closeSubPath();
    g.setColour (colour);
    g.fillPath (speaker);

    juce::Path cross;
    const float x0 = c.x + s * 0.14f, x1 = c.x + s * 0.46f, y0 = c.y - s * 0.16f, y1 = c.y + s * 0.16f;
    cross.startNewSubPath (x0, y0);
    cross.lineTo (x1, y1);
    cross.startNewSubPath (x1, y0);
    cross.lineTo (x0, y1);
    g.strokePath (cross, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::String partLabel (Part part)
{
    switch (part)
    {
        case Part::Chords: return "Chords";
        case Part::Bass:   return "Bass";
        case Part::Both:   return "Chords + bass";
        case Part::Melody: return "Melody";
        case Part::Arp:    return "Arp";
        case Part::All:    return "All";
        default:           break;
    }

    return {};
}

constexpr float kGripW = 8.0f;
constexpr float kBadgeGap = 6.0f;
constexpr float kNoBadgeBelow = 70.0f;   // chips narrower than this never show the OFF badge
constexpr float kChipPadding = 12.0f;    // content <= chip width - 12
} // namespace

//==============================================================================
// The chip font rule (final-spec 7.10), shared with the shell selftest (declared there; not in the frozen header):
// 13 px with an 8 px grip gap; below 76 px 12 px / 6 px; below 64 px 11.5 px / 5 px. A chip's content is
// grip (8) + gap + label (+ 6 + OFF badge) and must fit chip width - 12. The OFF badge shows only where that rule still
// holds with it (never below 70 px; the Bass chip needs about 84 px, so the five-chip row shows it only at large sizes);
// otherwise OFF is a dimmed label plus a slash over the grip.
namespace cb::ui::exportpanel
{
float chipFontSize (float chipWidth) { return chipWidth >= 76.0f ? 13.0f : (chipWidth >= 64.0f ? 12.0f : 11.5f); }
float chipGripGap (float chipWidth)  { return chipWidth >= 76.0f ? 8.0f : (chipWidth >= 64.0f ? 6.0f : 5.0f); }

juce::Font chipBadgeFont() { return cb::ui::font (9.0f, 700).withExtraKerningFactor (0.08f); }
float chipBadgeWidth() { return widthOf (chipBadgeFont(), "OFF") + 8.0f; }

float chipLabelContentWidth (float chipWidth, const juce::String& label)
{
    return kGripW + chipGripGap (chipWidth) + widthOf (cb::ui::font (chipFontSize (chipWidth), 600), label);
}

bool chipShowsBadge (float chipWidth, const juce::String& label, bool off)
{
    return off && chipWidth >= kNoBadgeBelow
        && chipLabelContentWidth (chipWidth, label) + kBadgeGap + chipBadgeWidth() <= chipWidth - kChipPadding;
}

float chipContentWidth (float chipWidth, const juce::String& label, bool off)
{
    return chipLabelContentWidth (chipWidth, label) + (chipShowsBadge (chipWidth, label, off) ? kBadgeGap + chipBadgeWidth() : 0.0f);
}
} // namespace cb::ui::exportpanel

//==============================================================================
struct ExportPanel::Impl : public juce::Component,
                           public juce::TooltipClient,
                           private juce::Timer
{
    //==============================================================================
    class Chip : public juce::Component,
                 public juce::SettableTooltipClient
    {
    public:
        Chip (Impl& o, Part p, juce::Colour c) : owner (o), part (p), label (partLabel (p)), colour (c)
        {
            noFocus (*this);
            setMouseCursor (juce::MouseCursor::DraggingHandCursor);
            setTitle (label + " MIDI clip");
        }

        void paint (juce::Graphics& g) override
        {
            namespace ep = cb::ui::exportpanel;
            const auto r = getLocalBounds().toFloat().reduced (1.0f);
            const float radius = r.getHeight() * 0.5f;
            const bool enabled = isEnabled();
            const bool over = enabled && isMouseOver (true);
            const bool active = enabled && (pressed || dragging) && ! muted;

            if (active)
            {
                g.setColour (colour.withAlpha (0.18f));
                g.drawRoundedRectangle (r.expanded (0.5f), radius + 0.5f, 2.0f);
            }

            const auto base = over || active ? Col::panelHover : Col::panelRaised;
            g.setGradientFill (juce::ColourGradient::vertical (base.brighter (0.05f), r.getY(), base.darker (0.08f), r.getBottom()));
            g.fillRoundedRectangle (r, radius);

            if (! muted)
            {
                g.setColour (colour.withAlpha (active ? 0.26f : (over ? 0.15f : 0.05f)));
                g.fillRoundedRectangle (r, radius);
            }

            g.setColour (active ? colour.withAlpha (0.9f) : (over && ! muted ? colour.withAlpha (0.6f) : Col::outline));
            g.drawRoundedRectangle (r.reduced (0.5f), radius - 0.5f, 1.0f);

            // Font and grip gap by chip width (final-spec 7.10); the OFF badge only on chips >= 70 px.
            const float chipW = (float) getWidth();
            const auto font = cb::ui::font (ep::chipFontSize (chipW), 600);
            const bool badge = ep::chipShowsBadge (chipW, label, muted);
            const float gripGap = ep::chipGripGap (chipW);
            const float textW = widthOf (font, label);
            const float contentW = ep::chipContentWidth (chipW, label, muted);
            const float x = std::round (r.getCentreX() - contentW * 0.5f);
            const bool lively = enabled && ! muted;
            const auto gripCentre = juce::Point<float> (x + 3.0f, r.getCentreY());

            drawGrip (g, gripCentre, lively ? colour : Col::textFaint);

            if (muted && ! badge)
            {
                // Narrow OFF: a 6 px slash over the grip.
                g.setColour (Col::textDim.withAlpha (enabled ? 0.9f : 0.5f));
                g.drawLine (gripCentre.x - 3.5f + 1.0f, gripCentre.y + 3.5f, gripCentre.x + 2.5f + 1.0f, gripCentre.y - 3.5f, 1.4f);
            }

            g.setFont (font);
            g.setColour (lively ? Col::text : (enabled ? Col::textDim.withAlpha (muted ? 0.75f : 1.0f) : Col::textFaint));
            g.drawText (label, juce::Rectangle<float> (x + kGripW + gripGap, r.getY(), textW + 2.0f, r.getHeight()),
                        juce::Justification::centredLeft, false);

            if (badge)
            {
                const float badgeW = ep::chipBadgeWidth();
                const auto b = juce::Rectangle<float> (x + kGripW + gripGap + textW + kBadgeGap, r.getCentreY() - 7.0f, badgeW, 14.0f);
                g.setColour (Col::textFaint.withAlpha (enabled ? 0.9f : 0.5f));
                g.drawRoundedRectangle (b.reduced (0.5f), 4.0f, 1.0f);
                g.setFont (ep::chipBadgeFont());
                g.drawText ("OFF", b, juce::Justification::centred, false);
            }
        }

        void setState (bool shouldBeMuted, const juce::String& tip)
        {
            if (muted != shouldBeMuted)
            {
                muted = shouldBeMuted;
                repaint();
            }

            if (getTooltip() != tip)
            {
                setTooltip (tip);
                setDescription (tip);
            }

            setMouseCursor (isEnabled() && ! muted ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
        }

        void enablementChanged() override { repaint(); }
        void mouseEnter (const juce::MouseEvent&) override { repaint(); }
        void mouseExit (const juce::MouseEvent&) override  { repaint(); }

        void mouseDown (const juce::MouseEvent& e) override
        {
            pressed = false;
            dragStarted = false;

            if (e.mods.isPopupMenu())   // right-click: "Save <Part> as .mid..." (never a press)
            {
                if (isEnabled())
                    owner.showChipMenu (*this);
                return;
            }

            if (! isEnabled())
                return;

            pressed = true;
            repaint();
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (! pressed || e.mods.isPopupMenu() || ! isEnabled() || dragStarted || e.getDistanceFromDragStart() < 5)
                return;

            dragStarted = true;
            owner.startExternalDrag (*this);
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (pressed && ! e.mods.isPopupMenu() && isEnabled() && ! dragStarted && getLocalBounds().contains (e.getPosition()))
            {
                if (muted)
                    owner.showBassOffStatus();
                else
                    owner.showStatus ("Drag me onto FL Studio's playlist or piano roll", Col::textDim);
            }

            pressed = false;
            repaint();
        }

        Impl& owner;
        const Part part;
        const juce::String label;
        const juce::Colour colour;
        bool pressed = false, dragStarted = false, dragging = false, muted = false;
    };

    //==============================================================================
    // "Save v" pill in the caption row. Right-clicks are ignored (never act as a left click).
    class SaveButton : public juce::Button
    {
    public:
        bool compact = false;   // icon + chevron only

        SaveButton() : juce::Button ("Save")
        {
            noFocus (*this);   // Enter in the host must never re-open the save menu
            setTooltip ("Save as a .mid file: all parts, chords + bass, or one part");
            setTitle ("Save as a MIDI file");
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
        }

        static juce::Font labelFont() { return cb::ui::font (11.5f, 600); }

        int idealWidth (bool compactMode) const
        {
            return (int) std::ceil (8.0f + 10.0f + (compactMode ? 0.0f : 5.0f + widthOf (labelFont(), "Save")) + 5.0f + 7.0f + 8.0f);
        }

        void mouseDown (const juce::MouseEvent& e) override { if (! e.mods.isPopupMenu()) juce::Button::mouseDown (e); }
        void mouseDrag (const juce::MouseEvent& e) override { if (! e.mods.isPopupMenu()) juce::Button::mouseDrag (e); }
        void mouseUp (const juce::MouseEvent& e) override   { if (! e.mods.isPopupMenu()) juce::Button::mouseUp (e); }

        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            const auto r = getLocalBounds().toFloat().reduced (0.5f);
            const float radius = r.getHeight() * 0.5f;
            const bool enabled = isEnabled();
            const float alpha = enabled ? 1.0f : 0.4f;

            const auto base = enabled && (over || down) ? Col::panelHover : Col::panelRaised;
            g.setGradientFill (juce::ColourGradient::vertical (base.brighter (0.05f).withMultipliedAlpha (alpha), r.getY(),
                                                               base.darker (0.08f).withMultipliedAlpha (alpha), r.getBottom()));
            g.fillRoundedRectangle (r, radius);
            g.setColour (Col::accent.withAlpha ((over && enabled ? 0.85f : 0.45f) * alpha));
            g.drawRoundedRectangle (r.reduced (0.5f), radius - 0.5f, 1.0f);

            const auto font = labelFont();
            float x = r.getX() + 8.0f;
            drawSaveIcon (g, { x, r.getCentreY() - 5.0f, 10.0f, 10.0f }, Col::accent.withMultipliedAlpha (alpha));
            x += 10.0f;

            if (! compact)
            {
                const float textW = widthOf (font, "Save");
                g.setFont (font);
                g.setColour (enabled ? Col::text : Col::textFaint);
                g.drawText ("Save", juce::Rectangle<float> (x + 5.0f, r.getY(), textW + 2.0f, r.getHeight()),
                            juce::Justification::centredLeft, false);
                x += 5.0f + textW;
            }

            drawChevron (g, { x + 5.0f + 3.5f, r.getCentreY() }, (enabled ? Col::textDim : Col::textFaint));
        }
    };

    //==============================================================================
    explicit Impl (ChordbloomProcessor& p)
        : processor (p),
          chords (*this, Part::Chords, Col::tonic),
          bass (*this, Part::Bass, cb::ui::layerColour (cb::Layer::Bass)),
          melody (*this, Part::Melody, cb::ui::layerColour (cb::Layer::Melody)),
          arp (*this, Part::Arp, cb::ui::layerColour (cb::Layer::Arp)),
          all (*this, Part::All, Col::accent)
    {
        setOpaque (false);
        noFocus (*this);
        setTitle ("Drag to DAW");
        setDescription ("Drag the parts into your DAW as MIDI clips, or save them as a .mid file");

        for (auto* c : chips())
            addAndMakeVisible (c);

        addAndMakeVisible (save);
        save.onClick = [this] { showSaveMenu(); };
        updateEnablement();
        startTimerHz (4);
    }

    ~Impl() override { stopTimer(); }

    std::array<Chip*, 5> chips() { return { &chords, &bass, &melody, &arp, &all }; }

    Chip* chipFor (Part part)
    {
        for (auto* c : chips())
            if (c->part == part)
                return c;

        return nullptr;
    }

    //==============================================================================
    bool paramOn (const char* id, bool fallback) const
    {
        if (auto* v = processor.getAPVTS().getRawParameterValue (id))
            return v->load() >= 0.5f;

        return fallback;
    }

    int paramInt (const char* id, int fallback) const
    {
        if (auto* v = processor.getAPVTS().getRawParameterValue (id))
            return (int) std::lround (v->load());

        return fallback;
    }

    // The bass part's content is empty (auto with bassMode Off): the chip shows OFF and refuses (final-spec 6.4).
    bool bassContentEmpty() const
    {
        return ! processor.isLayerCustom (cb::Layer::Bass) && paramInt (cb::ParamIDs::bassMode, 1) == (int) cb::BassMode::Off;
    }

    // True when a melody note starts inside the progression's loop (what the Melody chip exports).
    bool hasMelodyInLoop() const
    {
        const double total = (double) processor.getProgression().totalBeats();

        for (const auto& n : processor.getMelody().notes)
            if (n.startBeat < total)
                return true;

        return false;
    }

    // The tracks the All chip writes: Chords, then the layers that play (final-spec 6.4).
    juce::StringArray allTracks() const
    {
        juce::StringArray tracks { "Chords" };

        for (auto layer : { cb::Layer::Bass, cb::Layer::Melody, cb::Layer::Arp })
            if (processor.isLayerPlaying (layer))
                tracks.add (cb::layerName (layer));

        return tracks;
    }

    juce::String arpDescription() const
    {
        if (processor.isLayerCustom (cb::Layer::Arp))
            return "edited";

        const auto s = processor.getArpSettings();
        return juce::String (cb::arpShapeName (s.shape)) + ", " + cb::rateDivisionName (s.rate);
    }

    void updateEnablement()
    {
        const bool hasChords = ! processor.getProgression().empty();

        for (auto* c : { (juce::Component*) &chords, (juce::Component*) &bass, (juce::Component*) &arp,
                         (juce::Component*) &all, (juce::Component*) &save })
            if (c->isEnabled() != hasChords)
                c->setEnabled (hasChords);

        const bool melodyReady = hasChords && hasMelodyInLoop();

        if (melody.isEnabled() != melodyReady)
            melody.setEnabled (melodyReady);

        const juce::String noChords ("Add some chords to the progression first");
        const juce::String mutedHere (", muted in Chordbloom");
        const bool bassOn = paramOn (cb::ParamIDs::bassOn, true);
        const bool arpOn = paramOn (cb::ParamIDs::arpOn, false);
        const bool bassEmpty = bassContentEmpty();

        chords.setState (false, hasChords ? juce::String ("Drag the chords (MIDI ch 1) into FL Studio: playlist, channel rack or piano roll")
                                          : noChords);

        if (! hasChords)
            bass.setState (bassEmpty, noChords);
        else if (bassEmpty)
            bass.setState (true, "Bass is Off: pick a bass pattern (Controls, or the Roll's Bass tab) to export a bass line");
        else
            bass.setState (false, "Drag the bass (" + juce::String (processor.isLayerCustom (cb::Layer::Bass) ? "edited"
                                                        : cb::bassModeName ((cb::BassMode) juce::jlimit (0, cb::numBassModes - 1,
                                                            paramInt (cb::ParamIDs::bassMode, 1))))
                                  + ", MIDI ch 2) into FL Studio" + (bassOn ? juce::String() : mutedHere));

        melody.setState (false, ! hasChords ? noChords
                                            : (melodyReady ? juce::String ("Drag the melody (one loop, MIDI ch 3) into FL Studio")
                                                                 + (paramOn (cb::ParamIDs::melodyOn, true) ? juce::String() : mutedHere)
                                                           : juce::String ("Draw or generate a melody first: switch the top panel to Roll")));

        arp.setState (false, hasChords ? "Drag the arp (" + arpDescription() + ") into FL" + (arpOn ? juce::String() : mutedHere)
                                       : noChords);

        const auto tracks = allTracks();
        all.setState (false, hasChords ? tracks.joinIntoString (", ") + ": " + juce::String (tracks.size())
                                             + (tracks.size() == 1 ? " track" : " tracks") + "; best on the playlist"
                                       : noChords);

        if (hasChords != lastHasChords)
        {
            lastHasChords = hasChords;
            repaint (captionArea.getSmallestIntegerContainer());
        }
    }

    //==============================================================================
    // Layout (final-spec 7.10): caption row (title, detail, Save pill), then five equal chips.
    void resized() override
    {
        auto r = getLocalBounds().reduced (10, 0);
        const int h = getHeight();
        const bool roomy = h >= 60;
        const int pad = roomy ? 7 : 5;
        r.removeFromTop (pad);
        r.removeFromBottom (pad);

        auto captionRow = r.removeFromTop (18);
        r.removeFromTop (roomy ? 5 : 2);
        const int chipH = juce::jlimit (16, 28, r.getHeight());
        auto row = r.withHeight (chipH);

        // Save pill on the right of the caption row (icon-only on a very narrow panel)
        save.compact = captionRow.getWidth() < 240;
        const int saveW = save.idealWidth (save.compact);
        save.setBounds (captionRow.removeFromRight (saveW));
        captionRow.removeFromRight (8);
        captionArea = captionRow.toFloat();

        // Five equal chips, 6 px apart (fractional widths rounded per edge so all stay within 1 px of each other)
        constexpr int gap = 6;
        const float chipW = (float) (row.getWidth() - 4 * gap) / 5.0f;
        int i = 0;

        for (auto* c : chips())
        {
            const int x0 = row.getX() + juce::roundToInt ((float) i * (chipW + (float) gap));
            const int x1 = i == 4 ? row.getRight() : row.getX() + juce::roundToInt ((float) i * (chipW + (float) gap) + chipW);
            c->setBounds (x0, row.getY(), x1 - x0, row.getHeight());
            ++i;
        }
    }

    void paint (juce::Graphics& g) override
    {
        paintCard (g, getLocalBounds().toFloat());
        actionArea = {};

        if (captionArea.getWidth() < 10.0f)
            return;

        auto r = captionArea.withTrimmedLeft (4.0f);

        if (status.isNotEmpty())
        {
            paintStatus (g, r);
            return;
        }

        const auto titleFont = cb::ui::font (10.5f, 600).withExtraKerningFactor (0.12f);
        const juce::String title ("DRAG TO DAW");
        g.setFont (titleFont);
        g.setColour (lastHasChords ? Col::textDim : Col::textFaint);
        g.drawText (title, r, juce::Justification::centredLeft, false);

        const auto detailFont = cb::ui::font (11.0f, 400);
        const juce::String detail = lastHasChords ? "drop on the playlist or a piano roll" : "add chords to the progression first";
        const auto detailArea = r.withTrimmedLeft (widthOf (titleFont, title) + 10.0f);

        if (widthOf (detailFont, detail) <= detailArea.getWidth())
        {
            g.setFont (detailFont);
            g.setColour (Col::textFaint);
            g.drawText (detail, detailArea, juce::Justification::centredLeft, false);
        }
    }

    // Status line (replaces the caption text only; the Save pill stays). An actionable one ends with a clickable link.
    void paintStatus (juce::Graphics& g, juce::Rectangle<float> r)
    {
        const auto font = cb::ui::font (11.5f, 600);

        if (statusAction == nullptr)
        {
            auto text = status;

            if (widthOf (font, text) > r.getWidth() && statusShort.isNotEmpty())
                text = statusShort;

            g.setFont (font);
            g.setColour (statusColour);
            g.drawFittedText (text, r.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
            return;
        }

        const auto linkFont = cb::ui::font (11.5f, 700);
        const float linkW = 12.0f + 5.0f + widthOf (linkFont, statusActionLabel);
        auto text = status;

        if (widthOf (font, text) + 14.0f + linkW > r.getWidth() && statusShort.isNotEmpty())
            text = statusShort;

        const float textW = std::min (widthOf (font, text) + 2.0f, std::max (0.0f, r.getWidth() - linkW - 14.0f));
        g.setFont (font);
        g.setColour (statusColour);
        g.drawText (text, r.withWidth (textW), juce::Justification::centredLeft, true);

        auto link = juce::Rectangle<float> (r.getX() + textW + 10.0f, r.getY() - 1.0f, linkW + 8.0f, r.getHeight() + 2.0f);
        actionArea = link;

        if (actionHover)
        {
            g.setColour (Col::accent.withAlpha (0.16f));
            g.fillRoundedRectangle (link, link.getHeight() * 0.5f);
        }

        drawMuteIcon (g, juce::Rectangle<float> (link.getX() + 4.0f, link.getCentreY() - 6.0f, 12.0f, 12.0f), Col::accent);
        g.setFont (linkFont);
        g.setColour (Col::accent.brighter (actionHover ? 0.2f : 0.0f));
        g.drawText (statusActionLabel, link.withTrimmedLeft (21.0f), juce::Justification::centredLeft, false);
    }

    //==============================================================================
    void showStatus (const juce::String& text, juce::Colour colour, int durationMs = 3000,
                     std::function<void()> action = {}, const juce::String& actionLabel = {}, const juce::String& shortText = {})
    {
        status = text;
        statusShort = shortText;
        statusColour = colour;
        statusAction = std::move (action);
        statusActionLabel = actionLabel;
        statusUntil = juce::Time::getMillisecondCounter() + (juce::uint32) durationMs;
        actionHover = false;
        repaint();
    }

    void clearStatus()
    {
        status.clear();
        statusAction = nullptr;
        actionArea = {};
        actionHover = false;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint();
    }

    void showBassOffStatus()
    {
        showStatus ("Bass is Off: pick a bass pattern first", Col::subdominant, 3500, {}, {}, "Bass is Off");
    }

    // I2: Chordbloom keeps playing along with the host after the clip went into FL; offer to mute it.
    void showPlaysAlongHint()
    {
        if (! paramOn (cb::ParamIDs::syncToHost, false))
            return;

        if (! paramOn (cb::ParamIDs::previewOn, true) && ! paramOn (cb::ParamIDs::midiOut, true))
            return;   // Chordbloom makes no sound and sends no MIDI: nothing doubles up

        const auto now = juce::Time::getMillisecondCounter();

        if (lastHintAt != 0 && now - lastHintAt < 60000)
            return;

        lastHintAt = now;
        juce::Component::SafePointer<Impl> safe (this);
        showStatus ("Dropped into FL? Chordbloom still plays along", Col::subdominant, 10000,
                    [safe] { if (safe != nullptr) safe->stopPlayingWithHost(); }, "Mute", "Still playing along");
    }

    void stopPlayingWithHost()
    {
        if (auto* param = processor.getAPVTS().getParameter (cb::ParamIDs::syncToHost))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (0.0f);
            param->endChangeGesture();
        }

        showStatus ("Muted: Chordbloom stays quiet while FL plays (Play with FL is off)", Col::accent, 4500, {}, {},
                    "Muted: Play with FL is off");
    }

    juce::String getTooltip() override
    {
        if (statusAction != nullptr && actionHover)
            return "Turns Play with FL off, so only your FL clip plays. Turn it back on in the header any time.";

        return {};
    }

    void updateActionHover (juce::Point<float> p)
    {
        const bool over = statusAction != nullptr && actionArea.contains (p);

        if (over != actionHover)
        {
            actionHover = over;
            setMouseCursor (over ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
            repaint (captionArea.expanded (2.0f, 4.0f).getSmallestIntegerContainer());
        }
    }

    void mouseMove (const juce::MouseEvent& e) override  { updateActionHover (e.position); }
    void mouseExit (const juce::MouseEvent&) override    { updateActionHover ({ -100.0f, -100.0f }); }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (statusAction != nullptr && ! e.mods.isPopupMenu() && actionArea.contains (e.position)
            && actionArea.contains (e.mouseDownPosition))
        {
            auto action = statusAction;
            statusAction = nullptr;
            action();
        }
    }

    //==============================================================================
    void startExternalDrag (Chip& chip)
    {
        if (chip.part == Part::Bass && bassContentEmpty())
        {
            showBassOffStatus();
            return;
        }

        const auto file = processor.writeMidiToTempFile (chip.part);

        if (! file.existsAsFile())
        {
            showStatus ("Nothing to export yet", Col::dominant);
            return;
        }

        chip.dragging = true;
        chip.repaint();

        juce::Component::SafePointer<Chip> safeChip (&chip);
        juce::Component::SafePointer<Impl> safe (this);
        const bool started = juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, &chip,
            [safeChip, safe]
            {
                if (auto* c = safeChip.getComponent())
                {
                    c->dragging = false;
                    c->pressed = false;
                    c->repaint();
                }

                if (safe != nullptr)
                    safe->externalDragFinished();
            });

        if (! started)
        {
            chip.dragging = false;
            chip.repaint();
            showStatus ("Couldn't start the drag", Col::dominant);
        }
    }

    // The OS drag reports no result: a release outside the plugin window is taken as "dropped into the host".
    void externalDragFinished()
    {
        if (auto* top = getTopLevelComponent())
            if (top->getScreenBounds().contains (juce::Desktop::getMousePosition()))
                return;

        showPlaysAlongHint();
    }

    //==============================================================================
    // Menus (final-spec 7.10): these builders are the only source of what the async menus show.
    enum SaveIds { saveAll = 1, saveBoth, saveChords, saveBass, saveMelody, saveArp };

    static Part partForSaveId (int id)
    {
        switch (id)
        {
            case saveAll:    return Part::All;
            case saveBoth:   return Part::Both;
            case saveChords: return Part::Chords;
            case saveBass:   return Part::Bass;
            case saveMelody: return Part::Melody;
            case saveArp:    return Part::Arp;
            default:         break;
        }

        return Part::Chords;
    }

    bool canExport (Part part) const
    {
        const bool hasChords = ! processor.getProgression().empty();

        switch (part)
        {
            case Part::Chords: case Part::Arp: case Part::All: return hasChords;
            case Part::Bass: case Part::Both:                 return hasChords && ! bassContentEmpty();
            case Part::Melody:                                return hasChords && hasMelodyInLoop();
            default:                                          break;
        }

        return false;
    }

    juce::PopupMenu createSaveMenu() const
    {
        const bool off = bassContentEmpty();
        const int numTracks = allTracks().size();
        juce::PopupMenu menu;
        menu.addSectionHeader ("Save as .mid file");
        menu.addItem (saveAll, "All parts  (" + juce::String (numTracks) + (numTracks == 1 ? " track)" : " tracks)"), canExport (Part::All));
        menu.addItem (saveBoth, off ? "Chords + bass  (Bass is Off)" : "Chords + bass  (2 tracks)", canExport (Part::Both));
        menu.addItem (saveChords, "Chords only", canExport (Part::Chords));
        menu.addItem (saveBass, off ? "Bass only  (Bass is Off)" : "Bass only", canExport (Part::Bass));
        menu.addItem (saveMelody, hasMelodyInLoop() ? juce::String ("Melody only") : juce::String ("Melody only  (no melody yet)"),
                      canExport (Part::Melody));
        menu.addItem (saveArp, "Arp only", canExport (Part::Arp));
        return menu;
    }

    juce::PopupMenu createChipMenu (Part part) const
    {
        juce::PopupMenu menu;

        if (part == Part::Both)
            return menu;

        const auto name = part == Part::All ? juce::String ("all parts") : partLabel (part);
        menu.addItem (1, "Save " + name + " as .mid" + utf8 ("\xe2\x80\xa6"), canExport (part));
        return menu;
    }

    void showSaveMenu()
    {
        auto menu = createSaveMenu();
        menu.setLookAndFeel (&getLookAndFeel());
        juce::Component::SafePointer<Impl> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&save), [safe] (int result)
        {
            if (safe != nullptr && result != 0)
                safe->launchSaveChooser (partForSaveId (result));
        });
    }

    void showChipMenu (Chip& chip)
    {
        auto menu = createChipMenu (chip.part);
        menu.setLookAndFeel (&getLookAndFeel());
        juce::Component::SafePointer<Impl> safe (this);
        const auto part = chip.part;
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&chip), [safe, part] (int result)
        {
            if (safe != nullptr && result == 1)
                safe->launchSaveChooser (part);
        });
    }

    void launchSaveChooser (Part part)
    {
        auto dir = lastSaveDirectory();

        if (! dir.isDirectory())
            dir = juce::File::getSpecialLocation (juce::File::userMusicDirectory);

        if (! dir.isDirectory())
            dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);

        // B14 / I25: the processor's name keeps '#' (F#m, C# minor) and describes the progression, like the drag files.
        const auto name = processor.getExportFileName (part);

        chooser = std::make_unique<juce::FileChooser> ("Save MIDI file", dir.getChildFile (name), "*.mid;*.midi", true);

        const int chooserFlags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                          | juce::FileBrowserComponent::warnAboutOverwriting;

        juce::Component::SafePointer<Impl> safe (this);
        chooser->launchAsync (chooserFlags, [safe, part] (const juce::FileChooser& fc)
        {
            if (safe == nullptr)
                return;

            auto file = fc.getResult();

            if (file == juce::File())
                return;

            if (! file.hasFileExtension ("mid;midi"))
                file = file.withFileExtension ("mid");

            lastSaveDirectory() = file.getParentDirectory();
            const bool ok = safe->processor.saveMidiFile (file, part);
            safe->showStatus (ok ? "Saved " + file.getFileName() : juce::String ("Couldn't save the file"),
                              ok ? Col::accent : Col::dominant, 3000, {}, {}, ok ? juce::String ("Saved") : juce::String());
        });
    }

    void timerCallback() override
    {
        updateEnablement();

        if (status.isNotEmpty() && juce::Time::getMillisecondCounter() > statusUntil && ! actionHover)
            clearStatus();
    }

    //==============================================================================
    ChordbloomProcessor& processor;
    Chip chords, bass, melody, arp, all;
    SaveButton save;
    std::unique_ptr<juce::FileChooser> chooser;

    juce::Rectangle<float> captionArea, actionArea;
    bool lastHasChords = false;

    juce::String status, statusShort, statusActionLabel;
    juce::Colour statusColour;
    std::function<void()> statusAction;
    bool actionHover = false;
    juce::uint32 statusUntil = 0;
    juce::uint32 lastHintAt = 0;
};

//==============================================================================
ExportPanel::ExportPanel (ChordbloomProcessor& p) : impl (std::make_unique<Impl> (p))
{
    setOpaque (false);
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    setTitle ("Drag to DAW");
    addAndMakeVisible (*impl);
}

ExportPanel::~ExportPanel() = default;

void ExportPanel::paint (juce::Graphics&) {}   // the Impl surface paints the panel

void ExportPanel::resized() { impl->setBounds (getLocalBounds()); }

void ExportPanel::refresh() { impl->updateEnablement(); }

void ExportPanel::showPlaysAlongHint() { impl->showPlaysAlongHint(); }
void ExportPanel::notifyExternalDragFinished() { impl->externalDragFinished(); }

juce::Rectangle<int> ExportPanel::getChipBounds (ChordbloomProcessor::ExportPart part) const
{
    if (auto* chip = impl->chipFor (part))
        return chip->getBounds() + impl->getPosition();

    return {};
}

juce::Rectangle<int> ExportPanel::getSaveButtonBounds() const { return impl->save.getBounds() + impl->getPosition(); }

juce::PopupMenu ExportPanel::createSaveMenu() const { return impl->createSaveMenu(); }

juce::PopupMenu ExportPanel::createChipMenu (ChordbloomProcessor::ExportPart part) const { return impl->createChipMenu (part); }
