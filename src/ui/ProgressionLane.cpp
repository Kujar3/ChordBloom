#include "ui/ProgressionLane.h"

#include "core/Generate.h"
#include "ui/DragPayload.h"
#include "ui/Theme.h"

#include <algorithm>
#include <cmath>

namespace
{
namespace Col = cb::ui::Colours;

constexpr float kBlockRadius = 6.0f;
constexpr float kTopBar = 3.0f;
constexpr float kDragThreshold = 6.0f;
constexpr float kEdgeGrab = 6.0f;
constexpr int kMinVisibleBeats = 16;
constexpr int kPillH = 24;

// Spark settings live as plain properties on the APVTS state tree, so they are saved with the plugin state.
constexpr const char* kSparkMoodProp   = "cbSparkMood";
constexpr const char* kSparkChordsProp = "cbSparkChords";
constexpr const char* kSparkBeatsProp  = "cbSparkBeats";

// Popup menu item ids
enum MenuIds
{
    idDuplicate = 1, idDelete = 2, idClear = 3,
    idLengthBase = 100,       // + beats
    idInversionAuto = 200,    // 201 + inversion
    idOctaveBase = 310,       // + octave shift (-2..+2)
    idChangeBase = 400,       // + ChordQuality
    idSuggestBase = 500,      // + index into the captured suggestion list
    idTransposeUp = 600, idTransposeDown = 601, idMoveToKey = 602,
    idSpark = 700,
    idMoodBase = 800,         // + Mood
    idChordsBase = 810,       // + number of chords
    idBeatsBase = 820,        // + beats per chord
    idPresetBase = 1000       // + preset index
};

juce::String toJuce (const std::string& s) { return juce::String::fromUTF8 (s.data(), (int) s.size()); }

const juce::String& middleDot()
{
    static const juce::String dot = juce::String::charToString ((juce::juce_wchar) 0x00b7);
    return dot;
}

float widthOf (const juce::Font& f, const juce::String& s) { return juce::GlyphArrangement::getStringWidth (f, s); }

juce::Font fittedFont (float height, int weight, const juce::String& text, float maxWidth, float minHeight)
{
    auto f = cb::ui::font (height, weight);

    for (float h = height; h > minHeight && widthOf (f, text) > maxWidth;)
    {
        h = std::max (minHeight, h - 0.5f);
        f = cb::ui::font (h, weight);
    }

    return f;
}

// "1 beat", "3 beats", "1 bar", "2 bars", "6 beats"
juce::String lengthText (int beats)
{
    if (beats % 4 == 0)
        return juce::String (beats / 4) + (beats == 4 ? " bar" : " bars");

    return juce::String (beats) + (beats == 1 ? " beat" : " beats");
}

// "4 bars", "4 bars + 2 beats", "3 beats"
juce::String totalText (int beats)
{
    const int bars = beats / 4;
    const int rest = beats % 4;

    if (bars == 0)
        return juce::String (rest) + (rest == 1 ? " beat" : " beats");

    juce::String s;
    s << bars << (bars == 1 ? " bar" : " bars");

    if (rest > 0)
        s << " + " << rest << (rest == 1 ? " beat" : " beats");

    return s;
}

int roundUpToBar (int beats) { return ((beats + 3) / 4) * 4; }

// "+3" / "-2" (typographic minus)
juce::String signedSemitones (int n)
{
    return (n > 0 ? juce::String ("+") : juce::String::charToString ((juce::juce_wchar) 0x2212)) + juce::String (std::abs (n));
}

// "A minor", "Eb major", "D Dorian"
juce::String shortKeyName (const cb::Key& k)
{
    juce::String scale;

    switch (k.scale)
    {
        case cb::ScaleType::Major:         scale = "major"; break;
        case cb::ScaleType::NaturalMinor:  scale = "minor"; break;
        case cb::ScaleType::HarmonicMinor: scale = "harmonic minor"; break;
        case cb::ScaleType::MelodicMinor:  scale = "melodic minor"; break;
        default:                           scale = cb::scaleInfo (k.scale).name; break;
    }

    return toJuce (k.tonicName()) + " " + scale;
}

// Small "x" remove button shown on the hovered block.
bool hasCloseButton (juce::Rectangle<float> block) { return block.getWidth() >= 56.0f && block.getHeight() >= 40.0f; }

juce::Rectangle<float> closeButtonBounds (juce::Rectangle<float> block)
{
    const float d = 15.0f;
    return { block.getRight() - 9.0f - d, block.getY() + kTopBar + 4.0f, d, d };
}

void drawHalo (juce::Graphics& g, juce::Rectangle<float> r, float radius, juce::Colour colour, float strength, int rings)
{
    for (int i = rings; i >= 1; --i)
    {
        const float fade = (float) (rings + 1 - i) / (float) rings;
        g.setColour (colour.withAlpha (juce::jlimit (0.0f, 1.0f, strength * fade * fade)));
        const float grow = 0.5f + (float) i;
        g.drawRoundedRectangle (r.expanded (grow), radius + grow, 1.25f);
    }
}

void noFocus (juce::Component& c)
{
    c.setWantsKeyboardFocus (false);
    c.setMouseClickGrabsKeyboardFocus (false);
}

//==============================================================================
// Icons (drawn with Path, sized to `r`)
enum class Glyph { none, spark, presets, move, live, stop };

void drawSparkle (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour colour)
{
    auto star = [] (juce::Point<float> c, float rad)
    {
        juce::Path p;
        const float w = rad * 0.28f;
        p.startNewSubPath (c.x, c.y - rad);
        p.quadraticTo (c.x + w, c.y - w, c.x + rad, c.y);
        p.quadraticTo (c.x + w, c.y + w, c.x, c.y + rad);
        p.quadraticTo (c.x - w, c.y + w, c.x - rad, c.y);
        p.quadraticTo (c.x - w, c.y - w, c.x, c.y - rad);
        p.closeSubPath();
        return p;
    };

    const float s = std::min (r.getWidth(), r.getHeight());
    const auto c = r.getCentre();
    g.setColour (colour);
    g.fillPath (star ({ c.x - s * 0.08f, c.y + s * 0.06f }, s * 0.42f));
    g.fillPath (star ({ c.x + s * 0.34f, c.y - s * 0.32f }, s * 0.17f));
}

void drawPresetsIcon (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour colour)
{
    // Three stacked rows with a leading dot: a list of ready-made progressions.
    const float s = std::min (r.getWidth(), r.getHeight());
    const auto c = r.getCentre();
    g.setColour (colour);

    for (int i = -1; i <= 1; ++i)
    {
        const float y = c.y + (float) i * s * 0.34f;
        g.fillEllipse (juce::Rectangle<float> (2.4f, 2.4f).withCentre ({ c.x - s * 0.38f, y }));
        g.fillRoundedRectangle (c.x - s * 0.18f, y - 0.7f, s * 0.62f, 1.4f, 0.7f);
    }
}

void drawMoveIcon (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour colour)
{
    // Arrow into a bar: "snap the chords to the key".
    const float s = std::min (r.getWidth(), r.getHeight());
    const auto c = r.getCentre();
    juce::Path p;
    p.startNewSubPath (c.x - s * 0.45f, c.y);
    p.lineTo (c.x + s * 0.22f, c.y);
    p.startNewSubPath (c.x - s * 0.02f, c.y - s * 0.26f);
    p.lineTo (c.x + s * 0.24f, c.y);
    p.lineTo (c.x - s * 0.02f, c.y + s * 0.26f);
    p.startNewSubPath (c.x + s * 0.45f, c.y - s * 0.36f);
    p.lineTo (c.x + s * 0.45f, c.y + s * 0.36f);
    g.setColour (colour);
    g.strokePath (p, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void drawStopIcon (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour colour)
{
    // Speaker with a cross: "stop playing along".
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
    g.strokePath (cross, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void drawChevron (juce::Graphics& g, juce::Point<float> c, juce::Colour colour)
{
    juce::Path p;
    p.startNewSubPath (c.x - 3.5f, c.y - 1.6f);
    p.lineTo (c.x, c.y + 1.9f);
    p.lineTo (c.x + 3.5f, c.y - 1.6f);
    g.setColour (colour);
    g.strokePath (p, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

//==============================================================================
// Pill-shaped header button: optional icon, label, dim detail text and a chevron (optionally its own click zone).
class PillButton : public juce::Button
{
public:
    enum class Style { outline, accent, filled, info, live };

    PillButton (const juce::String& name, Glyph g, Style s) : juce::Button (name), glyph (g), style (s)
    {
        noFocus (*this);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTitle (name);
    }

    juce::String text, detail;
    bool chevron = false;        // draws a small chevron at the right
    bool splitChevron = false;   // the chevron is its own click zone (onMenu), the rest is onPrimary
    bool compact = false;        // icon (+ chevron) only
    bool showDetail = true;
    float fontSize = 12.0f;
    float pulse = 0.0f;          // live dot animation 0..1
    std::function<void()> onPrimary, onMenu;

    static constexpr float kChevronZone = 22.0f;

    juce::Font labelFont() const { return cb::ui::font (fontSize, 600); }
    juce::Font detailFont() const { return cb::ui::font (fontSize - 0.5f, 400); }

    // Right-hand zones: split chevron, plain chevron, stop icon (live style).
    float trailingZone() const
    {
        return (chevron ? (splitChevron ? kChevronZone : 16.0f) : 0.0f) + (style == Style::live ? 24.0f : 0.0f);
    }

    int idealWidth (bool compactMode, bool withDetail, const juce::String& label) const
    {
        const float iconW = glyph == Glyph::none ? 0.0f : 13.0f;
        float w = 1.0f + 11.0f + iconW;

        if (! compactMode && label.isNotEmpty())
        {
            w += (iconW > 0.0f ? 6.0f : 0.0f) + widthOf (labelFont(), label);

            if (withDetail && detail.isNotEmpty())
                w += 6.0f + widthOf (detailFont(), detail);
        }

        const float zone = trailingZone();
        w += zone > 0.0f ? (compactMode ? 3.0f : 6.0f) + zone : 11.0f;
        return (int) std::ceil (w);
    }

    int idealWidth() const { return idealWidth (compact, showDetail, text); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        lastDownX = e.position.x;
        juce::Button::mouseDown (e);
    }

    void clicked (const juce::ModifierKeys& mods) override
    {
        const bool menuZone = mods.isPopupMenu() || (chevron && ! splitChevron)
                              || (splitChevron && lastDownX >= (float) getWidth() - kChevronZone);

        if (menuZone && onMenu != nullptr)
            onMenu();
        else if (onPrimary != nullptr)
            onPrimary();
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        const float radius = r.getHeight() * 0.5f;
        const bool enabled = isEnabled();
        const float alpha = enabled ? 1.0f : 0.38f;
        over = over && enabled;

        const bool chevronHot = splitChevron && over && getMouseXYRelative().x >= getWidth() - (int) kChevronZone;
        const bool mainHot = over && ! chevronHot;

        juce::Colour tint, edge, ink, iconInk;

        switch (style)
        {
            case Style::accent:
                tint = Col::accent.withAlpha (mainHot || chevronHot ? 0.17f : 0.10f);
                edge = Col::accent.withAlpha (over ? 0.75f : 0.45f);
                ink = Col::text;
                iconInk = Col::accent;
                break;

            case Style::filled:
                tint = (down ? Col::accent.darker (0.12f) : (over ? Col::accent.brighter (0.12f) : Col::accent));
                edge = juce::Colours::white.withAlpha (0.16f);
                ink = Col::background;
                iconInk = Col::background;
                break;

            case Style::info:
                tint = Col::secondary.withAlpha (over ? 0.20f : 0.12f);
                edge = Col::secondary.withAlpha (over ? 0.8f : 0.5f);
                ink = Col::text;
                iconInk = Col::secondary.brighter (0.2f);
                break;

            case Style::live:
                tint = (over ? Col::dominant.withAlpha (0.14f) : Col::accent.withAlpha (0.08f));
                edge = over ? Col::dominant.withAlpha (0.7f) : Col::accent.withAlpha (0.35f);
                ink = over ? Col::text : Col::textDim.brighter (0.2f);
                iconInk = over ? Col::dominant : Col::textDim;
                break;

            case Style::outline:
            default:
                tint = over || down ? (down ? Col::panelRaised : Col::panelHover) : juce::Colours::transparentBlack;
                edge = Col::outline;
                ink = over ? Col::text : Col::textDim;
                iconInk = ink;
                break;
        }

        if (style == Style::filled)
        {
            g.setColour (Col::accent.withAlpha (0.16f * alpha));
            g.fillRoundedRectangle (r.expanded (1.5f), radius + 1.5f);
            g.setGradientFill (juce::ColourGradient::vertical (tint.brighter (0.08f).withMultipliedAlpha (alpha), r.getY(),
                                                               tint.darker (0.10f).withMultipliedAlpha (alpha), r.getBottom()));
            g.fillRoundedRectangle (r, radius);
        }
        else
        {
            if (style != Style::outline)
            {
                g.setGradientFill (juce::ColourGradient::vertical (Col::panelRaised.withMultipliedAlpha (alpha), r.getY(),
                                                                   Col::panelRaised.darker (0.08f).withMultipliedAlpha (alpha), r.getBottom()));
                g.fillRoundedRectangle (r, radius);
            }

            g.setColour (tint.withMultipliedAlpha (alpha));
            g.fillRoundedRectangle (r, radius);

            if (chevronHot)
            {
                juce::Graphics::ScopedSaveState save (g);
                juce::Path clip;
                clip.addRoundedRectangle (r, radius);
                g.reduceClipRegion (clip);
                g.setColour (juce::Colours::white.withAlpha (0.06f));
                g.fillRect (r.withLeft (r.getRight() - kChevronZone));
            }
        }

        g.setColour (edge.withMultipliedAlpha (alpha));
        g.drawRoundedRectangle (r.reduced (0.5f), radius - 0.5f, 1.0f);

        ink = ink.withMultipliedAlpha (alpha);
        iconInk = iconInk.withMultipliedAlpha (alpha);

        // Content
        auto content = r;

        if (chevron)
        {
            auto zone = content.removeFromRight (splitChevron ? kChevronZone : 16.0f);

            if (splitChevron)
            {
                g.setColour (edge.withMultipliedAlpha (0.6f * alpha));
                g.fillRect (juce::Rectangle<float> (zone.getX(), r.getY() + 5.0f, 1.0f, r.getHeight() - 10.0f));
                drawChevron (g, zone.getCentre().translated (-0.5f, 0.0f), (chevronHot ? Col::text : ink));
            }
            else
            {
                drawChevron (g, zone.getCentre().translated (-3.0f, 0.0f), ink.withMultipliedAlpha (0.85f));
            }
        }

        if (style == Style::live)
        {
            auto zone = content.removeFromRight (24.0f);
            drawStopIcon (g, zone.withSizeKeepingCentre (12.0f, 12.0f).translated (-3.0f, 0.0f), iconInk);
        }

        const auto lf = labelFont();
        const auto df = detailFont();
        const bool withLabel = ! compact && text.isNotEmpty();
        const bool withDetail = withLabel && showDetail && detail.isNotEmpty();
        const float iconW = glyph == Glyph::none ? 0.0f : 13.0f;
        const float labelW = withLabel ? widthOf (lf, text) : 0.0f;
        const float detailW = withDetail ? widthOf (df, detail) : 0.0f;
        const float totalW = iconW + (withLabel && iconW > 0.0f ? 6.0f : 0.0f) + labelW + (withDetail ? 6.0f + detailW : 0.0f);

        float x = (chevron || style == Style::live) && withLabel ? content.getX() + 11.0f
                                                                 : std::round (content.getCentreX() - totalW * 0.5f);
        x = std::max (x, content.getX() + (withLabel ? 8.0f : 3.0f));
        const float cy = r.getCentreY();

        if (iconW > 0.0f)
        {
            const auto iconArea = juce::Rectangle<float> (x, cy - 6.5f, 13.0f, 13.0f);

            switch (glyph)
            {
                case Glyph::spark:   drawSparkle (g, iconArea, iconInk); break;
                case Glyph::presets: drawPresetsIcon (g, iconArea, iconInk); break;
                case Glyph::move:    drawMoveIcon (g, iconArea, iconInk); break;
                case Glyph::live:
                {
                    const auto c = iconArea.getCentre();
                    const float grow = 3.0f + 3.0f * pulse;
                    g.setColour (Col::accent.withAlpha ((0.35f * (1.0f - pulse)) * alpha));
                    g.fillEllipse (juce::Rectangle<float> (6.0f + grow, 6.0f + grow).withCentre (c));
                    g.setColour (Col::accent.withMultipliedAlpha (alpha));
                    g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre (c));
                    break;
                }
                case Glyph::stop:    drawStopIcon (g, iconArea, iconInk); break;
                case Glyph::none:
                default: break;
            }

            x += iconW + (withLabel ? 6.0f : 0.0f);
        }

        if (withLabel)
        {
            const float maxW = std::max (0.0f, content.getRight() - x);
            g.setFont (lf);
            g.setColour (ink);
            g.drawText (text, juce::Rectangle<float> (x, r.getY(), std::min (labelW + 2.0f, maxW), r.getHeight()),
                        juce::Justification::centredLeft, true);
            x += labelW + 6.0f;

            if (withDetail && x + detailW <= content.getRight() + 0.5f)
            {
                g.setFont (df);
                g.setColour ((style == Style::filled ? Col::background.withAlpha (0.7f) : Col::textFaint.brighter (0.25f))
                                 .withMultipliedAlpha (alpha));
                g.drawText (detail, juce::Rectangle<float> (x, r.getY(), detailW + 2.0f, r.getHeight()),
                            juce::Justification::centredLeft, false);
            }
        }
    }

private:
    Glyph glyph;
    Style style;
    float lastDownX = 0.0f;
};

//==============================================================================
class PlayButton : public juce::Button
{
public:
    PlayButton() : juce::Button ("Play")
    {
        noFocus (*this);   // FL keeps its shortcuts: Enter must never toggle playback here
        setTooltip ("Play / stop the progression loop");
        setTitle ("Play or stop the progression loop");
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat();
        const float d = std::min (r.getWidth(), r.getHeight()) - 4.0f;
        r = r.withSizeKeepingCentre (d, d);

        const bool playing = getToggleState();
        const float alpha = isEnabled() ? 1.0f : 0.35f;
        auto base = Col::accent;

        if (over && isEnabled())
            base = base.brighter (0.15f);

        if (down)
            base = base.darker (0.15f);

        if (playing)
        {
            g.setColour (Col::accent.withAlpha (0.18f));
            g.fillEllipse (r.expanded (2.0f));
        }

        g.setGradientFill (juce::ColourGradient::vertical (base.brighter (0.1f).withMultipliedAlpha (alpha), r.getY(),
                                                           base.darker (0.12f).withMultipliedAlpha (alpha), r.getBottom()));
        g.fillEllipse (r);
        g.setColour (juce::Colours::white.withAlpha (0.18f * alpha));
        g.drawEllipse (r.reduced (0.5f), 1.0f);

        const auto c = r.getCentre();
        const float s = d * 0.34f;
        g.setColour (Col::background.withAlpha (0.92f * alpha));

        if (playing)
        {
            g.fillRoundedRectangle (juce::Rectangle<float> (s * 0.95f, s * 0.95f).withCentre (c), 1.5f);
        }
        else
        {
            juce::Path tri;
            tri.addTriangle (c.x - s * 0.36f, c.y - s * 0.55f, c.x - s * 0.36f, c.y + s * 0.55f, c.x + s * 0.6f, c.y);
            g.fillPath (tri, juce::AffineTransform::translation (s * 0.06f, 0.0f));
        }
    }
};

class ClearButton : public juce::Button
{
public:
    ClearButton() : juce::Button ("Clear")
    {
        noFocus (*this);
        setTooltip ("Clear the progression (undo brings it back)");
        setTitle ("Clear the progression");
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const auto r = getLocalBounds().toFloat().reduced (0.5f);
        const bool enabled = isEnabled();
        const float alpha = enabled ? 1.0f : 0.35f;
        const float radius = r.getHeight() * 0.5f;

        if (enabled && (over || down))
        {
            g.setColour (down ? Col::panelRaised : Col::panelHover);
            g.fillRoundedRectangle (r, radius);
        }

        g.setColour (Col::outline.withMultipliedAlpha (alpha));
        g.drawRoundedRectangle (r, radius, 1.0f);

        const auto ink = ((enabled && over) ? Col::text : Col::textDim).withMultipliedAlpha (alpha);
        const auto font = cb::ui::font (12.0f, 600);
        const juce::String label ("Clear");
        const float textW = widthOf (font, label);
        const float iconW = 10.0f;
        const bool iconOnly = r.getWidth() < iconW + 6.0f + textW + 16.0f;
        const float x = std::round (r.getCentreX() - (iconOnly ? iconW : (iconW + 6.0f + textW)) * 0.5f);
        const float cy = r.getCentreY();

        // Trash-can icon
        g.setColour (ink);
        g.fillRoundedRectangle (x, cy - 4.5f, iconW, 1.4f, 0.7f);
        g.fillRoundedRectangle (x + 3.0f, cy - 6.0f, iconW - 6.0f, 1.4f, 0.7f);
        juce::Path body;
        body.addRoundedRectangle (x + 1.2f, cy - 2.5f, iconW - 2.4f, 7.5f, 1.2f);
        g.strokePath (body, juce::PathStrokeType (1.3f));

        if (! iconOnly)
        {
            g.setFont (font);
            g.drawText (label, juce::Rectangle<float> (x + iconW + 6.0f, r.getY(), textW + 2.0f, r.getHeight()),
                        juce::Justification::centredLeft, false);
        }
    }
};

// Invisible, click-through child that exposes one painted block to accessibility clients.
class AccessibleItem : public juce::Component
{
public:
    AccessibleItem()
    {
        setInterceptsMouseClicks (false, false);
        noFocus (*this);
        setAccessible (true);
    }

    std::function<void()> onPress, onMenu;

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        juce::AccessibilityActions actions;
        actions.addAction (juce::AccessibilityActionType::press, [this] { if (onPress != nullptr) onPress(); });
        actions.addAction (juce::AccessibilityActionType::showMenu, [this] { if (onMenu != nullptr) onMenu(); });
        return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::button, std::move (actions));
    }
};

struct Block
{
    cb::ProgressionSlot slot;
    juce::String name, displayName, roman, length;
    juce::Colour colour;
    juce::Rectangle<float> bounds;
    juce::Font nameFont { juce::FontOptions() };
    juce::Font romanFont { juce::FontOptions() };
    bool showName = true, showRoman = true, showLength = true;
    float textInset = 6.0f;
    float flash = 0.0f;
};

//==============================================================================
// "Change to": same-root variants of a chord, split into the ones that fit the key and a few spicier ones.
struct ChangeOption
{
    cb::ChordQuality quality;
    bool inKey;
};

std::vector<ChangeOption> changeToOptions (const cb::Chord& chord, const cb::Key& key)
{
    using Q = cb::ChordQuality;
    const auto q = chord.quality;
    const bool dimFamily = q == Q::Diminished || q == Q::HalfDiminished7 || q == Q::Diminished7;
    const auto& iv = cb::qualityIntervals (q);
    const auto has = [&iv] (int x) { return std::find (iv.begin(), iv.end(), x) != iv.end(); };

    bool minorThird = ! dimFamily && has (3);
    const bool majorThird = has (4);

    if (! dimFamily && ! minorThird && ! majorThird)   // sus chords: follow the key's third above the root
        minorThird = key.contains (cb::pc (chord.root + 3)) && ! key.contains (cb::pc (chord.root + 4));

    std::vector<Q> candidates;

    if (dimFamily)
        candidates = { Q::Diminished, Q::HalfDiminished7, Q::Diminished7 };
    else if (minorThird)
        candidates = { Q::Minor, Q::Minor7, Q::MinorAdd9, Q::Minor9, Q::Minor11, Q::Minor6, Q::MinorMajor7, Q::Sus2, Q::Sus4 };
    else
        candidates = { Q::Major, Q::Major7, Q::Add9, Q::Major9, Q::Major6, Q::Major69, Q::Major7Sharp11,
                       Q::Dominant7, Q::Dominant9, Q::Dominant13, Q::Dominant7Flat9, Q::Sus2, Q::Sus4, Q::Dominant7Sus4 };

    if (std::find (candidates.begin(), candidates.end(), q) == candidates.end())
        candidates.insert (candidates.begin(), q);

    const auto parallel = key.parallel();
    // Tones that "belong": the key, its parallel key and (for chromatic chords such as secondary dominants) the
    // chord's own tones, so A7 in C major can still become A or A7b9.
    const auto ownTones = chord.pitchClasses();
    const auto fitsKeyOrParallel = [&] (const cb::Chord& c)
    {
        for (int pcl : c.pitchClasses())
            if (! key.contains (pcl) && ! parallel.contains (pcl)
                && std::find (ownTones.begin(), ownTones.end(), pcl) == ownTones.end())
                return false;
        return true;
    };

    const auto isDominantType = [] (Q x)
    {
        return x == Q::Dominant7 || x == Q::Dominant9 || x == Q::Dominant13 || x == Q::Dominant7Flat9 || x == Q::Dominant7Sus4;
    };

    // Dominant colours make sense where a dominant 7th on this root actually functions as one (V, V/x) or when
    // the chord already is a dominant; maj7#11 on chords whose maj7 belongs to the key (or its parallel).
    const cb::Chord dom7 { chord.root, Q::Dominant7 };
    const auto domFunction = cb::harmonicFunction (dom7, key);
    const bool dominantSensible = isDominantType (q) || domFunction == cb::HarmonicFunction::Dominant
                                  || domFunction == cb::HarmonicFunction::Secondary;
    const bool lydianSensible = fitsKeyOrParallel ({ chord.root, Q::Major7 });

    std::vector<ChangeOption> out;

    for (auto cand : candidates)
    {
        const cb::Chord c { chord.root, cand };
        const bool inKey = cb::isDiatonic (c, key);

        if (! inKey && cand != q)
        {
            if (isDominantType (cand) && ! dominantSensible)
                continue;

            if (cand == Q::Major7Sharp11 && ! lydianSensible)
                continue;

            if (cand == Q::MinorMajor7 && key.degreeOf (chord.root) != 0)
                continue;

            // Other outside colours only when they at least belong to the parallel key (borrowed sound); Dorian
            // m6 and the dominant colours above are the deliberate exceptions.
            if (! isDominantType (cand) && cand != Q::Minor6 && cand != Q::MinorMajor7 && ! fitsKeyOrParallel (c))
                continue;
        }

        out.push_back ({ cand, inKey });
    }

    std::stable_partition (out.begin(), out.end(), [] (const ChangeOption& o) { return o.inKey; });
    return out;
}
} // namespace

//==============================================================================
// The Impl is the full-size child surface: it paints the lane, owns the buttons and handles all interaction.
struct ProgressionLane::Impl : public juce::Component,
                               public juce::TooltipClient,
                               private juce::Timer
{
    Impl (ProgressionLane& o, ChordbloomProcessor& p) : owner (o), processor (p)
    {
        setOpaque (false);
        noFocus (*this);
        setTitle ("Progression");
        setDescription ("The chord progression timeline");

        for (juce::Component* c : { (juce::Component*) &playButton, (juce::Component*) &clearButton,
                                    (juce::Component*) &sparkButton, (juce::Component*) &presetsButton })
            addAndMakeVisible (c);

        for (juce::Component* c : { (juce::Component*) &moveButton, (juce::Component*) &hostButton,
                                    (juce::Component*) &emptySparkButton, (juce::Component*) &emptyPresetsButton })
            addChildComponent (c);

        playButton.onClick = [this]
        {
            processor.setLocalPlayback (! processor.isLocalPlaybackOn());
            playButton.setToggleState (processor.isLocalPlaybackOn(), juce::dontSendNotification);
            pollTransport();
        };

        clearButton.onClick = [this] { clearAll(); };

        // Spark: click = new progression, chevron / right-click = settings
        sparkButton.text = "Spark";
        sparkButton.chevron = true;
        sparkButton.splitChevron = true;
        sparkButton.onPrimary = [this] { spark(); };
        sparkButton.onMenu = [this] { showSparkMenu (sparkButton); };

        presetsButton.text = "Presets";
        presetsButton.chevron = true;
        presetsButton.onMenu = [this] { showPresetsMenu (presetsButton); };
        presetsButton.setTooltip ("Ready-made progressions for this key, grouped by mood (replaces the lane; undo brings it back)");

        moveButton.onPrimary = [this] { transposeBy (moveShift); };
        moveButton.onMenu = [this] { showLaneMenu (&moveButton); };

        hostButton.text = "Playing with FL";
        hostButton.onPrimary = [this] { stopPlayingWithHost(); };
        hostButton.onMenu = hostButton.onPrimary;
        hostButton.setTooltip ("Chordbloom is looping the progression along with FL's transport (Play with FL is on).\n"
                               "Already dropped the clip into FL? Click to stop Chordbloom playing along, so you don't hear it twice.");

        emptySparkButton.text = "Spark a progression";
        emptySparkButton.fontSize = 12.5f;
        emptySparkButton.onPrimary = [this] { spark(); };
        emptySparkButton.onMenu = [this] { showSparkMenu (emptySparkButton); };

        emptyPresetsButton.text = "Presets";
        emptyPresetsButton.fontSize = 12.5f;
        emptyPresetsButton.chevron = true;
        emptyPresetsButton.onMenu = [this] { showPresetsMenu (emptyPresetsButton); };
        emptyPresetsButton.setTooltip (presetsButton.getTooltip());

        updateSparkLabels();
        sync();
        startTimerHz (idleHz);
    }

    ~Impl() override
    {
        stopTimer();

        if (previewing)
            processor.stopPreview();
    }

    //==============================================================================
    // Model sync
    // `synced` is what selection / flashes / buttons were last reconciled with; `cached` is what the blocks are
    // laid out from (paint() may refresh the layout on its own, but only sync() touches state).
    bool outOfDate() const
    {
        return processor.getProgression() != synced || processor.getKey() != syncedKey;
    }

    void sync()
    {
        const auto& prog = processor.getProgression();
        const bool changed = prog != synced || processor.getKey() != syncedKey;

        if (changed)
        {
            // Flash a slot that just appeared (append, insert, duplicate).
            int inserted = -1;

            if (prog.slots.size() == synced.slots.size() + 1)
            {
                size_t i = 0;

                while (i < synced.slots.size() && prog.slots[i] == synced.slots[i])
                    ++i;

                // Only a genuine single insertion (everything after it shifted by one) counts.
                if (std::equal (synced.slots.begin() + (std::ptrdiff_t) i, synced.slots.end(), prog.slots.begin() + (std::ptrdiff_t) i + 1))
                    inserted = (int) i;
            }

            synced = prog;
            syncedKey = processor.getKey();
            const int n = (int) prog.slots.size();

            // Any pending press refers to the old progression (a host state load, undo, a second touch...):
            // cancel it so a later drag can never index past the new blocks or grab a different chord.
            cancelPress();

            if (resizeSlot >= n)
                resizeSlot = -1;

            if (dragSourceSlot >= n)
                dragSourceSlot = -1;

            layoutBlocks();

            if (inserted >= 0 && inserted < (int) blocks.size())
            {
                blocks[(size_t) inserted].flash = 1.0f;
                speedUp();
            }

            if (selected >= n)
                setSelected (-1, true);

            if (hoverSlot >= n)
                hoverSlot = -1;

            moveShift = n > 0 ? processor.bestTranspositionForKey() : 0;
            updateMoveButton();
            layoutHeader();
            layoutEmptyState();
            syncProxies();
            repaint();
        }

        clearButton.setEnabled (! prog.empty());
        updatePlayButtonEnablement();
    }

    // B5: while FL plays and Play with FL is on, Chordbloom follows FL's transport and its own Play has no effect
    // (FL's Stop also stops it), so the button is disabled with an explanation instead of lighting up silently.
    void updatePlayButtonEnablement()
    {
        const bool local = processor.isLocalPlaybackOn();
        const bool hostDriven = processor.isHostPlaying() && syncToHostOn();
        const bool wantEnabled = (! processor.getProgression().empty() || local) && ! hostDriven;

        if (playButton.isEnabled() != wantEnabled)
            playButton.setEnabled (wantEnabled);

        if (hostDriven != playButtonHostDriven)
        {
            playButtonHostDriven = hostDriven;
            playButton.setTooltip (hostDriven ? "FL is playing: Chordbloom follows FL's transport (Play with FL is on).\n"
                                                "Stop FL to loop the progression here, or click \"Playing with FL\" to mute it."
                                              : "Play / stop the progression loop");
        }
    }

    void cancelPress()
    {
        if (pressedSlot >= 0 && previewing)
        {
            processor.stopPreview();
            previewing = false;
        }

        pressedSlot = -1;
    }

    void setSelected (int index, bool notify)
    {
        index = index >= 0 && index < (int) cached.slots.size() ? index : -1;

        if (index == selected)
            return;

        repaintSelectionMarker();
        repaintBlock (selected);
        selected = index;
        repaintBlock (selected);
        repaintSelectionMarker();

        if (notify && owner.onSelectionChanged != nullptr)
            owner.onSelectionChanged (selected);
    }

    void flashAll()
    {
        for (auto& b : blocks)
            b.flash = 1.0f;

        speedUp();
        repaint();
    }

    //==============================================================================
    // Layout
    void resized() override
    {
        auto area = getLocalBounds().reduced (12, 8);
        compact = area.getHeight() < 104;

        auto head = area.removeFromTop (compact ? 28 : 32);
        playButton.setBounds (head.removeFromLeft (head.getHeight()));
        head.removeFromLeft (10);
        headerRow = head;

        area.removeFromTop (compact ? 4 : 6);
        rulerArea = area.removeFromTop (compact ? 14 : 16).toFloat();
        area.removeFromTop (3);
        trackArea = area.toFloat();

        layoutBlocks();
        layoutHeader();
        layoutEmptyState();
        syncProxies();
    }

    // Left part of the header: "PROGRESSION  4 bars  · 4 chords"
    struct HeaderText
    {
        juce::String title, readout, detail;
        juce::Font titleFont { juce::FontOptions() }, readoutFont { juce::FontOptions() }, detailFont { juce::FontOptions() };
        float titleW = 0, readoutW = 0, detailW = 0;

        float width() const { return titleW + 12.0f + readoutW + (detailW > 0.0f ? 8.0f + detailW : 0.0f); }
    };

    HeaderText headerText() const
    {
        HeaderText t;
        t.title = "PROGRESSION";
        t.titleFont = cb::ui::font (10.5f, 600).withExtraKerningFactor (0.12f);
        t.readoutFont = cb::ui::font (13.0f, 600);
        t.detailFont = cb::ui::font (12.0f, 400);

        const int n = (int) cached.slots.size();
        t.readout = n == 0 ? juce::String ("Empty") : totalText (effectiveTotal());

        if (n > 0)
            t.detail << middleDot() << "  " << n << (n == 1 ? " chord" : " chords");

        t.titleW = widthOf (t.titleFont, t.title) + 2.0f;
        t.readoutW = widthOf (t.readoutFont, t.readout) + 2.0f;
        t.detailW = t.detail.isNotEmpty() ? widthOf (t.detailFont, t.detail) + 2.0f : 0.0f;
        return t;
    }

    juce::String hintText() const
    {
        // B4: FL's pattern loop is shorter than the progression; Chordbloom keeps going across it - say so.
        if (hostLoopBeats > 0.0 && ! cached.slots.empty())
            return "FL loops every " + totalText (juce::roundToInt (hostLoopBeats)) + "  " + middleDot()
                   + "  the progression keeps going";

        return cached.slots.empty() ? juce::String ("Click a pad to hear it, double-click to add it")
                                    : juce::String ("Scroll a chord to change its length") + "  " + middleDot() + "  right-click for more";
    }

    void updateMoveButton()
    {
        const bool show = moveShift != 0 && ! cached.slots.empty();

        if (show)
        {
            const auto keyName = shortKeyName (processor.getKey());
            moveFullText = "Move chords to " + keyName + " (" + signedSemitones (moveShift) + ")";
            moveShortText = "To " + keyName + " (" + signedSemitones (moveShift) + ")";
            moveButton.setTitle (moveFullText);
            moveButton.setTooltip ("Most of these chords sit outside " + toJuce (processor.getKey().name()) + ".\n"
                                   "Transpose the whole progression by " + signedSemitones (moveShift)
                                   + " semitones so it fits the key (undo brings it back).");
        }

        moveButton.setVisible (show);
    }

    void updateSparkLabels()
    {
        const auto s = loadSparkSettings();
        shownSpark = s;
        const juce::String mood (cb::moodName (s.mood));
        sparkButton.detail = mood;
        emptySparkButton.detail = {};

        juce::String tip;
        tip << "Spark: generate a fresh " << mood << " progression (" << s.numChords << " chords, "
            << lengthText (s.beatsPerChord) << " each). It replaces the lane; undo brings the old one back.\n"
            << "Click the arrow or right-click for mood, length and rhythm.";
        sparkButton.setTooltip (tip);
        emptySparkButton.setTooltip (tip);
        sparkButton.setTitle ("Spark a " + mood + " progression");
        emptySparkButton.setTitle (sparkButton.getTitle());
        sparkButton.repaint();
        emptySparkButton.repaint();
    }

    // Places the header pills right-to-left and decides how much text fits (pills shrink before anything clips).
    void layoutHeader()
    {
        if (headerRow.isEmpty())
            return;

        auto head = headerRow;
        const int pillH = std::min (kPillH, head.getHeight());
        const auto text = headerText();
        const bool showMove = moveButton.isVisible();
        const bool showHost = hostButton.isVisible();

        const int clearFull = 74, clearCompact = 34;

        struct Level { bool sparkDetail, moveFull, presetsText, sparkText, hostText, clearText; };
        const Level levels[] = {
            { true,  true,  true,  true,  true,  true  },
            { false, true,  true,  true,  true,  true  },
            { false, false, true,  true,  true,  true  },
            { false, false, true,  true,  false, true  },
            { false, false, false, true,  false, true  },
            { false, false, false, false, false, true  },
            { false, false, false, false, false, false },
        };

        const float available = (float) head.getWidth();
        Level chosen = levels[std::size (levels) - 1];
        int sparkW = 0, presetsW = 0, moveW = 0, hostW = 0, clearW = 0;

        for (const auto& level : levels)
        {
            sparkW = sparkButton.idealWidth (! level.sparkText, level.sparkDetail, sparkButton.text);
            presetsW = presetsButton.idealWidth (! level.presetsText, false, presetsButton.text);
            moveW = showMove ? moveButton.idealWidth (false, false, level.moveFull ? moveFullText : moveShortText) : 0;
            hostW = showHost ? hostButton.idealWidth (! level.hostText, false, hostButton.text) : 0;
            clearW = level.clearText ? clearFull : clearCompact;

            const float needed = text.width() + (showHost ? 10.0f + (float) hostW : 0.0f) + 16.0f
                                 + (showMove ? (float) moveW + 8.0f : 0.0f) + (float) sparkW + 8.0f + (float) presetsW + 8.0f
                                 + (float) clearW;
            chosen = level;

            if (needed <= available)
                break;
        }

        sparkButton.compact = ! chosen.sparkText;
        sparkButton.showDetail = chosen.sparkDetail;
        presetsButton.compact = ! chosen.presetsText;
        hostButton.compact = ! chosen.hostText;
        moveButton.text = chosen.moveFull ? moveFullText : moveShortText;

        auto placeRight = [&head, pillH] (juce::Component& c, int w)
        {
            w = std::min (w, std::max (0, head.getWidth()));
            c.setBounds (head.removeFromRight (w).withSizeKeepingCentre (w, pillH));
            head.removeFromRight (8);
        };

        placeRight (clearButton, clearW);
        placeRight (presetsButton, presetsW);
        placeRight (sparkButton, sparkW);

        if (showMove)
            placeRight (moveButton, moveW);

        // Left text, then the host pill right after it.
        float x = (float) head.getX() + text.width();

        if (showHost)
        {
            const int hx = (int) std::ceil (x + 10.0f);
            const int w = std::min (hostW, std::max (0, head.getRight() - hx));
            hostButton.setBounds (juce::Rectangle<int> (hx, head.getY(), w, head.getHeight()).withSizeKeepingCentre (w, pillH));
            x = (float) hostButton.getRight();
        }

        hintArea = juce::Rectangle<float> (x + 16.0f, (float) head.getY(), (float) head.getRight() - (x + 16.0f),
                                           (float) head.getHeight());

        for (auto* b : { &sparkButton, &presetsButton, &moveButton, &hostButton })
            b->repaint();

        repaint (headerRow.withLeft (0));
    }

    // Empty lane: hint text plus Spark / Presets buttons, centred together in the track.
    void layoutEmptyState()
    {
        const bool show = cached.slots.empty() && ! dropActive && ! trackArea.isEmpty();
        emptySparkButton.setVisible (show);
        emptyPresetsButton.setVisible (show);

        if (! show)
            return;

        const auto r = trackArea.reduced (3.0f);
        const int h = (int) std::min (28.0f, r.getHeight() - 8.0f);
        const int sparkW = emptySparkButton.idealWidth (false, false, emptySparkButton.text);
        const int presetsW = emptyPresetsButton.idealWidth (false, false, emptyPresetsButton.text);
        const float buttonsW = (float) (sparkW + 8 + presetsW);

        emptyText = "Drag chords here or double-click a pad";
        emptyFont = cb::ui::font (juce::jlimit (12.0f, 14.0f, r.getHeight() * 0.22f), 600);
        float textW = widthOf (emptyFont, emptyText);
        const float icon = 16.0f, sepW = 30.0f;
        float total = icon + 8.0f + textW + sepW + buttonsW;

        if (total > r.getWidth() - 24.0f)
        {
            emptyText = "Drag or double-click chords";
            textW = widthOf (emptyFont, emptyText);
            total = icon + 8.0f + textW + sepW + buttonsW;
        }

        showEmptyText = total <= r.getWidth() - 24.0f;

        if (! showEmptyText)
            total = buttonsW;

        float x = std::round (r.getCentreX() - total * 0.5f);
        const int cy = (int) std::round (r.getCentreY());

        if (showEmptyText)
        {
            emptyTextArea = { x, r.getY(), icon + 8.0f + textW + 4.0f, r.getHeight() };
            x += icon + 8.0f + textW + sepW;
        }

        emptySparkButton.setBounds ((int) x, cy - h / 2, sparkW, h);
        emptyPresetsButton.setBounds ((int) x + sparkW + 8, cy - h / 2, presetsW, h);
    }

    int lengthOf (int index) const
    {
        if (index == resizeSlot)
            return resizeLength;

        return cached.slots[(size_t) index].lengthBeats;
    }

    int effectiveTotal() const
    {
        int total = 0;

        for (int i = 0; i < (int) cached.slots.size(); ++i)
            total += lengthOf (i);

        return total;
    }

    double startBeatOf (int index) const
    {
        double beat = 0.0;

        for (int i = 0; i < index && i < (int) cached.slots.size(); ++i)
            beat += lengthOf (i);

        return beat;
    }

    float beatToX (double beat) const { return trackArea.getX() + (float) beat * pxPerBeat; }

    void layoutBlocks()
    {
        cached = processor.getProgression();
        key = processor.getKey();

        if (resizeSlot >= (int) cached.slots.size())
            resizeSlot = -1;

        const int total = effectiveTotal();
        visibleBeats = std::max (kMinVisibleBeats, roundUpToBar (total));

        // While an edge is being dragged the scale stays put so the edge tracks the mouse exactly
        // (blocks that grow past the end are clipped until the mouse is released).
        if (resizeSlot >= 0)
            visibleBeats = frozenVisibleBeats;

        pxPerBeat = trackArea.getWidth() / (float) visibleBeats;

        std::vector<Block> newBlocks (cached.slots.size());
        const float trackH = trackArea.getHeight();
        const float nameH = juce::jlimit (12.0f, 18.0f, trackH * 0.25f);
        const float romanH = juce::jlimit (10.0f, 12.0f, trackH * 0.18f);

        double beat = 0.0;

        for (size_t i = 0; i < cached.slots.size(); ++i)
        {
            auto& b = newBlocks[i];
            b.slot = cached.slots[i];
            b.slot.lengthBeats = lengthOf ((int) i);

            if (i < blocks.size() && blocks[i].slot.chord == b.slot.chord)
                b.flash = blocks[i].flash;

            const auto& chord = b.slot.chord;
            b.name = toJuce (chord.name (key));

            if (b.slot.inversion > 0 && b.slot.inversion < chord.numTones())
                b.name << "/" << toJuce (cb::spellChordTone (chord, chord.intervals()[(size_t) b.slot.inversion], key));

            b.roman = toJuce (cb::romanNumeral (chord, key));
            b.colour = cb::ui::functionColour (cb::harmonicFunction (chord, key));
            b.length = lengthText (b.slot.lengthBeats);

            if (b.slot.octaveShift != 0)
                b.length << "  " << middleDot() << "  " << (b.slot.octaveShift > 0 ? "+" : "") << b.slot.octaveShift << " oct";

            const float x0 = std::round (beatToX (beat)) + 1.0f;
            beat += b.slot.lengthBeats;
            const float x1 = std::round (beatToX (beat)) - 1.0f;
            b.bounds = { x0, trackArea.getY(), std::max (2.0f, x1 - x0), trackH };

            // Narrow blocks fall back to shorter names (no slash bass, then just the root) instead of eliding.
            b.textInset = b.bounds.getWidth() < 52.0f ? 3.0f : 6.0f;
            const float textW = b.bounds.getWidth() - 2.0f * b.textInset;
            b.showName = false;

            for (const auto& candidate : { b.name, toJuce (chord.name (key)), toJuce (cb::spellChordTone (chord, 0, key)) })
            {
                const auto f = fittedFont (nameH, 600, candidate, textW, 9.0f);

                if (widthOf (f, candidate) <= textW + 0.5f)
                {
                    b.displayName = candidate;
                    b.nameFont = f;
                    b.showName = true;
                    break;
                }
            }

            b.romanFont = fittedFont (romanH, 400, b.roman, textW, 8.5f);
            b.showRoman = b.showName && trackH >= 38.0f && widthOf (b.romanFont, b.roman) <= textW + 0.5f;
            b.showLength = trackH >= 50.0f && widthOf (cb::ui::font (10.5f, 400), b.length) <= b.bounds.getWidth() - 14.0f;
        }

        blocks = std::move (newBlocks);
    }

    // Accessibility: one invisible, click-through proxy per block (never touched from paint()).
    void syncProxies()
    {
        while (proxies.size() > blocks.size())
            proxies.pop_back();

        while (proxies.size() < blocks.size())
        {
            proxies.push_back (std::make_unique<AccessibleItem>());
            addAndMakeVisible (*proxies.back());
            proxies.back()->toBack();
        }

        const int n = (int) blocks.size();

        for (int i = 0; i < n; ++i)
        {
            const auto& b = blocks[(size_t) i];
            auto& proxy = *proxies[(size_t) i];
            proxy.setBounds (b.bounds.getSmallestIntegerContainer());
            proxy.setTitle (b.name + " (" + b.roman + "), " + b.length);
            proxy.setDescription ("Chord " + juce::String (i + 1) + " of " + juce::String (n));
            proxy.setHelpText ("Press to hear it; open the menu to change, resize or remove it");

            juce::Component::SafePointer<Impl> safe (this);
            proxy.onPress = [safe, i]
            {
                if (safe == nullptr || i >= (int) safe->processor.getProgression().slots.size())
                    return;

                safe->processor.startPreviewSlot (i);
                juce::Timer::callAfterDelay (700, [safe]
                {
                    if (safe != nullptr && ! safe->previewing)
                        safe->processor.stopPreview();
                });
            };
            proxy.onMenu = [safe, i]
            {
                if (safe != nullptr)
                    safe->showSlotMenu (i);
            };
        }
    }

    //==============================================================================
    // Painting
    void paint (juce::Graphics& g) override
    {
        if (processor.getProgression() != cached || processor.getKey() != key)   // defensive: never paint stale blocks
            layoutBlocks();

        paintCard (g);
        paintHeader (g);
        paintRuler (g);
        paintTrack (g);

        {
            juce::Graphics::ScopedSaveState save (g);

            if (resizeSlot >= 0)
                g.reduceClipRegion (getLocalBounds().withRight (juce::roundToInt (trackArea.getRight())));

            for (int i = 0; i < (int) blocks.size(); ++i)
                if (g.clipRegionIntersects (blocks[(size_t) i].bounds.expanded (6.0f).getSmallestIntegerContainer()))
                    paintBlock (g, i, blocks[(size_t) i], blocks[(size_t) i].bounds, false);
        }

        if (blocks.empty())
            paintEmptyState (g);

        paintSelectionMarker (g);
        paintPlayhead (g);
        paintDropCaret (g);
    }

    void paintCard (juce::Graphics& g) const
    {
        const auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setGradientFill (juce::ColourGradient::vertical (Col::panel.brighter (0.035f), r.getY(), Col::panel, r.getBottom()));
        g.fillRoundedRectangle (r, cb::ui::kCornerRadius);
        g.setColour (Col::outline.withAlpha (0.55f));
        g.drawRoundedRectangle (r, cb::ui::kCornerRadius, 1.0f);
    }

    void paintHeader (juce::Graphics& g) const
    {
        auto r = headerRow.toFloat();
        const auto t = headerText();

        g.setFont (t.titleFont);
        g.setColour (Col::textDim);
        g.drawText (t.title, r.removeFromLeft (t.titleW), juce::Justification::centredLeft, false);
        r.removeFromLeft (12.0f);

        const bool empty = cached.slots.empty();
        g.setFont (t.readoutFont);
        g.setColour (empty ? Col::textFaint : Col::text);
        g.drawText (t.readout, r.removeFromLeft (t.readoutW), juce::Justification::centredLeft, false);

        if (t.detail.isNotEmpty())
        {
            r.removeFromLeft (8.0f);
            g.setFont (t.detailFont);
            g.setColour (Col::textDim);
            g.drawText (t.detail, r.removeFromLeft (std::min (t.detailW, std::max (0.0f, r.getWidth()))),
                        juce::Justification::centredLeft, true);
        }

        // Discoverability hint, only when there is room for it.
        const auto hint = hintText();
        const auto hintFont = cb::ui::font (11.5f, 400);

        if (widthOf (hintFont, hint) + 8.0f < hintArea.getWidth())
        {
            g.setFont (hintFont);
            g.setColour (Col::textFaint);
            g.drawText (hint, hintArea, juce::Justification::centredRight, false);
        }
    }

    void paintRuler (juce::Graphics& g) const
    {
        const auto r = rulerArea;
        const int total = effectiveTotal();

        // Loop region
        if (total > 0)
        {
            auto loop = juce::Rectangle<float> (trackArea.getX(), r.getBottom() - 3.0f, (float) total * pxPerBeat, 3.0f);
            g.setColour (Col::accent.withAlpha (0.28f));
            g.fillRoundedRectangle (loop, 1.5f);
        }

        const auto numberFont = cb::ui::font (10.5f, 600);
        const float barW = pxPerBeat * 4.0f;
        const int barStep = barW >= 22.0f ? 1 : (barW >= 11.0f ? 2 : 4);

        for (int beat = 0; beat <= visibleBeats; ++beat)
        {
            const bool isBar = beat % 4 == 0;
            const float x = std::round (beatToX (beat));
            const float h = isBar ? 7.0f : 3.0f;

            if (! isBar && pxPerBeat < 5.0f)
                continue;

            g.setColour (isBar ? Col::textFaint : Col::textFaint.withAlpha (0.55f));
            g.fillRect (juce::Rectangle<float> (beat == visibleBeats ? x - 1.0f : x, r.getBottom() - 3.0f - h, 1.0f, h));

            const int bar = beat / 4;

            if (isBar && beat < visibleBeats && bar % barStep == 0)
            {
                g.setFont (numberFont);
                g.setColour (beat < total ? Col::textDim : Col::textFaint);
                g.drawText (juce::String (bar + 1), juce::Rectangle<float> (x + 4.0f, r.getY() - 1.0f, 28.0f, r.getHeight() - 3.0f),
                            juce::Justification::centredLeft, false);
            }
        }
    }

    void paintTrack (juce::Graphics& g) const
    {
        const auto r = trackArea;
        const int total = effectiveTotal();

        g.setColour (Col::background.withAlpha (0.65f));
        g.fillRoundedRectangle (r, kBlockRadius);

        {
            juce::Graphics::ScopedSaveState save (g);
            juce::Path clip;
            clip.addRoundedRectangle (r, kBlockRadius);
            g.reduceClipRegion (clip);

            // Alternate bar shading + faint beat lines
            for (int bar = 0; bar * 4 < visibleBeats; ++bar)
            {
                const float x0 = std::round (beatToX (bar * 4));
                const float x1 = std::round (beatToX (std::min (visibleBeats, bar * 4 + 4)));

                if (bar % 2 == 1)
                {
                    g.setColour (juce::Colours::white.withAlpha (0.018f));
                    g.fillRect (juce::Rectangle<float> (x0, r.getY(), x1 - x0, r.getHeight()));
                }

                g.setColour (Col::outline.withAlpha (0.55f));
                g.fillRect (juce::Rectangle<float> (x0, r.getY(), 1.0f, r.getHeight()));

                if (pxPerBeat >= 8.0f)
                {
                    g.setColour (Col::outline.withAlpha (0.22f));

                    for (int b = 1; b < 4 && bar * 4 + b < visibleBeats; ++b)
                        g.fillRect (juce::Rectangle<float> (std::round (beatToX (bar * 4 + b)), r.getY(), 1.0f, r.getHeight()));
                }
            }

            // Outside the loop: dimmed
            if (total > 0 && total < visibleBeats)
            {
                const float x = std::round (beatToX (total));
                g.setColour (Col::background.withAlpha (0.35f));
                g.fillRect (juce::Rectangle<float> (x, r.getY(), r.getRight() - x, r.getHeight()));
                g.setColour (Col::accent.withAlpha (0.35f));
                g.fillRect (juce::Rectangle<float> (x, r.getY(), 1.0f, r.getHeight()));
            }
        }

        g.setColour (Col::outline.withAlpha (0.5f));
        g.drawRoundedRectangle (r.reduced (0.5f), kBlockRadius, 1.0f);
    }

    void paintEmptyState (juce::Graphics& g) const
    {
        const auto r = trackArea.reduced (3.0f);

        juce::Path outline;
        outline.addRoundedRectangle (r, kBlockRadius - 1.0f);
        juce::Path dashed;
        const float dashes[] = { 5.0f, 4.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);
        g.setColour (Col::textFaint.withAlpha (dropActive ? 0.9f : 0.45f));
        g.fillPath (dashed);

        if (dropActive || ! showEmptyText)
            return;

        const float icon = 16.0f;
        const float x = emptyTextArea.getX();
        const float cy = r.getCentreY();

        // Plus-in-circle icon
        const auto circle = juce::Rectangle<float> (icon, icon).withCentre ({ x + icon * 0.5f, cy });
        g.setColour (Col::accent.withAlpha (0.8f));
        g.drawEllipse (circle.reduced (0.75f), 1.3f);
        g.fillRoundedRectangle (juce::Rectangle<float> (7.0f, 1.4f).withCentre (circle.getCentre()), 0.7f);
        g.fillRoundedRectangle (juce::Rectangle<float> (1.4f, 7.0f).withCentre (circle.getCentre()), 0.7f);

        g.setFont (emptyFont);
        g.setColour (Col::textDim);
        g.drawText (emptyText, juce::Rectangle<float> (x + icon + 8.0f, r.getY(), emptyTextArea.getWidth() - icon - 8.0f, r.getHeight()),
                    juce::Justification::centredLeft, false);

        // "or" between the hint and the buttons
        if (emptySparkButton.isVisible())
        {
            const float gapL = emptyTextArea.getRight(), gapR = (float) emptySparkButton.getX();
            g.setFont (cb::ui::font (11.5f, 400));
            g.setColour (Col::textFaint);
            g.drawText ("or", juce::Rectangle<float> (gapL, r.getY(), gapR - gapL, r.getHeight()), juce::Justification::centred, false);
        }
    }

    void paintBlock (juce::Graphics& g, int index, const Block& b, juce::Rectangle<float> r, bool forDragImage) const
    {
        const bool isSelected = ! forDragImage && index == selected;
        const bool isPlaying = ! forDragImage && playing && index == playingSlot;
        const bool isHovered = ! forDragImage && index == hoverSlot;
        const bool isDragSource = ! forDragImage && index == dragSourceSlot;
        const auto colour = b.colour;

        if (isDragSource)
        {
            // Leave a ghost where the block was picked up
            juce::Path outline;
            outline.addRoundedRectangle (r.reduced (0.5f), kBlockRadius);
            juce::Path dashed;
            const float dashes[] = { 4.0f, 3.0f };
            juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);
            g.setColour (colour.withAlpha (0.08f));
            g.fillRoundedRectangle (r, kBlockRadius);
            g.setColour (colour.withAlpha (0.5f));
            g.fillPath (dashed);
            return;
        }

        if (isPlaying)
            drawHalo (g, r, kBlockRadius, colour, 0.32f, 4);

        if (b.flash > 0.0f)
            drawHalo (g, r, kBlockRadius, Col::accent, 0.45f * b.flash, 4);

        auto base = isHovered ? Col::panelHover : Col::panelRaised;

        if (isSelected || isPlaying)
            base = base.brighter (0.05f);

        g.setGradientFill (juce::ColourGradient::vertical (base.brighter (0.05f), r.getY(), base.darker (0.08f), r.getBottom()));
        g.fillRoundedRectangle (r, kBlockRadius);

        const float tint = isPlaying ? 0.30f : (isSelected ? 0.21f : 0.14f + (isHovered ? 0.04f : 0.0f));
        g.setGradientFill (juce::ColourGradient::vertical (colour.withAlpha (tint * 1.15f), r.getY(),
                                                           colour.withAlpha (tint * 0.7f), r.getBottom()));
        g.fillRoundedRectangle (r, kBlockRadius);

        {
            juce::Graphics::ScopedSaveState save (g);
            juce::Path clip;
            clip.addRoundedRectangle (r, kBlockRadius);
            g.reduceClipRegion (clip);

            // Progress through the playing block
            if (isPlaying)
            {
                const float px = juce::jlimit (r.getX(), r.getRight(), beatToX (playBeat));
                g.setColour (colour.withAlpha (0.16f));
                g.fillRect (juce::Rectangle<float> (r.getX(), r.getY(), px - r.getX(), r.getHeight()));
            }

            g.setColour (colour.withAlpha (isPlaying || isSelected ? 1.0f : 0.85f));
            g.fillRect (r.withHeight (kTopBar));
        }

        // Outline
        if (isSelected)
        {
            g.setColour (Col::accent);
            g.drawRoundedRectangle (r.reduced (0.75f), kBlockRadius - 0.5f, 1.5f);
        }
        else
        {
            g.setColour (isPlaying ? colour.withAlpha (0.8f)
                                   : Col::outline.withAlpha (0.8f).interpolatedWith (colour.withAlpha (0.55f), isHovered ? 0.7f : 0.35f));
            g.drawRoundedRectangle (r.reduced (0.5f), kBlockRadius - 0.5f, 1.0f);
        }

        // Text
        const auto content = r.withTrimmedTop (kTopBar).reduced (b.textInset, 0.0f);
        const float lengthH = b.showLength ? 16.0f : 0.0f;
        const auto textZone = content.withTrimmedBottom (lengthH * 0.6f);

        if (b.showName)
        {
            const float nameH = b.nameFont.getHeight();
            const float romanH = b.showRoman ? b.romanFont.getHeight() : 0.0f;
            const float spacing = b.showRoman ? 2.0f : 0.0f;
            const float y = std::round (textZone.getCentreY() - (nameH + spacing + romanH) * 0.5f);

            g.setFont (b.nameFont);
            g.setColour (Col::text);
            g.drawText (b.displayName, content.withY (y).withHeight (nameH), juce::Justification::centred, false);

            if (b.showRoman)
            {
                g.setFont (b.romanFont);
                g.setColour (isPlaying || isSelected ? colour.brighter (0.45f) : Col::textDim);
                g.drawText (b.roman, content.withY (y + nameH + spacing).withHeight (romanH), juce::Justification::centred, false);
            }
        }

        if (b.showLength)
        {
            g.setFont (cb::ui::font (10.5f, 400));
            g.setColour (Col::textFaint.brighter (isHovered || isSelected ? 0.35f : 0.1f));
            g.drawText (b.length, content.withTrimmedTop (content.getHeight() - lengthH).translated (0.0f, -3.0f),
                        juce::Justification::centredRight, false);
        }

        // Remove button on hover
        if (isHovered && hasCloseButton (r) && resizeSlot < 0)
        {
            const auto c = closeButtonBounds (r);
            g.setColour (hoverClose ? Col::dominant : Col::background.withAlpha (0.5f));
            g.fillEllipse (c);
            g.setColour (hoverClose ? Col::dominant : Col::outline);
            g.drawEllipse (c.reduced (0.5f), 1.0f);

            const auto centre = c.getCentre();
            const float arm = 3.0f;
            juce::Path cross;
            cross.startNewSubPath (centre.x - arm, centre.y - arm);
            cross.lineTo (centre.x + arm, centre.y + arm);
            cross.startNewSubPath (centre.x + arm, centre.y - arm);
            cross.lineTo (centre.x - arm, centre.y + arm);
            g.setColour (hoverClose ? Col::background : Col::textDim);
            g.strokePath (cross, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // Resize grip on hover
        if (isHovered && r.getWidth() >= 24.0f)
        {
            const float gx = r.getRight() - 4.5f;
            const float cy = r.getCentreY();
            g.setColour ((hoverEdge || resizeSlot == index ? Col::accent : Col::textFaint).withAlpha (0.9f));
            g.fillRoundedRectangle (gx - 1.5f, cy - 7.0f, 1.0f, 14.0f, 0.5f);
            g.fillRoundedRectangle (gx + 0.5f, cy - 7.0f, 1.0f, 14.0f, 0.5f);
        }
    }

    // Where the palette adds chords: a small notch after the selected block.
    juce::Rectangle<float> selectionMarkerBounds() const
    {
        if (selected < 0 || selected >= (int) blocks.size() || dropActive || resizeSlot >= 0)
            return {};

        const float x = juce::jlimit (trackArea.getX() + 1.0f, trackArea.getRight() - 1.0f, boundaryX (selected + 1));
        return { x - 6.0f, rulerArea.getBottom() - 8.0f, 12.0f, trackArea.getY() - rulerArea.getBottom() + 9.0f };
    }

    void repaintSelectionMarker()
    {
        const auto r = selectionMarkerBounds();

        if (! r.isEmpty())
            repaint (r.expanded (3.0f).getSmallestIntegerContainer());
    }

    void paintSelectionMarker (juce::Graphics& g) const
    {
        const auto r = selectionMarkerBounds();

        if (r.isEmpty())
            return;

        const float x = r.getCentreX();
        const float tip = trackArea.getY() + 0.5f;
        juce::Path notch;
        notch.addTriangle (x - 5.0f, tip - 6.5f, x + 5.0f, tip - 6.5f, x, tip);
        g.setColour (Col::panel);
        g.strokePath (notch, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (Col::accent);
        g.fillPath (notch);
    }

    void paintPlayhead (juce::Graphics& g) const
    {
        if (! playing)
            return;

        const float x = beatToX (playBeat);
        const float top = rulerArea.getY();
        const float bottom = trackArea.getBottom();

        g.setColour (Col::accent.withAlpha (0.16f));
        g.fillRect (juce::Rectangle<float> (x - 3.0f, top, 6.0f, bottom - top));
        g.setColour (Col::accent);
        g.fillRect (juce::Rectangle<float> (x - 0.75f, top + 2.0f, 1.5f, bottom - top - 2.0f));

        juce::Path head;
        head.addTriangle (x - 5.0f, top, x + 5.0f, top, x, top + 6.0f);
        g.fillPath (head);
    }

    float boundaryX (int index) const
    {
        if (blocks.empty())
            return trackArea.getX() + 2.0f;

        if (index <= 0)
            return blocks.front().bounds.getX() - 0.5f;

        if (index >= (int) blocks.size())
            return blocks.back().bounds.getRight() + 1.0f;

        return (blocks[(size_t) index - 1].bounds.getRight() + blocks[(size_t) index].bounds.getX()) * 0.5f;
    }

    void paintRulerPill (juce::Graphics& g, const juce::String& label, float x, juce::Colour colour) const
    {
        const auto font = cb::ui::font (11.0f, 600);
        const float w = widthOf (font, label) + 14.0f;
        auto pill = juce::Rectangle<float> (w, rulerArea.getHeight() + 2.0f).withCentre ({ x, rulerArea.getCentreY() - 1.0f });
        pill = pill.withX (juce::jlimit (getLocalBounds().toFloat().getX() + 4.0f,
                                         std::max (4.0f, (float) getWidth() - 4.0f - w), pill.getX()));
        g.setColour (colour);
        g.fillRoundedRectangle (pill, pill.getHeight() * 0.5f);
        g.setColour (Col::background);
        g.setFont (font);
        g.drawText (label, pill, juce::Justification::centred, false);
    }

    void paintDropCaret (juce::Graphics& g) const
    {
        if (! dropActive)
            return;

        // Replace: the target block lights up and previews the incoming chord.
        if (dropReplace >= 0 && dropReplace < (int) blocks.size())
        {
            const auto& b = blocks[(size_t) dropReplace];
            const auto r = b.bounds;

            drawHalo (g, r, kBlockRadius, Col::accent, 0.42f, 4);
            g.setGradientFill (juce::ColourGradient::vertical (Col::panelRaised.brighter (0.05f), r.getY(),
                                                               Col::panelRaised.darker (0.08f), r.getBottom()));
            g.fillRoundedRectangle (r, kBlockRadius);
            g.setGradientFill (juce::ColourGradient::vertical (Col::accent.withAlpha (0.22f), r.getY(),
                                                               Col::accent.withAlpha (0.10f), r.getBottom()));
            g.fillRoundedRectangle (r, kBlockRadius);
            g.setColour (Col::accent);
            g.drawRoundedRectangle (r.reduced (1.0f), kBlockRadius - 1.0f, 2.0f);

            const auto content = r.reduced (4.0f, 2.0f);
            const auto nameFont = fittedFont (juce::jlimit (12.0f, 17.0f, r.getHeight() * 0.25f), 700, dropLabel, content.getWidth(), 9.0f);
            const juce::String caption ("replaces " + b.displayName);
            const auto captionFont = cb::ui::font (10.5f, 400);
            const bool showCaption = r.getHeight() >= 38.0f && widthOf (captionFont, caption) <= content.getWidth();
            const float blockH = nameFont.getHeight() + (showCaption ? 2.0f + captionFont.getHeight() : 0.0f);
            const float y = std::round (content.getCentreY() - blockH * 0.5f);

            g.setFont (nameFont);
            g.setColour (Col::text);
            g.drawText (dropLabel, content.withY (y).withHeight (nameFont.getHeight()), juce::Justification::centred, false);

            if (showCaption)
            {
                g.setFont (captionFont);
                g.setColour (Col::accent.brighter (0.3f));
                g.drawText (caption, content.withY (y + nameFont.getHeight() + 2.0f).withHeight (captionFont.getHeight()),
                            juce::Justification::centred, false);
            }

            paintRulerPill (g, "Replace with " + dropLabel, r.getCentreX(), Col::accent);
            return;
        }

        if (dropIndex < 0)
            return;

        const bool noMove = dropFromSlot >= 0 && (dropIndex == dropFromSlot || dropIndex == dropFromSlot + 1);
        const auto colour = dropRefused ? Col::dominant : Col::accent;
        const float alpha = noMove ? 0.35f : 1.0f;
        const float x = juce::jlimit (trackArea.getX() + 1.0f, trackArea.getRight() - 1.0f, boundaryX (dropIndex));
        const float top = trackArea.getY() - 2.0f;
        const float bottom = trackArea.getBottom() + 2.0f;

        g.setColour (colour.withAlpha (0.18f * alpha));
        g.fillRoundedRectangle (juce::Rectangle<float> (x - 4.0f, top, 8.0f, bottom - top), 4.0f);
        g.setColour (colour.withAlpha (alpha));
        g.fillRoundedRectangle (juce::Rectangle<float> (x - 1.0f, top, 2.0f, bottom - top), 1.0f);
        g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ x, top }));
        g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ x, bottom }));

        // Label pill on the ruler: what is about to be inserted
        juce::String label;

        if (dropRefused)
            label = "Full (" + juce::String (cb::kMaxSlots) + " chords)";
        else if (dropFromSlot < 0)
            label = "+ " + dropLabel;

        if (label.isNotEmpty())
            paintRulerPill (g, label, x, colour);
    }

    //==============================================================================
    // Repaint helpers
    void repaintBlock (int index)
    {
        if (index >= 0 && index < (int) blocks.size())
            repaint (blocks[(size_t) index].bounds.expanded (7.0f).getSmallestIntegerContainer());
    }

    void repaintPlayheadAt (double beat)
    {
        const float x = beatToX (beat);
        repaint (juce::Rectangle<float> (x - 7.0f, rulerArea.getY() - 1.0f, 14.0f, trackArea.getBottom() - rulerArea.getY() + 2.0f)
                     .getSmallestIntegerContainer());
    }

    //==============================================================================
    // Hit testing
    struct Hit
    {
        int slot = -1;
        bool edge = false;
        bool close = false;
    };

    Hit hitTest (juce::Point<float> p) const
    {
        if (! trackArea.expanded (0.0f, 2.0f).contains (p))
            return {};

        // Right edges first so the grab zone wins over the neighbour's left side.
        for (int i = 0; i < (int) blocks.size(); ++i)
        {
            const auto& r = blocks[(size_t) i].bounds;

            if (r.getWidth() >= 14.0f && p.x >= r.getRight() - kEdgeGrab && p.x <= r.getRight() + 2.5f)
                return { i, true, false };
        }

        for (int i = 0; i < (int) blocks.size(); ++i)
        {
            const auto& r = blocks[(size_t) i].bounds;

            if (r.expanded (1.0f, 0.0f).contains (p.x, juce::jlimit (trackArea.getY(), trackArea.getBottom() - 0.01f, p.y)))
                return { i, false, hasCloseButton (r) && closeButtonBounds (r).expanded (2.0f).contains (p) };
        }

        return {};
    }

    int insertionIndexAt (float x) const
    {
        for (int i = 0; i < (int) blocks.size(); ++i)
            if (x < blocks[(size_t) i].bounds.getCentreX())
                return i;

        return (int) blocks.size();
    }

    // A chord dropped on the middle of a block replaces it; near the edges it is inserted.
    int replaceTargetAt (juce::Point<float> p) const
    {
        if (! trackArea.expanded (0.0f, 8.0f).contains (p))
            return -1;

        for (int i = 0; i < (int) blocks.size(); ++i)
        {
            const auto& r = blocks[(size_t) i].bounds;

            if (p.x >= r.getX() && p.x <= r.getRight())
            {
                const float edge = juce::jlimit (8.0f, 36.0f, r.getWidth() * 0.25f);
                return p.x > r.getX() + edge && p.x < r.getRight() - edge ? i : -1;
            }
        }

        return -1;
    }

    //==============================================================================
    // Mouse
    void updateHover (juce::Point<float> p, bool inside)
    {
        lastMouse = p;
        const auto hit = inside ? hitTest (p) : Hit {};

        if (hit.slot != hoverSlot || hit.edge != hoverEdge || hit.close != hoverClose)
        {
            repaintBlock (hoverSlot);
            hoverSlot = hit.slot;
            hoverEdge = hit.edge;
            hoverClose = hit.close;
            repaintBlock (hoverSlot);
        }

        setMouseCursor (hit.edge ? juce::MouseCursor::LeftRightResizeCursor
                                 : (hit.close ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor));
    }

    void mouseMove (const juce::MouseEvent& e) override  { updateHover (e.position, true); }
    void mouseEnter (const juce::MouseEvent& e) override { updateHover (e.position, true); }
    void mouseExit (const juce::MouseEvent& e) override
    {
        if (resizeSlot < 0)
            updateHover (e.position, false);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (outOfDate())
            sync();

        dragStarted = false;
        pressedSlot = -1;
        wheelAccum = 0.0f;
        const auto hit = hitTest (e.position);

        if (hit.slot < 0)
        {
            // Anywhere that is not a block (track gaps, ruler, header): right-click = progression menu,
            // left-click = deselect (also possible when the chords fill the whole track).
            if (e.mods.isPopupMenu())
                showLaneMenu (nullptr);
            else
                setSelected (-1, true);

            return;
        }

        if (hit.close && ! e.mods.isPopupMenu())
        {
            // Ignore the second click of a double-click so a fast double-click can't remove two blocks.
            if (e.getNumberOfClicks() == 1)
                removeSlot (hit.slot);

            updateHover (e.position, true);
            return;
        }

        setSelected (hit.slot, true);

        if (e.mods.isPopupMenu())
        {
            showSlotMenu (hit.slot);
            return;
        }

        if (hit.edge)
        {
            resizeSlot = hit.slot;
            resizeLength = cached.slots[(size_t) hit.slot].lengthBeats;
            frozenVisibleBeats = visibleBeats;
            resizePxPerBeat = pxPerBeat;
            resizeStartX = beatToX (startBeatOf (hit.slot));
            resizeGrabOffset = e.position.x - beatToX (startBeatOf (hit.slot) + resizeLength);
            repaintBlock (hit.slot);
            return;
        }

        pressedSlot = hit.slot;
        downPos = e.position;
        processor.startPreviewSlot (hit.slot);
        previewing = true;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (resizeSlot >= 0)
        {
            const float beats = (e.position.x - resizeGrabOffset - resizeStartX) / std::max (1.0f, resizePxPerBeat);
            const int newLength = juce::jlimit (cb::kMinSlotBeats, cb::kMaxSlotBeats, (int) std::lround (beats));

            if (newLength != resizeLength)
            {
                resizeLength = newLength;
                layoutBlocks();
                layoutHeader();   // the length readout changes width
                repaint();
            }

            return;
        }

        if (pressedSlot < 0 || pressedSlot >= (int) blocks.size() || dragStarted
            || e.position.getDistanceFrom (downPos) < kDragThreshold)
            return;

        dragStarted = true;

        if (previewing)
        {
            processor.stopPreview();
            previewing = false;
        }

        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
        {
            juce::Point<int> offset;
            auto image = makeDragImage (pressedSlot, offset);

            if (image.getImage().isNull())
                return;

            dragSourceSlot = pressedSlot;
            repaintBlock (dragSourceSlot);
            // allowDraggingToExternalWindows: after ~0.7 s outside the plugin the editor turns it into a MIDI file drag.
            container->startDragging (cb::ui::drag::makeSlotPayload (pressedSlot), this, image, true, &offset, &e.source);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (resizeSlot >= 0)
        {
            const int slot = resizeSlot;
            const int length = resizeLength;
            resizeSlot = -1;

            const auto& prog = processor.getProgression();

            if (slot < (int) prog.slots.size() && prog.slots[(size_t) slot].lengthBeats != length)
            {
                auto s = prog.slots[(size_t) slot];
                s.lengthBeats = length;
                processor.updateSlot (slot, s);
            }

            sync();
            layoutBlocks();
            layoutHeader();
            syncProxies();
            repaint();
            updateHover (e.position, getLocalBounds().toFloat().contains (e.position));
            return;
        }

        if (previewing)
        {
            processor.stopPreview();
            previewing = false;
        }

        if (dragSourceSlot >= 0)
        {
            repaintBlock (dragSourceSlot);
            dragSourceSlot = -1;
        }

        pressedSlot = -1;
        dragStarted = false;
        updateHover (e.position, getLocalBounds().toFloat().contains (e.position));
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        // Deliberately nothing: the presses of a double-click already selected and auditioned the block. Deleting
        // here used to chain onto the neighbours that slide under the cursor when a user taps a block quickly
        // (JUCE also reports clicks 3 and 4 as double-clicks). Remove with the hover "x" or right-click > Delete.
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (outOfDate())
            sync();

        const auto hit = hitTest (e.position);

        if (hit.slot < 0 || resizeSlot >= 0)
        {
            juce::Component::mouseWheelMove (e, wheel);
            return;
        }

        if (wheel.isInertial)
            return;

        float delta = std::abs (wheel.deltaY) >= std::abs (wheel.deltaX) ? wheel.deltaY : -wheel.deltaX;

        if (wheel.isReversed)
            delta = -delta;

        if (hit.slot != wheelSlot)
        {
            wheelSlot = hit.slot;
            wheelAccum = 0.0f;
        }

        wheelAccum += delta;

        if (std::abs (wheelAccum) < 0.08f)
            return;

        const int step = wheelAccum > 0.0f ? 1 : -1;
        wheelAccum = 0.0f;

        const auto& prog = processor.getProgression();

        if (hit.slot >= (int) prog.slots.size())
            return;

        auto s = prog.slots[(size_t) hit.slot];
        const int newLength = juce::jlimit (cb::kMinSlotBeats, cb::kMaxSlotBeats, s.lengthBeats + step);

        if (newLength != s.lengthBeats)
        {
            s.lengthBeats = newLength;
            processor.updateSlot (hit.slot, s);
            sync();
        }
    }

    juce::ScaledImage makeDragImage (int index, juce::Point<int>& offset) const
    {
        if (index < 0 || index >= (int) blocks.size())
        {
            offset = {};
            return {};
        }

        const auto& b = blocks[(size_t) index];
        const float scale = juce::jmax (1.0f, (float) juce::Component::getApproximateScaleFactorForComponent (this));
        const float margin = 12.0f;
        const auto box = b.bounds.getSmallestIntegerContainer();
        const juce::Point<int> size (std::min (box.getWidth(), 168), box.getHeight());   // long blocks travel as a compact card

        juce::Image image (juce::Image::ARGB,
                           juce::roundToInt (((float) size.x + 2.0f * margin) * scale),
                           juce::roundToInt (((float) size.y + 2.0f * margin) * scale), true);
        {
            juce::Graphics g (image);
            g.addTransform (juce::AffineTransform::scale (scale));
            const juce::Rectangle<float> r (margin, margin, (float) size.x, (float) size.y);

            juce::Path shape;
            shape.addRoundedRectangle (r.translated (0.0f, 3.0f), kBlockRadius);
            juce::DropShadow (juce::Colours::black.withAlpha (0.5f), 10, {}).drawForPath (g, shape);
            drawHalo (g, r, kBlockRadius, b.colour, 0.3f, 4);
            paintBlock (g, index, b, r, true);
        }

        const float grabX = juce::jlimit (12.0f, std::max (12.0f, (float) size.x - 12.0f), downPos.x - b.bounds.getX());
        offset = juce::Point<float> (-(grabX + margin), b.bounds.getY() - margin - downPos.y).roundToInt();
        return juce::ScaledImage (image, (double) scale);
    }

    //==============================================================================
    // Edits
    void removeSlot (int index)
    {
        if (previewing)
        {
            processor.stopPreview();
            previewing = false;
        }

        const int newSelected = selected == index ? -1 : (selected > index ? selected - 1 : selected);
        processor.removeSlot (index);
        sync();
        setSelected (newSelected, true);
    }

    void clearAll()
    {
        if (previewing)
        {
            processor.stopPreview();
            previewing = false;
        }

        processor.clearProgression();
        setSelected (-1, true);
        sync();
    }

    // Spark / presets: replace the whole lane in one undoable step.
    void replaceProgression (cb::Progression p)
    {
        if (p.empty())
            return;

        if ((int) p.slots.size() > cb::kMaxSlots)
            p.slots.resize ((size_t) cb::kMaxSlots);

        cancelPress();

        if (p == processor.getProgression())
        {
            flashAll();
            return;
        }

        processor.setProgression (p);
        sync();
        setSelected (-1, true);
        flashAll();
    }

    void transposeBy (int semitones)
    {
        if (semitones == 0 || processor.getProgression().empty())
            return;

        cancelPress();
        processor.transposeProgression (semitones);
        sync();
        flashAll();
    }

    cb::SparkSettings loadSparkSettings() const
    {
        const auto& state = processor.getAPVTS().state;
        cb::SparkSettings s;

        const auto snap = [] (int v, std::initializer_list<int> allowed, int fallback)
        {
            for (int a : allowed)
                if (a == v)
                    return v;
            return fallback;
        };

        s.mood = (cb::Mood) juce::jlimit (0, cb::numMoods - 1, (int) state.getProperty (kSparkMoodProp, (int) cb::Mood::Emotional));
        s.numChords = snap ((int) state.getProperty (kSparkChordsProp, 4), { 4, 6, 8 }, 4);
        s.beatsPerChord = snap ((int) state.getProperty (kSparkBeatsProp, 4), { 2, 4, 8 }, 4);
        return s;
    }

    void saveSparkSettings (const cb::SparkSettings& s)
    {
        auto state = processor.getAPVTS().state;
        state.setProperty (kSparkMoodProp, (int) s.mood, nullptr);
        state.setProperty (kSparkChordsProp, s.numChords, nullptr);
        state.setProperty (kSparkBeatsProp, s.beatsPerChord, nullptr);
        updateSparkLabels();
        layoutHeader();
    }

    void spark()
    {
        const auto settings = loadSparkSettings();
        const auto current = processor.getProgression();
        cb::Progression result;

        // A fresh seed every click; retry a few times so a click never "does nothing" by landing on the same chords.
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            const auto seed = (std::uint32_t) juce::Random::getSystemRandom().nextInt() | 1u;
            result = cb::sparkProgression (processor.getKey(), processor.getPalette(), seed, settings);

            if (! result.empty() && result != current)
                break;
        }

        replaceProgression (result);
    }

    //==============================================================================
    // Menus
    juce::PopupMenu buildTransposeMenu() const
    {
        juce::PopupMenu m;
        const bool any = ! cached.slots.empty();
        m.addItem (idTransposeUp, "Up a semitone", any);
        m.addItem (idTransposeDown, "Down a semitone", any);

        if (moveShift != 0 && any)
            m.addItem (idMoveToKey, "Move chords to " + shortKeyName (processor.getKey()) + "  (" + signedSemitones (moveShift) + ")");

        return m;
    }

    juce::PopupMenu buildSparkMenu() const
    {
        const auto s = loadSparkSettings();
        juce::PopupMenu m;
        m.addSectionHeader ("Mood");

        for (int i = 0; i < cb::numMoods; ++i)
            m.addItem (idMoodBase + i, cb::moodName ((cb::Mood) i), true, (int) s.mood == i);

        m.addSectionHeader ("Chords");

        for (int n : { 4, 6, 8 })
            m.addItem (idChordsBase + n, juce::String (n) + " chords", true, s.numChords == n);

        m.addSectionHeader ("Each chord");

        for (int b : { 2, 4, 8 })
            m.addItem (idBeatsBase + b, juce::String (b) + " beats  (" + (b == 2 ? juce::String ("half a bar") : lengthText (b)) + ")",
                       true, s.beatsPerChord == b);

        m.addSeparator();
        m.addItem (idSpark, "Spark a new progression");
        return m;
    }

    juce::PopupMenu buildPresetsMenu (const std::vector<cb::Preset>& presets) const
    {
        juce::PopupMenu m;
        const auto& current = processor.getProgression();

        if (presets.empty())
        {
            m.addItem (-1, "No presets for this key yet", false);
            return m;
        }

        for (int mood = 0; mood < cb::numMoods; ++mood)
        {
            bool header = false;

            for (size_t i = 0; i < presets.size(); ++i)
            {
                const auto& preset = presets[i];

                if ((int) preset.mood != mood)
                    continue;

                if (! header)
                {
                    m.addSectionHeader (cb::moodName ((cb::Mood) mood));
                    header = true;
                }

                juce::PopupMenu::Item item (toJuce (preset.name));
                item.itemID = idPresetBase + (int) i;
                item.shortcutKeyDescription = toJuce (preset.romanSummary);
                item.isTicked = preset.progression == current;
                m.addItem (std::move (item));
            }
        }

        return m;
    }

    struct Suggestion
    {
        cb::Chord chord;
        float score;
        juce::String roman;
    };

    // Chords from the palette that fit between the slot's neighbours (the loop wraps around).
    std::vector<Suggestion> suggestionsFor (int index) const
    {
        std::vector<Suggestion> all;
        const int n = (int) cached.slots.size();

        if (index < 0 || index >= n)
            return all;

        const auto current = cached.slots[(size_t) index].chord;
        const auto prev = cached.slots[(size_t) ((index - 1 + n) % n)].chord;
        const auto next = cached.slots[(size_t) ((index + 1) % n)].chord;

        for (const auto& section : processor.getPalette())
        {
            for (const auto& pc : section.chords)
            {
                const auto& c = pc.chord;

                if (c.root == current.root || (n > 1 && (c.root == prev.root || c.root == next.root))
                    || std::any_of (all.begin(), all.end(), [&c] (const Suggestion& s) { return s.chord == c; }))
                    continue;

                float score;

                if (n == 1)
                {
                    score = cb::openingScore (c, key);
                }
                else
                {
                    score = std::sqrt (std::max (0.0f, cb::transitionScore (prev, c, key)) * std::max (0.0f, cb::transitionScore (c, next, key)));

                    if (index == 0)
                        score = 0.75f * score + 0.25f * cb::openingScore (c, key);
                }

                if (std::isfinite (score) && score > 0.05f)
                    all.push_back ({ c, score, toJuce (pc.roman) });
            }
        }

        std::stable_sort (all.begin(), all.end(), [] (const Suggestion& a, const Suggestion& b) { return a.score > b.score; });

        // At most two variants per root, eight in total, so the list offers different ideas.
        std::vector<Suggestion> out;

        for (const auto& s : all)
        {
            const auto sameRoot = std::count_if (out.begin(), out.end(), [&s] (const Suggestion& o) { return o.chord.root == s.chord.root; });

            if (sameRoot < 2)
                out.push_back (s);

            if (out.size() >= 8)
                break;
        }

        return out;
    }

    juce::PopupMenu buildSlotMenu (int index, const std::vector<Suggestion>& suggestions) const
    {
        juce::PopupMenu menu;

        if (index < 0 || index >= (int) blocks.size())
            return menu;

        const auto expected = cached.slots[(size_t) index];
        const auto& chord = expected.chord;
        const auto& b = blocks[(size_t) index];

        menu.addSectionHeader (b.name + "  (" + b.roman + ")");

        // Change to: same root, other qualities
        juce::PopupMenu changeMenu;
        const auto options = changeToOptions (chord, key);
        bool outsideHeader = false;

        for (const auto& o : options)
        {
            if (! o.inKey && ! outsideHeader)
            {
                changeMenu.addSectionHeader ("Outside the key");
                outsideHeader = true;
            }

            const cb::Chord c { chord.root, o.quality };
            juce::PopupMenu::Item item (toJuce (c.name (key)));
            item.itemID = idChangeBase + (int) o.quality;
            item.shortcutKeyDescription = toJuce (cb::romanNumeral (c, key));
            item.isTicked = o.quality == chord.quality;
            changeMenu.addItem (std::move (item));
        }

        // Suggested here: what fits between the neighbours
        juce::PopupMenu suggestMenu;

        for (size_t i = 0; i < suggestions.size(); ++i)
        {
            juce::PopupMenu::Item item (toJuce (suggestions[i].chord.name (key)));
            item.itemID = idSuggestBase + (int) i;
            item.shortcutKeyDescription = suggestions[i].roman;
            suggestMenu.addItem (std::move (item));
        }

        if (suggestions.empty())
            suggestMenu.addItem (-1, "Nothing stands out here", false);

        juce::PopupMenu lengthMenu;

        for (int beats : { 1, 2, 3, 4, 6, 8, 12, 16 })
            lengthMenu.addItem (idLengthBase + beats, beats % 4 == 0 && beats > 4 ? juce::String (beats) + " beats  (" + lengthText (beats) + ")"
                                                                                  : (beats == 4 ? juce::String ("4 beats  (1 bar)") : lengthText (beats)),
                                true, expected.lengthBeats == beats);

        juce::PopupMenu inversionMenu;
        inversionMenu.addItem (idInversionAuto, "Auto (voice leading)", true, expected.inversion < 0);
        const char* inversionNames[] = { "Root position", "1st inversion", "2nd inversion", "3rd inversion" };
        const auto tones = chord.pitchClasses();

        for (int inv = 0; inv < 4; ++inv)
        {
            juce::String label (inversionNames[inv]);

            if (inv > 0 && inv < (int) tones.size())
                label << "  (" << toJuce (chord.name (key)) << "/" << toJuce (cb::spellChordTone (chord, chord.intervals()[(size_t) inv], key)) << ")";

            inversionMenu.addItem (idInversionAuto + 1 + inv, label, inv < (int) tones.size(), expected.inversion == inv);
        }

        juce::PopupMenu octaveMenu;

        for (int oct = -2; oct <= 2; ++oct)
        {
            const juce::String label = oct == 0 ? juce::String ("Default")
                                                : (oct > 0 ? "+" : "") + juce::String (oct) + (std::abs (oct) == 1 ? " octave" : " octaves");
            octaveMenu.addItem (idOctaveBase + oct, label, true, expected.octaveShift == oct);
        }

        menu.addSubMenu ("Change to", changeMenu);
        menu.addSubMenu ("Suggested here", suggestMenu);
        menu.addSeparator();
        menu.addSubMenu ("Length", lengthMenu);
        menu.addSubMenu ("Inversion", inversionMenu);
        menu.addSubMenu ("Octave", octaveMenu);
        menu.addSeparator();
        menu.addSubMenu ("Transpose all", buildTransposeMenu());
        menu.addItem (idDuplicate, "Duplicate", (int) cached.slots.size() < cb::kMaxSlots);
        menu.addItem (idDelete, "Delete");
        return menu;
    }

    juce::PopupMenu buildLaneMenu (const std::vector<cb::Preset>& presets) const
    {
        juce::PopupMenu menu;
        menu.addSectionHeader ("Progression");
        const bool any = ! cached.slots.empty();
        menu.addItem (idTransposeUp, "Transpose up a semitone", any);
        menu.addItem (idTransposeDown, "Transpose down a semitone", any);

        if (moveShift != 0 && any)
            menu.addItem (idMoveToKey, "Move chords to " + shortKeyName (processor.getKey()) + "  (" + signedSemitones (moveShift) + ")");

        menu.addSeparator();
        menu.addItem (idSpark, "Spark a new progression");
        menu.addSubMenu ("Presets", buildPresetsMenu (presets));
        menu.addSeparator();
        menu.addItem (idClear, "Clear", any);
        return menu;
    }

    static juce::PopupMenu::Options menuOptions (juce::Component* target, bool atMouse)
    {
        auto o = juce::PopupMenu::Options().withTargetComponent (target);
        return atMouse ? o.withMousePosition() : o;
    }

    void showSlotMenu (int index)
    {
        if (index < 0 || index >= (int) blocks.size())
            return;

        const auto expected = cached.slots[(size_t) index];
        const auto suggestions = suggestionsFor (index);
        auto menu = buildSlotMenu (index, suggestions);
        menu.setLookAndFeel (&getLookAndFeel());

        juce::Component::SafePointer<Impl> safe (this);
        // Target component (for the plug-in window's DPI scale and look-and-feel), opened at the mouse position.
        menu.showMenuAsync (menuOptions (this, true), [safe, index, expected, suggestions] (int result)
        {
            if (safe != nullptr && result != 0)
                safe->applySlotMenuResult (index, expected, suggestions, result);
        });
    }

    void showLaneMenu (juce::Component* anchor)
    {
        auto presets = cb::presetsFor (processor.getKey());
        auto menu = buildLaneMenu (presets);
        menu.setLookAndFeel (&getLookAndFeel());

        juce::Component::SafePointer<Impl> safe (this);
        const auto keyAtOpen = processor.getKey();
        menu.showMenuAsync (menuOptions (anchor != nullptr ? anchor : this, anchor == nullptr),
                            [safe, presets, keyAtOpen] (int result)
        {
            if (safe != nullptr && result != 0)
                safe->applyLaneMenuResult (result, presets, keyAtOpen);
        });
    }

    void showSparkMenu (juce::Component& anchor)
    {
        auto menu = buildSparkMenu();
        menu.setLookAndFeel (&getLookAndFeel());

        juce::Component::SafePointer<Impl> safe (this);
        menu.showMenuAsync (menuOptions (&anchor, false), [safe] (int result)
        {
            if (safe == nullptr || result == 0)
                return;

            auto s = safe->loadSparkSettings();

            if (result == idSpark)
            {
                safe->spark();
                return;
            }

            if (result >= idBeatsBase && result < idBeatsBase + 10)
                s.beatsPerChord = result - idBeatsBase;
            else if (result >= idChordsBase && result < idChordsBase + 10)
                s.numChords = result - idChordsBase;
            else if (result >= idMoodBase && result < idMoodBase + cb::numMoods)
                s.mood = (cb::Mood) (result - idMoodBase);

            safe->saveSparkSettings (s);

            // Picking a mood is a request to hear it: spark right away (undo brings the old progression back).
            if (result >= idMoodBase && result < idMoodBase + cb::numMoods)
                safe->spark();
        });
    }

    void showPresetsMenu (juce::Component& anchor)
    {
        auto presets = cb::presetsFor (processor.getKey());
        auto menu = buildPresetsMenu (presets);
        menu.setLookAndFeel (&getLookAndFeel());

        juce::Component::SafePointer<Impl> safe (this);
        const auto keyAtOpen = processor.getKey();
        menu.showMenuAsync (menuOptions (&anchor, false), [safe, presets, keyAtOpen] (int result)
        {
            if (safe != nullptr && result != 0)
                safe->applyLaneMenuResult (result, presets, keyAtOpen);
        });
    }

    void applyLaneMenuResult (int result, const std::vector<cb::Preset>& presets, const cb::Key& keyAtOpen)
    {
        if (outOfDate())
            sync();

        if (result == idTransposeUp)        transposeBy (1);
        else if (result == idTransposeDown) transposeBy (-1);
        else if (result == idMoveToKey)     transposeBy (processor.bestTranspositionForKey());
        else if (result == idSpark)         spark();
        else if (result == idClear)         clearAll();
        else if (result >= idPresetBase && result - idPresetBase < (int) presets.size())
        {
            // Presets are spelled for the key the menu was opened in; ignore a stale pick after a key change.
            if (processor.getKey() == keyAtOpen)
                replaceProgression (presets[(size_t) (result - idPresetBase)].progression);
        }
    }

    void applySlotMenuResult (int index, const cb::ProgressionSlot& expected, const std::vector<Suggestion>& suggestions, int result)
    {
        if (result == idTransposeUp || result == idTransposeDown || result == idMoveToKey)
        {
            applyLaneMenuResult (result, {}, processor.getKey());
            return;
        }

        const auto& prog = processor.getProgression();

        // The progression may have changed while the menu was open (undo, host automation...).
        if (index >= (int) prog.slots.size() || prog.slots[(size_t) index] != expected)
            return;

        if (result == idDuplicate)
        {
            if (processor.duplicateSlot (index))
            {
                sync();
                setSelected (index + 1, true);
            }

            return;
        }

        if (result == idDelete)
        {
            removeSlot (index);
            return;
        }

        auto s = expected;
        bool chordChanged = false;

        if (result >= idPresetBase)
            return;

        if (result >= idSuggestBase && result < idSuggestBase + 100)
        {
            const int k = result - idSuggestBase;

            if (k >= (int) suggestions.size())
                return;

            s.chord = suggestions[(size_t) k].chord;
            s.inversion = -1;
            chordChanged = true;
        }
        else if (result >= idChangeBase && result < idChangeBase + cb::numChordQualities)
        {
            s.chord.quality = (cb::ChordQuality) (result - idChangeBase);
            s.inversion = s.inversion < s.chord.numTones() ? s.inversion : -1;
            chordChanged = s.chord != expected.chord;
        }
        else if (result >= idOctaveBase - 2 && result <= idOctaveBase + 2)
            s.octaveShift = juce::jlimit (-2, 2, result - idOctaveBase);
        else if (result >= idInversionAuto && result <= idInversionAuto + 4)
            s.inversion = result == idInversionAuto ? -1 : result - idInversionAuto - 1;
        else if (result >= idLengthBase && result < idLengthBase + 100)
            s.lengthBeats = juce::jlimit (cb::kMinSlotBeats, cb::kMaxSlotBeats, result - idLengthBase);

        if (s != expected)
        {
            processor.updateSlot (index, s);
            sync();

            if (chordChanged && index < (int) blocks.size())
            {
                blocks[(size_t) index].flash = 1.0f;
                speedUp();
                processor.startPreviewSlot (index);   // let the user hear the new chord in place
                juce::Component::SafePointer<Impl> safe (this);
                juce::Timer::callAfterDelay (600, [safe]
                {
                    if (safe != nullptr && ! safe->previewing)
                        safe->processor.stopPreview();
                });
            }
        }
    }

    void stopPlayingWithHost()
    {
        if (auto* param = processor.getAPVTS().getParameter (cb::ParamIDs::syncToHost))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (0.0f);
            param->endChangeGesture();
        }

        pollTransport();
    }

    //==============================================================================
    // Drag and drop (forwarded from the ProgressionLane, positions are in our coordinates: we fill the lane)
    void updateDrop (const juce::DragAndDropTarget::SourceDetails& details)
    {
        if (outOfDate())
            sync();

        const auto pos = getLocalPoint (&owner, details.localPosition).toFloat();
        const bool wasActive = dropActive;
        dropActive = true;
        dropIndex = insertionIndexAt (pos.x);
        dropFromSlot = -1;
        dropRefused = false;
        dropReplace = -1;

        if (cb::ui::drag::isChordPayload (details.description))
        {
            const auto chord = cb::ui::drag::chordFromPayload (details.description);
            dropLabel = toJuce (chord.name (key));
            dropReplace = replaceTargetAt (pos);
            dropRefused = dropReplace < 0 && (int) cached.slots.size() >= cb::kMaxSlots;
        }
        else
        {
            dropFromSlot = cb::ui::drag::slotFromPayload (details.description);
        }

        if (! wasActive)
        {
            layoutEmptyState();
            repaintSelectionMarker();
        }

        repaint();
    }

    void clearDrop()
    {
        if (! dropActive)
            return;

        dropActive = false;
        dropIndex = -1;
        dropFromSlot = -1;
        dropReplace = -1;
        layoutEmptyState();
        repaint();
    }

    void drop (const juce::DragAndDropTarget::SourceDetails& details)
    {
        updateDrop (details);
        const int index = dropIndex;
        const int replace = dropReplace;
        int newSelected = selected;
        int flashIndex = -1;

        if (cb::ui::drag::isChordPayload (details.description))
        {
            const auto chord = cb::ui::drag::chordFromPayload (details.description);

            if (replace >= 0 && replace < (int) cached.slots.size())
            {
                // Replace the chord, keep the slot's length and octave; inversion back to auto (the old one may not exist).
                auto s = cached.slots[(size_t) replace];

                if (s.chord != chord)
                {
                    s.chord = chord;
                    s.inversion = -1;
                    processor.updateSlot (replace, s);
                }

                newSelected = replace;
                flashIndex = replace;
            }
            else if (! dropRefused && processor.addChord (chord, index))
            {
                newSelected = index;   // the selection follows what was added (the palette glow continues from it)
            }
        }
        else if (cb::ui::drag::isSlotPayload (details.description))
        {
            const int n = (int) cached.slots.size();
            const int from = cb::ui::drag::slotFromPayload (details.description);
            const int to = juce::jlimit (0, std::max (0, n - 1), index > from ? index - 1 : index);

            if (from >= 0 && from < n && to != from)
            {
                processor.moveSlot (from, to);

                if (selected == from)
                    newSelected = to;
                else if (from < selected && selected <= to)
                    newSelected = selected - 1;
                else if (to <= selected && selected < from)
                    newSelected = selected + 1;
            }
        }

        dragSourceSlot = -1;
        clearDrop();
        sync();
        setSelected (newSelected, true);

        if (flashIndex >= 0 && flashIndex < (int) blocks.size())
        {
            blocks[(size_t) flashIndex].flash = 1.0f;
            speedUp();
        }
    }

    //==============================================================================
    juce::String getTooltip() override
    {
        if (resizeSlot >= 0 || dropActive)
            return {};

        if (hoverSlot >= 0 && hoverSlot < (int) blocks.size())
        {
            const auto& b = blocks[(size_t) hoverSlot];

            if (hoverEdge)
                return "Drag to change the length of " + b.name + " (" + b.length + ")";

            if (hoverClose)
                return "Remove " + b.name + " (undo brings it back)";

            juce::String tip;
            tip << b.name << "  (" << b.roman << ")  " << middleDot() << "  " << b.length
                << "\nClick to hear " << middleDot() << " drag to reorder, or out into FL Studio"
                << "\nDrop a pad on it to replace it " << middleDot() << " scroll to change the length"
                << "\nRight-click to change the chord, inversion, octave or remove it";
            return tip;
        }

        if (blocks.empty() && trackArea.contains (lastMouse))
            return "Drag chords here from the palette, double-click a pad, or let Spark start one for you";

        if (! blocks.empty() && trackArea.contains (lastMouse))
            return "Click empty space to deselect (chords are then added at the end) " + middleDot() + " right-click to transpose";

        return {};
    }

    //==============================================================================
    // Transport polling: slow while idle, 60 Hz only while something moves.
    void speedUp()
    {
        if (currentHz != activeHz)
        {
            currentHz = activeHz;
            startTimerHz (currentHz);
        }
    }

    bool syncToHostOn() const
    {
        if (auto* v = processor.getAPVTS().getRawParameterValue (cb::ParamIDs::syncToHost))
            return v->load() >= 0.5f;

        return false;
    }

    void pollTransport()
    {
        const bool local = processor.isLocalPlaybackOn();

        if (playButton.getToggleState() != local)
            playButton.setToggleState (local, juce::dontSendNotification);

        updatePlayButtonEnablement();

        const int ps = processor.getPlayingSlot();
        const bool nowPlaying = ps >= 0 && ps < (int) blocks.size();
        const double beat = nowPlaying ? juce::jlimit (0.0, (double) visibleBeats, processor.getPlaybackBeat()) : 0.0;
        const bool host = processor.isHostPlaying();

        hostPlaying = host;

        // "Following host" pill: the progression loops along with the host transport.
        const bool following = host && nowPlaying && syncToHostOn();

        if (following != hostButton.isVisible())
        {
            hostButton.setVisible (following);
            layoutHeader();
        }

        if (following)
        {
            hostButton.pulse = (float) std::fmod (juce::Time::getMillisecondCounterHiRes() / 1400.0, 1.0);
            hostButton.repaint();
        }

        // B4: a host loop shorter than the progression (FL pattern mode) - shown in the header hint and tooltip.
        const double loop = following ? processor.getHostLoopBeats() : 0.0;

        if (std::abs (loop - hostLoopBeats) > 1.0e-6)
        {
            hostLoopBeats = loop;
            juce::String tip ("Chordbloom is looping the progression along with FL's transport (Play with FL is on).\n");

            if (loop > 0.0)
                tip << "FL loops every " << totalText (juce::roundToInt (loop)) << ", shorter than the progression ("
                    << totalText (effectiveTotal()) << "): Chordbloom keeps going across FL's loop. Set the FL pattern to "
                    << totalText (effectiveTotal()) << " to line them up.\n";

            tip << "Already dropped the clip into FL? Click to stop Chordbloom playing along, so you don't hear it twice.";
            hostButton.setTooltip (tip);
            repaint (headerRow);
        }

        if (nowPlaying != playing || ps != playingSlot || (nowPlaying && std::abs (beat - playBeat) > 1.0e-6))
        {
            if (playing)
            {
                repaintPlayheadAt (playBeat);
                repaintBlock (playingSlot);
            }

            playing = nowPlaying;
            playingSlot = nowPlaying ? ps : -1;
            playBeat = beat;

            if (playing)
            {
                repaintPlayheadAt (playBeat);
                repaintBlock (playingSlot);
            }
        }
    }

    void timerCallback() override
    {
        if (outOfDate())
            sync();

        pollTransport();

        // The Spark settings live in the plugin state: a host state load can change them under us.
        {
            const auto s = loadSparkSettings();

            if (s.mood != shownSpark.mood || s.numChords != shownSpark.numChords || s.beatsPerChord != shownSpark.beatsPerChord)
            {
                updateSparkLabels();
                layoutHeader();
            }
        }

        // A block dragged out into the DAW never gets its mouse-up here (the OS drag swallows it): drop the ghost
        // as soon as no button is held any more.
        if (dragSourceSlot >= 0 && ! juce::ModifierKeys::getCurrentModifiersRealtime().isAnyMouseButtonDown())
        {
            repaintBlock (dragSourceSlot);
            dragSourceSlot = -1;
        }

        bool flashing = false;

        for (int i = 0; i < (int) blocks.size(); ++i)
        {
            auto& b = blocks[(size_t) i];

            if (b.flash > 0.0f)
            {
                b.flash = b.flash < 0.03f ? 0.0f : b.flash * 0.9f;
                repaintBlock (i);
                flashing = true;
            }
        }

        const int wanted = (playing || flashing) ? activeHz : idleHz;

        if (wanted != currentHz)
        {
            currentHz = wanted;
            startTimerHz (currentHz);
        }
    }

    //==============================================================================
    ProgressionLane& owner;
    ChordbloomProcessor& processor;

    PlayButton playButton;
    ClearButton clearButton;
    PillButton sparkButton { "Spark", Glyph::spark, PillButton::Style::accent };
    PillButton presetsButton { "Presets", Glyph::presets, PillButton::Style::outline };
    PillButton moveButton { "Move chords to key", Glyph::move, PillButton::Style::info };
    PillButton hostButton { "Stop playing along with FL", Glyph::live, PillButton::Style::live };
    PillButton emptySparkButton { "Spark a progression", Glyph::spark, PillButton::Style::filled };
    PillButton emptyPresetsButton { "Presets", Glyph::presets, PillButton::Style::outline };
    std::vector<std::unique_ptr<AccessibleItem>> proxies;

    cb::Progression cached, synced;
    cb::Key key, syncedKey;
    std::vector<Block> blocks;

    juce::Rectangle<int> headerRow;
    juce::Rectangle<float> hintArea, rulerArea, trackArea, emptyTextArea;
    bool compact = false;
    juce::String emptyText;
    juce::Font emptyFont { juce::FontOptions() };
    bool showEmptyText = true;
    juce::String moveFullText, moveShortText;
    cb::SparkSettings shownSpark;
    int moveShift = 0;
    int visibleBeats = kMinVisibleBeats;
    float pxPerBeat = 1.0f;

    int selected = -1;
    int hoverSlot = -1;
    bool hoverEdge = false;
    bool hoverClose = false;
    juce::Point<float> lastMouse;

    int pressedSlot = -1;
    bool previewing = false;
    bool dragStarted = false;
    int dragSourceSlot = -1;
    juce::Point<float> downPos;

    int resizeSlot = -1;
    int resizeLength = 4;
    int frozenVisibleBeats = kMinVisibleBeats;
    float resizePxPerBeat = 1.0f;
    float resizeStartX = 0.0f;
    float resizeGrabOffset = 0.0f;

    int wheelSlot = -1;
    float wheelAccum = 0.0f;

    bool dropActive = false;
    int dropIndex = -1;
    int dropFromSlot = -1;
    int dropReplace = -1;
    bool dropRefused = false;
    juce::String dropLabel;

    bool playing = false;
    bool hostPlaying = false;
    double hostLoopBeats = 0.0;   // processor.getHostLoopBeats() while following the host, else 0
    bool playButtonHostDriven = false;
    int playingSlot = -1;
    double playBeat = 0.0;

    static constexpr int idleHz = 15;
    static constexpr int activeHz = 60;
    int currentHz = idleHz;
};

//==============================================================================
ProgressionLane::ProgressionLane (ChordbloomProcessor& p) : impl (std::make_unique<Impl> (*this, p))
{
    setOpaque (false);
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    setTitle ("Progression");
    addAndMakeVisible (*impl);
}

ProgressionLane::~ProgressionLane() = default;

void ProgressionLane::paint (juce::Graphics&) {}   // the Impl surface paints the whole lane

void ProgressionLane::resized() { impl->setBounds (getLocalBounds()); }

void ProgressionLane::refresh() { impl->sync(); }

int ProgressionLane::getSelectedSlot() const noexcept
{
    // Never hand out an index the current progression doesn't have (e.g. right after an undo, before refresh()).
    const int n = (int) impl->processor.getProgression().slots.size();
    return impl->selected >= 0 && impl->selected < n ? impl->selected : -1;
}

void ProgressionLane::setSelectedSlot (int index) { impl->setSelected (index, false); }

void ProgressionLane::setSelectedSlot (int index, bool notify)
{
    if (impl->outOfDate())
        impl->sync();

    impl->setSelected (index, notify);
}

bool ProgressionLane::isInterestedInDragSource (const SourceDetails& details)
{
    if (cb::ui::drag::isChordPayload (details.description))
        return true;

    // Slot reordering only within this lane
    return cb::ui::drag::isSlotPayload (details.description) && details.sourceComponent.get() == impl.get();
}

void ProgressionLane::itemDragEnter (const SourceDetails& details) { impl->updateDrop (details); }
void ProgressionLane::itemDragMove (const SourceDetails& details)  { impl->updateDrop (details); }
void ProgressionLane::itemDragExit (const SourceDetails&)          { impl->clearDrop(); }
void ProgressionLane::itemDropped (const SourceDetails& details)   { impl->drop (details); }
