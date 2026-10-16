#include "ui/ProgressionLane.h"

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

//==============================================================================
class PlayButton : public juce::Button
{
public:
    PlayButton() : juce::Button ("Play")
    {
        setTooltip ("Play / stop the progression loop");
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
        setTooltip ("Clear the progression (undo brings it back)");
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
        const float x = std::round (r.getCentreX() - (iconW + 6.0f + textW) * 0.5f);
        const float cy = r.getCentreY();

        // Trash-can icon
        g.setColour (ink);
        g.fillRoundedRectangle (x, cy - 4.5f, iconW, 1.4f, 0.7f);
        g.fillRoundedRectangle (x + 3.0f, cy - 6.0f, iconW - 6.0f, 1.4f, 0.7f);
        juce::Path body;
        body.addRoundedRectangle (x + 1.2f, cy - 2.5f, iconW - 2.4f, 7.5f, 1.2f);
        g.strokePath (body, juce::PathStrokeType (1.3f));

        g.setFont (font);
        g.drawText (label, juce::Rectangle<float> (x + iconW + 6.0f, r.getY(), textW + 2.0f, r.getHeight()),
                    juce::Justification::centredLeft, false);
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
        addAndMakeVisible (playButton);
        addAndMakeVisible (clearButton);

        playButton.onClick = [this]
        {
            processor.setLocalPlayback (! processor.isLocalPlaybackOn());
            playButton.setToggleState (processor.isLocalPlaybackOn(), juce::dontSendNotification);
            pollTransport();
        };

        clearButton.onClick = [this]
        {
            if (previewing)
            {
                processor.stopPreview();
                previewing = false;
            }

            processor.clearProgression();
            setSelected (-1, true);
            sync();
        };

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

            if (resizeSlot >= n)
                resizeSlot = -1;

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

            repaint();
        }

        clearButton.setEnabled (! prog.empty());
        playButton.setEnabled (! prog.empty() || processor.isLocalPlaybackOn());
    }

    void setSelected (int index, bool notify)
    {
        index = index >= 0 && index < (int) cached.slots.size() ? index : -1;

        if (index == selected)
            return;

        repaintBlock (selected);
        selected = index;
        repaintBlock (selected);

        if (notify && owner.onSelectionChanged != nullptr)
            owner.onSelectionChanged (selected);
    }

    //==============================================================================
    // Layout
    void resized() override
    {
        auto area = getLocalBounds().reduced (12, 8);
        const bool compact = area.getHeight() < 104;

        auto head = area.removeFromTop (compact ? 28 : 32);
        playButton.setBounds (head.removeFromLeft (head.getHeight()));
        head.removeFromLeft (10);
        clearButton.setBounds (head.removeFromRight (74).withSizeKeepingCentre (74, 24));
        head.removeFromRight (10);
        headerText = head.toFloat();

        area.removeFromTop (compact ? 4 : 6);
        rulerArea = area.removeFromTop (compact ? 14 : 16).toFloat();
        area.removeFromTop (3);
        trackArea = area.toFloat();

        layoutBlocks();
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
                b.name << "/" << toJuce (key.spell (chord.pitchClasses()[(size_t) b.slot.inversion]));

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

            for (const auto& candidate : { b.name, toJuce (chord.name (key)), toJuce (key.spell (chord.root)) })
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
        auto r = headerText;
        const auto titleFont = cb::ui::font (10.5f, 600).withExtraKerningFactor (0.12f);
        const juce::String title ("PROGRESSION");
        const float titleW = widthOf (titleFont, title) + 2.0f;

        g.setFont (titleFont);
        g.setColour (Col::textDim);
        g.drawText (title, r.removeFromLeft (titleW), juce::Justification::centredLeft, false);
        r.removeFromLeft (12.0f);

        const int n = (int) cached.slots.size();
        juce::String readout;

        if (n == 0)
            readout = "Empty";
        else
            readout << totalText (effectiveTotal());

        const auto readoutFont = cb::ui::font (13.0f, 600);
        const float readoutW = widthOf (readoutFont, readout) + 2.0f;
        g.setFont (readoutFont);
        g.setColour (n == 0 ? Col::textFaint : Col::text);
        g.drawText (readout, r.removeFromLeft (readoutW), juce::Justification::centredLeft, false);

        if (n > 0)
        {
            juce::String detail;
            detail << middleDot() << "  " << n << (n == 1 ? " chord" : " chords");

            if (hostPlaying && playing)
                detail << "  " << middleDot() << "  following the host";

            const auto detailFont = cb::ui::font (12.0f, 400);
            const float detailW = widthOf (detailFont, detail) + 2.0f;
            r.removeFromLeft (8.0f);
            g.setFont (detailFont);
            g.setColour (Col::textDim);
            g.drawText (detail, r.removeFromLeft (std::min (detailW, r.getWidth())), juce::Justification::centredLeft, true);
        }

        // Discoverability hint on the right, only when there is room for it.
        const juce::String hint = n == 0 ? juce::String ("Click a pad to hear it, double-click to add it")
                                         : juce::String ("Scroll a chord to change its length") + "  " + middleDot()
                                               + "  right-click for more";
        const auto hintFont = cb::ui::font (11.5f, 400);

        if (widthOf (hintFont, hint) + 24.0f < r.getWidth())
        {
            g.setFont (hintFont);
            g.setColour (Col::textFaint);
            g.drawText (hint, r, juce::Justification::centredRight, false);
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

        const juce::String text ("Drag chords here or double-click a pad");
        const auto font = cb::ui::font (juce::jlimit (12.0f, 14.0f, r.getHeight() * 0.22f), 600);
        const float textW = widthOf (font, text);
        const float icon = 16.0f;
        const float x = std::round (r.getCentreX() - (icon + 8.0f + textW) * 0.5f);
        const float cy = r.getCentreY();

        // Plus-in-circle icon
        const auto circle = juce::Rectangle<float> (icon, icon).withCentre ({ x + icon * 0.5f, cy });
        g.setColour (Col::accent.withAlpha (0.8f));
        g.drawEllipse (circle.reduced (0.75f), 1.3f);
        g.fillRoundedRectangle (juce::Rectangle<float> (7.0f, 1.4f).withCentre (circle.getCentre()), 0.7f);
        g.fillRoundedRectangle (juce::Rectangle<float> (1.4f, 7.0f).withCentre (circle.getCentre()), 0.7f);

        g.setFont (font);
        g.setColour (Col::textDim);
        g.drawText (text, juce::Rectangle<float> (x + icon + 8.0f, r.getY(), textW + 4.0f, r.getHeight()),
                    juce::Justification::centredLeft, false);
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

    void paintDropCaret (juce::Graphics& g) const
    {
        if (! dropActive || dropIndex < 0)
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
            if (trackArea.contains (e.position) && ! e.mods.isPopupMenu())
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
                repaint();
            }

            return;
        }

        if (pressedSlot < 0 || dragStarted || e.position.getDistanceFrom (downPos) < kDragThreshold)
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
            dragSourceSlot = pressedSlot;
            repaintBlock (dragSourceSlot);
            container->startDragging (cb::ui::drag::makeSlotPayload (pressedSlot), this, image, false, &offset, &e.source);
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

            layoutBlocks();
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

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (outOfDate())
            sync();

        const auto hit = hitTest (e.position);

        if (hit.slot < 0 || hit.edge || hit.close || e.mods.isPopupMenu())
            return;

        removeSlot (hit.slot);
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

    void showSlotMenu (int index)
    {
        if (index < 0 || index >= (int) blocks.size())
            return;

        const auto expected = cached.slots[(size_t) index];
        const auto& chord = expected.chord;
        const auto& b = blocks[(size_t) index];

        juce::PopupMenu menu;
        menu.setLookAndFeel (&getLookAndFeel());
        menu.addSectionHeader (b.name + "  (" + b.roman + ")");

        juce::PopupMenu lengthMenu;

        for (int beats : { 1, 2, 3, 4, 6, 8, 12, 16 })
            lengthMenu.addItem (100 + beats, beats % 4 == 0 && beats > 4 ? juce::String (beats) + " beats  (" + lengthText (beats) + ")"
                                                                       : (beats == 4 ? juce::String ("4 beats  (1 bar)") : lengthText (beats)),
                                true, expected.lengthBeats == beats);

        juce::PopupMenu inversionMenu;
        inversionMenu.addItem (200, "Auto (voice leading)", true, expected.inversion < 0);
        const char* inversionNames[] = { "Root position", "1st inversion", "2nd inversion", "3rd inversion" };
        const auto tones = chord.pitchClasses();

        for (int inv = 0; inv < 4; ++inv)
        {
            juce::String label (inversionNames[inv]);

            if (inv > 0 && inv < (int) tones.size())
                label << "  (" << toJuce (chord.name (key)) << "/" << toJuce (key.spell (tones[(size_t) inv])) << ")";

            inversionMenu.addItem (201 + inv, label, inv < (int) tones.size(), expected.inversion == inv);
        }

        juce::PopupMenu octaveMenu;

        for (int oct = -2; oct <= 2; ++oct)
        {
            const juce::String label = oct == 0 ? juce::String ("Default")
                                                : (oct > 0 ? "+" : "") + juce::String (oct) + (std::abs (oct) == 1 ? " octave" : " octaves");
            octaveMenu.addItem (310 + oct, label, true, expected.octaveShift == oct);
        }

        menu.addSubMenu ("Length", lengthMenu);
        menu.addSubMenu ("Inversion", inversionMenu);
        menu.addSubMenu ("Octave", octaveMenu);
        menu.addSeparator();
        menu.addItem (1, "Duplicate", (int) cached.slots.size() < cb::kMaxSlots);
        menu.addItem (2, "Delete");

        juce::Component::SafePointer<Impl> safe (this);
        // Target component (for the plug-in window's DPI scale and look-and-feel), opened at the mouse position.
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(),
                            [safe, index, expected] (int result)
        {
            if (safe != nullptr && result != 0)
                safe->applyMenuResult (index, expected, result);
        });
    }

    void applyMenuResult (int index, const cb::ProgressionSlot& expected, int result)
    {
        const auto& prog = processor.getProgression();

        // The progression may have changed while the menu was open (undo, host automation...).
        if (index >= (int) prog.slots.size() || prog.slots[(size_t) index] != expected)
            return;

        if (result == 1)
        {
            if (processor.duplicateSlot (index))
            {
                sync();
                setSelected (index + 1, true);
            }

            return;
        }

        if (result == 2)
        {
            removeSlot (index);
            return;
        }

        auto s = expected;

        if (result >= 300)
            s.octaveShift = juce::jlimit (-2, 2, result - 310);
        else if (result >= 200)
            s.inversion = result == 200 ? -1 : result - 201;
        else if (result >= 100)
            s.lengthBeats = juce::jlimit (cb::kMinSlotBeats, cb::kMaxSlotBeats, result - 100);

        if (s != expected)
        {
            processor.updateSlot (index, s);
            sync();
        }
    }

    //==============================================================================
    // Drag and drop (forwarded from the ProgressionLane, positions are in our coordinates: we fill the lane)
    void updateDrop (const juce::DragAndDropTarget::SourceDetails& details)
    {
        if (outOfDate())
            sync();

        const auto pos = getLocalPoint (&owner, details.localPosition).toFloat();
        dropActive = true;
        dropIndex = insertionIndexAt (pos.x);
        dropFromSlot = -1;
        dropRefused = false;

        if (cb::ui::drag::isChordPayload (details.description))
        {
            const auto chord = cb::ui::drag::chordFromPayload (details.description);
            dropLabel = toJuce (chord.name (key));
            dropRefused = (int) cached.slots.size() >= cb::kMaxSlots;
        }
        else
        {
            dropFromSlot = cb::ui::drag::slotFromPayload (details.description);
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
        repaint();
    }

    void drop (const juce::DragAndDropTarget::SourceDetails& details)
    {
        updateDrop (details);
        const int index = dropIndex;
        int newSelected = selected;

        if (cb::ui::drag::isChordPayload (details.description))
        {
            if (! dropRefused && processor.addChord (cb::ui::drag::chordFromPayload (details.description), index))
                if (selected >= index)
                    newSelected = selected + 1;
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
                << "\nClick to hear " << middleDot() << " drag to reorder " << middleDot() << " double-click to remove"
                << "\nScroll to change the length " << middleDot() << " right-click for inversion, octave and more";
            return tip;
        }

        if (blocks.empty() && trackArea.contains (lastMouse))
            return "Drag chords here from the palette, or double-click a pad to add it";

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

    void pollTransport()
    {
        const bool local = processor.isLocalPlaybackOn();

        if (playButton.getToggleState() != local)
            playButton.setToggleState (local, juce::dontSendNotification);

        const bool wantEnabled = ! cached.slots.empty() || local;

        if (playButton.isEnabled() != wantEnabled)
            playButton.setEnabled (wantEnabled);

        const int ps = processor.getPlayingSlot();
        const bool nowPlaying = ps >= 0 && ps < (int) blocks.size();
        const double beat = nowPlaying ? juce::jlimit (0.0, (double) visibleBeats, processor.getPlaybackBeat()) : 0.0;
        const bool host = processor.isHostPlaying();

        if (host != hostPlaying || nowPlaying != playing)
            repaint (headerText.getSmallestIntegerContainer());

        hostPlaying = host;

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

    cb::Progression cached, synced;
    cb::Key key, syncedKey;
    std::vector<Block> blocks;

    juce::Rectangle<float> headerText, rulerArea, trackArea;
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
    bool dropRefused = false;
    juce::String dropLabel;

    bool playing = false;
    bool hostPlaying = false;
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
