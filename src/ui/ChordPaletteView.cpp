#include "ui/ChordPaletteView.h"

#include "ui/DragPayload.h"
#include "ui/PluginEditor.h"
#include "ui/ProgressionLane.h"
#include "ui/Theme.h"

#include <algorithm>
#include <cmath>

namespace
{
namespace Col = cb::ui::Colours;

constexpr int kColumns = cb::kMaxPadsPerSection;   // 8 pads per row
constexpr float kPadGap = 6.0f;
constexpr float kRowGap = 6.0f;
constexpr float kPadRadius = 7.0f;
constexpr float kAccentBar = 3.0f;
constexpr float kMinSuggestScore = 0.35f;
constexpr int kTopSuggestions = 3;
constexpr float kDragThreshold = 6.0f;

juce::String toJuce (const std::string& s) { return juce::String::fromUTF8 (s.data(), (int) s.size()); }

const juce::String& middleDot()
{
    static const juce::String dot = juce::String::charToString ((juce::juce_wchar) 0x00b7);
    return dot;
}

float widthOf (const juce::Font& f, const juce::String& s) { return juce::GlyphArrangement::getStringWidth (f, s); }

// Largest font (<= height) for which `text` fits into maxWidth, never smaller than minHeight.
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

const char* defaultSectionTitle (int section)
{
    static const char* titles[] = { "Triads", "Sevenths", "Color", "Borrowed", "Secondary" };
    return titles[juce::jlimit (0, (int) std::size (titles) - 1, section)];
}

// Soft halo made of concentric strokes: cheap, crisp, and it only shows in the gaps between pads.
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

// The top-suggestion marker (also used by the legend).
void drawTopDot (juce::Graphics& g, juce::Point<float> c)
{
    g.setColour (Col::accent.withAlpha (0.25f));
    g.fillEllipse (juce::Rectangle<float> (9.0f, 9.0f).withCentre (c));
    g.setColour (Col::accent);
    g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre (c));
}

// Invisible, click-through child that exposes one painted item (pad) to accessibility clients.
class AccessibleItem : public juce::Component
{
public:
    AccessibleItem()
    {
        setInterceptsMouseClicks (false, false);
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
        setAccessible (true);
    }

    std::function<void()> onPress;

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        juce::AccessibilityActions actions;
        actions.addAction (juce::AccessibilityActionType::press, [this] { if (onPress != nullptr) onPress(); });
        return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::button, std::move (actions));
    }
};

struct Pad
{
    cb::PaletteChord info;
    juce::String name, roman, tooltip;
    juce::Colour colour;
    juce::Rectangle<float> bounds;
    juce::Font nameFont { juce::FontOptions() };
    juce::Font romanFont { juce::FontOptions() };
    bool showRoman = true;

    float score = 0.0f;   // suggestion strength for the current context chord (or opening strength)
    bool top = false;     // one of the top suggestions

    float hover = 0.0f;   // animated 0..1
    float flash = 0.0f;   // animated 1 -> 0 after the chord was added
    bool flashError = false;
};

struct Row
{
    juce::String title;
    std::vector<Pad> pads;
    juce::Rectangle<float> bounds, label, cells;
};
} // namespace

//==============================================================================
// The Impl is the full-size child surface that paints and handles everything (it is a TooltipClient so each
// pad can show its own hint).
struct ChordPaletteView::Impl : public juce::Component,
                                public juce::TooltipClient,
                                private juce::Timer,
                                private juce::AsyncUpdater
{
    explicit Impl (ChordbloomProcessor& p) : processor (p)
    {
        setOpaque (false);
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);   // a click must never pull keyboard focus away from the host
        setTitle ("Chord palette");
        setDescription ("Chords that fit the key. Click a chord to hear it, double-click to add it to the progression.");
        rebuild();
    }

    ~Impl() override
    {
        stopTimer();
        cancelPendingUpdate();

        if (previewing)
            processor.stopPreview();
    }

    //==============================================================================
    bool matchesProcessor() const
    {
        if (processor.getKey() != key)
            return false;

        const auto& palette = processor.getPalette();

        for (int s = 0; s < cb::numPaletteSections; ++s)
        {
            const auto* section = s < (int) palette.size() ? &palette[(size_t) s] : nullptr;
            const size_t count = section != nullptr ? std::min (section->chords.size(), (size_t) kColumns) : 0;
            const auto& row = rows[(size_t) s];

            if (row.pads.size() != count)
                return false;

            for (size_t i = 0; i < count; ++i)
                if (row.pads[i].info.chord != section->chords[i].chord || row.pads[i].info.roman != section->chords[i].roman)
                    return false;
        }

        return true;
    }

    void rebuild()
    {
        const auto& palette = processor.getPalette();
        key = processor.getKey();

        std::vector<Row> newRows ((size_t) cb::numPaletteSections);

        for (int s = 0; s < cb::numPaletteSections; ++s)
        {
            auto& row = newRows[(size_t) s];
            const auto* section = s < (int) palette.size() ? &palette[(size_t) s] : nullptr;

            row.title = (section != nullptr && ! section->title.empty() ? toJuce (section->title)
                                                                        : juce::String (defaultSectionTitle (s))).toUpperCase();
            if (section == nullptr)
                continue;

            for (const auto& pc : section->chords)
            {
                if ((int) row.pads.size() >= kColumns)
                    break;

                Pad pad;
                pad.info = pc;
                pad.name = toJuce (pc.chord.name (key));
                pad.roman = toJuce (pc.roman);
                pad.colour = cb::ui::functionColour (pc.function);
                pad.tooltip = makeTooltip (pad);

                // Keep animation state when the same chord stays in the same place (e.g. refresh after an edit).
                const size_t index = row.pads.size();

                if ((size_t) s < rows.size() && index < rows[(size_t) s].pads.size()
                    && rows[(size_t) s].pads[index].info.chord == pc.chord)
                {
                    const auto& old = rows[(size_t) s].pads[index];
                    pad.hover = old.hover;
                    pad.flash = old.flash;
                    pad.flashError = old.flashError;
                }

                row.pads.push_back (std::move (pad));
            }
        }

        rows = std::move (newRows);

        if (padFor (pressedId) == nullptr)
        {
            pressedId = -1;
            pressedOnPlus = false;
        }

        if (padFor (hoverId) == nullptr)
            hoverId = -1;

        if (padFor (firstClickPad) == nullptr)
            firstClickPad = -1;

        recomputeScores();
        layout();
    }

    juce::String makeTooltip (const Pad& pad) const
    {
        juce::String tip;
        tip << pad.name << "  (" << pad.roman << ")  " << middleDot() << "  " << cb::functionName (pad.info.function);

        if (! pad.info.hint.empty())
            tip << "\n" << toJuce (pad.info.hint);

        return tip;
    }

    //==============================================================================
    // Where a double-click / "+" adds: after the lane's selected slot, else at the end.
    ProgressionLane* findLane() const
    {
        if (auto* editor = findParentComponentOfClass<ChordbloomEditor>())
            return &editor->getProgressionLane();

        return nullptr;
    }

    int selectedSlot() const
    {
        const int n = (int) processor.getProgression().slots.size();

        if (auto* lane = findLane())
        {
            const int sel = lane->getSelectedSlot();
            return sel >= 0 && sel < n ? sel : -1;
        }

        return -1;
    }

    juce::String addPlaceText() const
    {
        const int sel = selectedSlot();

        if (sel < 0)
            return "to the end";

        const auto& slots = processor.getProgression().slots;
        return "after " + toJuce (slots[(size_t) sel].chord.name (key));
    }

    void addChordFromPad (int id)
    {
        auto* pad = padFor (id);

        if (pad == nullptr)
            return;

        const auto chord = pad->info.chord;
        auto* lane = findLane();
        const int n = (int) processor.getProgression().slots.size();
        const int sel = selectedSlot();
        const int at = sel >= 0 && sel < n - 1 ? sel + 1 : -1;   // after the last slot == append
        const bool added = processor.addChord (chord, at);

        if (added && lane != nullptr)
        {
            // The selection follows the new chord, so the glow (context) continues from it. The lane caches the
            // progression and the processor's change message is async: sync it first, then select with notification.
            const int newIndex = at >= 0 ? at : (int) processor.getProgression().slots.size() - 1;
            lane->refresh();
            lane->setSelectedSlot (newIndex, true);
        }

        // addChord() (or the selection change) may have refreshed us synchronously: look the pad up again.
        if (auto* p = padFor (id))
        {
            p->flash = 1.0f;
            p->flashError = ! added && processor.getProgression().slots.size() >= (size_t) cb::kMaxSlots;
            startAnimation();
        }
    }

    //==============================================================================
    void setContext (std::optional<cb::Chord> chord)
    {
        const bool emptyNow = processor.getProgression().empty();

        if (chord == context && emptyNow == scoredEmpty)
            return;

        context = chord;
        recomputeScores();
        repaint();
    }

    void setSounding (std::optional<cb::Chord> chord)
    {
        if (chord == sounding)
            return;

        repaintChord (sounding);
        sounding = chord;
        repaintChord (sounding);
    }

    void recomputeScores()
    {
        // No context and nothing in the lane yet: guide the first chord instead of showing a flat grid.
        scoredEmpty = processor.getProgression().empty();
        const bool opening = ! context.has_value() && scoredEmpty;

        struct Candidate
        {
            float score;
            cb::Chord chord;
        };

        // Best-scoring chord per root, so the top dots mark three different destinations (not F, Fmaj7, Fmaj9).
        std::vector<Candidate> bestPerRoot;

        for (auto& row : rows)
        {
            for (auto& pad : row.pads)
            {
                pad.top = false;

                float score = 0.0f;

                if (context.has_value())
                    score = cb::transitionScore (*context, pad.info.chord, key);
                else if (opening)
                    score = cb::openingScore (pad.info.chord, key);

                pad.score = std::isfinite (score) ? juce::jlimit (0.0f, 1.0f, score) : 0.0f;

                if (pad.score < kMinSuggestScore)
                    continue;

                // The dots mark where to go next: a colour change on the same root (E7 -> E, Am7 -> Am) keeps its
                // glow but never takes one of the three dots.
                if (context.has_value() && pad.info.chord.root == context->root)
                    continue;

                auto it = std::find_if (bestPerRoot.begin(), bestPerRoot.end(),
                                        [&] (const Candidate& c) { return c.chord.root == pad.info.chord.root; });

                if (it == bestPerRoot.end())
                    bestPerRoot.push_back ({ pad.score, pad.info.chord });
                else if (pad.score > it->score)
                    *it = { pad.score, pad.info.chord };
            }
        }

        std::stable_sort (bestPerRoot.begin(), bestPerRoot.end(),
                          [] (const Candidate& a, const Candidate& b) { return a.score > b.score; });

        if (bestPerRoot.size() > (size_t) kTopSuggestions)
            bestPerRoot.resize ((size_t) kTopSuggestions);

        anyGlow = false;

        for (auto& row : rows)
        {
            for (auto& pad : row.pads)
            {
                pad.top = std::any_of (bestPerRoot.begin(), bestPerRoot.end(),
                                       [&] (const Candidate& c) { return c.chord == pad.info.chord; });
                anyGlow = anyGlow || pad.score >= kMinSuggestScore;
            }
        }
    }

    //==============================================================================
    void resized() override { layout(); }

    void layout()
    {
        auto area = getLocalBounds().toFloat().reduced (12.0f, 10.0f);
        const int numRows = (int) rows.size();
        legendArea = {};

        if (numRows == 0 || area.isEmpty())
            return;

        const float labelW = juce::jlimit (72.0f, 100.0f, area.getWidth() * 0.085f);
        const float rowH = std::max (16.0f, (area.getHeight() - kRowGap * (float) (numRows - 1)) / (float) numRows);
        const float cellsX = area.getX() + labelW;
        const float cellsW = area.getRight() - cellsX;
        const float pitch = (cellsW + kPadGap) / (float) kColumns;   // pad width + gap

        const float nameH = juce::jlimit (12.5f, 18.0f, rowH * 0.31f);
        const float romanH = juce::jlimit (10.5f, 12.5f, rowH * 0.22f);

        for (int r = 0; r < numRows; ++r)
        {
            auto& row = rows[(size_t) r];
            const float y0 = std::round (area.getY() + (float) r * (rowH + kRowGap));
            const float y1 = std::round (area.getY() + (float) r * (rowH + kRowGap) + rowH);

            row.bounds = { area.getX(), y0, area.getWidth(), y1 - y0 };
            row.label = row.bounds.withWidth (labelW - 8.0f);
            row.cells = { cellsX, y0, cellsW, y1 - y0 };

            for (size_t i = 0; i < row.pads.size(); ++i)
            {
                auto& pad = row.pads[i];
                const float x0 = std::round (cellsX + (float) i * pitch);
                const float x1 = std::round (cellsX + (float) i * pitch + pitch - kPadGap);
                pad.bounds = { x0, y0, std::max (8.0f, x1 - x0), y1 - y0 };

                const float textW = pad.bounds.getWidth() - 14.0f;
                pad.nameFont = fittedFont (nameH, 600, pad.name, textW, 10.0f);
                pad.romanFont = fittedFont (romanH, 400, pad.roman, textW, 8.5f);
                // Decided on the unrounded row height, so every row agrees (rounding makes rows 1 px apart).
                pad.showRoman = rowH >= 30.0f;
            }
        }

        // Legend: the free cells at the end of the lowest row that has at least two empty columns (the Secondary
        // row never has more than 6 pads, so this is normally the bottom-right corner).
        for (int r = numRows - 1; r >= 0; --r)
        {
            const auto& row = rows[(size_t) r];
            const int used = (int) row.pads.size();

            if (kColumns - used >= 2)
            {
                const float x = used > 0 ? std::round (cellsX + (float) used * pitch) : cellsX + cellsW * 0.5f;
                legendArea = { x, row.bounds.getY(), row.cells.getRight() - x, row.bounds.getHeight() };
                break;
            }
        }

        triggerAsyncUpdate();   // accessible pad proxies follow the layout (never touched from paint)
    }

    //==============================================================================
    // Accessibility: one invisible, click-through proxy per pad.
    void handleAsyncUpdate() override
    {
        size_t count = 0;

        for (const auto& row : rows)
            count += row.pads.size();

        while (proxies.size() > count)
            proxies.pop_back();

        while (proxies.size() < count)
        {
            proxies.push_back (std::make_unique<AccessibleItem>());
            addAndMakeVisible (*proxies.back());
        }

        size_t k = 0;

        forEachPad ([&] (int id, Pad& pad)
        {
            auto& proxy = *proxies[k++];
            proxy.setBounds (pad.bounds.getSmallestIntegerContainer());
            proxy.setTitle (pad.name + " (" + pad.roman + ")");
            proxy.setDescription (juce::String (cb::functionName (pad.info.function)) + ". " + toJuce (pad.info.hint));
            proxy.setHelpText ("Adds " + pad.name + " to the progression");

            juce::Component::SafePointer<Impl> safe (this);
            proxy.onPress = [safe, id]
            {
                if (safe != nullptr)
                    safe->addChordFromPad (id);
            };
        });
    }

    //==============================================================================
    void paint (juce::Graphics& g) override
    {
        if (! matchesProcessor())   // defensive: stay correct even if a refresh() was missed
            rebuild();

        paintCard (g);

        const auto titleFont = cb::ui::font (10.5f, 600).withExtraKerningFactor (0.12f);

        for (const auto& row : rows)
        {
            g.setFont (titleFont);
            g.setColour (Col::textDim);
            g.drawText (row.title, row.label, juce::Justification::centredLeft, true);

            if (row.pads.empty())
            {
                g.setFont (cb::ui::font (13.0f, 400));
                g.setColour (Col::textFaint.withAlpha (0.7f));
                g.drawText (juce::String::charToString ((juce::juce_wchar) 0x2014) + "  none in this key",
                            row.cells.withTrimmedLeft (6.0f), juce::Justification::centredLeft, true);
            }
        }

        paintLegend (g);

        // Halos and rings go first so they sit behind every pad and only show in the gaps.
        forEachPad ([&] (int id, Pad& pad)
        {
            if (g.clipRegionIntersects (pad.bounds.expanded (8.0f).getSmallestIntegerContainer()))
                paintHalo (g, pad, isLit (id, pad));
        });

        forEachPad ([&] (int id, Pad& pad)
        {
            if (! g.clipRegionIntersects (pad.bounds.expanded (4.0f).getSmallestIntegerContainer()))
                return;

            const bool hovered = id == hoverId;
            paintPad (g, pad, pad.bounds, pad.hover, isLit (id, pad), id == pressedId,
                      hovered ? pad.hover : 0.0f, hovered && hoverPlus);
        });
    }

    void paintCard (juce::Graphics& g) const
    {
        const auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setGradientFill (juce::ColourGradient::vertical (Col::panel.brighter (0.035f), r.getY(), Col::panel, r.getBottom()));
        g.fillRoundedRectangle (r, cb::ui::kCornerRadius);
        g.setColour (Col::outline.withAlpha (0.55f));
        g.drawRoundedRectangle (r, cb::ui::kCornerRadius, 1.0f);
    }

    // Small key that explains the glow ring and the dot (bottom-right corner of the grid).
    void paintLegend (juce::Graphics& g) const
    {
        if (! anyGlow || legendArea.getWidth() < 120.0f || legendArea.getHeight() < 16.0f)
            return;

        const bool opening = ! context.has_value();
        const juce::String glowText = opening ? "glow = good first chord" : "glow = good next chord";
        // "top pick", never a count: often fewer than three dots are shown, and the legend must always be true.
        juce::String dotText = "dot = top pick";

        if (context.has_value())
            dotText << " after " << toJuce (context->name (key));

        const auto f = cb::ui::font (11.0f, 400);
        const float swatchW = 18.0f, gap = 7.0f;
        const auto area = legendArea.reduced (2.0f, 0.0f);
        const bool twoLines = area.getHeight() >= 34.0f;

        auto drawGlowSwatch = [&] (juce::Point<float> c)
        {
            const auto r = juce::Rectangle<float> (swatchW - 4.0f, 10.0f).withCentre (c);
            g.setColour (Col::panelRaised);
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (Col::accent.withAlpha (0.18f));
            g.drawRoundedRectangle (r.expanded (2.5f), 5.0f, 1.25f);
            g.setColour (Col::accent.withAlpha (0.85f));
            g.drawRoundedRectangle (r.expanded (1.0f), 4.0f, 1.4f);
        };

        g.setFont (f);

        if (twoLines)
        {
            if (widthOf (f, dotText) + swatchW + gap > area.getWidth())
                dotText = "dot = top pick";

            const float lineH = 16.0f;
            const float textW = std::max (widthOf (f, glowText), widthOf (f, dotText));
            const float blockW = swatchW + gap + textW;

            if (blockW > area.getWidth())
                return;

            const float x = area.getRight() - blockW - 4.0f;
            const float y = std::round (area.getCentreY() - lineH);

            drawGlowSwatch ({ x + swatchW * 0.5f, y + lineH * 0.5f });
            drawTopDot (g, { x + swatchW * 0.5f, y + lineH * 1.5f + 1.0f });

            g.setColour (Col::textDim.withAlpha (0.85f));
            g.drawText (glowText, juce::Rectangle<float> (x + swatchW + gap, y, textW + 2.0f, lineH),
                        juce::Justification::centredLeft, false);
            g.setColour (Col::textFaint.brighter (0.15f));
            g.drawText (dotText, juce::Rectangle<float> (x + swatchW + gap, y + lineH + 1.0f, textW + 2.0f, lineH),
                        juce::Justification::centredLeft, false);
            return;
        }

        const juce::String shortDot ("dot = top pick");
        const float w1 = widthOf (f, glowText), w2 = widthOf (f, shortDot);
        const float blockW = swatchW + gap + w1 + 14.0f + 10.0f + gap + w2;

        if (blockW > area.getWidth())
            return;

        float x = area.getRight() - blockW - 4.0f;
        const float cy = area.getCentreY();
        drawGlowSwatch ({ x + swatchW * 0.5f, cy });
        x += swatchW + gap;
        g.setColour (Col::textDim.withAlpha (0.85f));
        g.drawText (glowText, juce::Rectangle<float> (x, cy - 8.0f, w1 + 2.0f, 16.0f), juce::Justification::centredLeft, false);
        x += w1 + 14.0f;
        drawTopDot (g, { x + 5.0f, cy });
        x += 10.0f + gap;
        g.setColour (Col::textFaint.brighter (0.15f));
        g.drawText (shortDot, juce::Rectangle<float> (x, cy - 8.0f, w2 + 2.0f, 16.0f), juce::Justification::centredLeft, false);
    }

    bool isLit (int id, const Pad& pad) const
    {
        return id == pressedId || (sounding.has_value() && *sounding == pad.info.chord);
    }

    void paintHalo (juce::Graphics& g, const Pad& pad, bool lit) const
    {
        const auto r = pad.bounds;

        if (lit)
            drawHalo (g, r, kPadRadius, pad.colour, 0.30f, 4);

        if (pad.score >= kMinSuggestScore)
        {
            // Graded so the eye finds the strong moves first: weak = faint hairline, strong = bright ring + halo.
            const float t = juce::jlimit (0.0f, 1.0f, juce::jmap (pad.score, kMinSuggestScore, 0.95f, 0.0f, 1.0f));

            if (t > 0.3f)
                drawHalo (g, r, kPadRadius, Col::accent, 0.24f * (t - 0.3f) / 0.7f, 3);

            g.setColour (Col::accent.withAlpha (0.14f + 0.82f * std::pow (t, 1.4f)));
            g.drawRoundedRectangle (r.expanded (1.75f), kPadRadius + 1.75f, 1.0f + 0.9f * t);
        }

        if (pad.flash > 0.0f)
            drawHalo (g, r, kPadRadius, pad.flashError ? Col::dominant : Col::accent, 0.45f * pad.flash, 4);
    }

    static juce::Rectangle<float> plusBounds (juce::Rectangle<float> pad)
    {
        const float d = juce::jlimit (14.0f, 18.0f, pad.getHeight() * 0.34f);
        return { pad.getRight() - d - 4.0f, pad.getY() + kAccentBar + 3.0f, d, d };
    }

    // Draws one pad. Shared with the drag image.
    static void paintPad (juce::Graphics& g, const Pad& pad, juce::Rectangle<float> r, float hover, bool lit, bool pressed,
                          float plusAlpha, bool plusHover)
    {
        const auto colour = pad.colour;

        if (pressed)
            r = r.reduced (1.0f);
        else
            r = r.translated (0.0f, -hover);

        // Lift shadow
        if (hover > 0.0f && ! pressed)
        {
            g.setColour (juce::Colours::black.withAlpha (0.28f * hover));
            g.fillRoundedRectangle (r.translated (0.0f, 2.0f), kPadRadius);
        }

        // Raised base with a gentle vertical gradient
        auto base = Col::panelRaised.interpolatedWith (Col::panelHover, hover);

        if (lit)
            base = base.brighter (0.06f);

        g.setGradientFill (juce::ColourGradient::vertical (base.brighter (0.05f), r.getY(), base.darker (0.08f), r.getBottom()));
        g.fillRoundedRectangle (r, kPadRadius);

        // Function tint
        const float tintTop = lit ? 0.44f : 0.23f + 0.06f * hover;
        const float tintBottom = lit ? 0.24f : 0.07f + 0.04f * hover;
        g.setGradientFill (juce::ColourGradient::vertical (colour.withAlpha (tintTop), r.getY(),
                                                           colour.withAlpha (tintBottom), r.getBottom()));
        g.fillRoundedRectangle (r, kPadRadius);

        // Accent bar along the top, clipped to the rounded shape
        {
            juce::Graphics::ScopedSaveState save (g);
            juce::Path clip;
            clip.addRoundedRectangle (r, kPadRadius);
            g.reduceClipRegion (clip);
            g.setColour (colour.withAlpha (lit ? 1.0f : 0.85f));
            g.fillRect (r.withHeight (kAccentBar));
        }

        // Outline
        const auto edge = lit ? colour.withAlpha (0.85f)
                              : Col::outline.withAlpha (0.8f).interpolatedWith (colour.withAlpha (0.55f), 0.35f + 0.4f * hover);
        g.setColour (edge);
        g.drawRoundedRectangle (r.reduced (0.5f), kPadRadius - 0.5f, 1.0f);

        // Text: chord name, roman numeral below
        const auto content = r.withTrimmedTop (kAccentBar);
        const float nameH = pad.nameFont.getHeight();
        const float romanH = pad.showRoman ? pad.romanFont.getHeight() : 0.0f;
        const float spacing = pad.showRoman ? juce::jlimit (0.0f, 3.0f, (content.getHeight() - nameH - romanH) * 0.12f) : 0.0f;
        const float blockH = nameH + spacing + romanH;
        const float y = std::round (content.getCentreY() - blockH * 0.5f);
        const auto textArea = content.reduced (6.0f, 0.0f);

        g.setFont (pad.nameFont);
        g.setColour (Col::text);
        g.drawText (pad.name, textArea.withY (y).withHeight (nameH), juce::Justification::centred, true);

        if (pad.showRoman)
        {
            g.setFont (pad.romanFont);
            g.setColour (lit ? colour.brighter (0.45f) : Col::textDim);
            g.drawText (pad.roman, textArea.withY (y + nameH + spacing).withHeight (romanH), juce::Justification::centred, true);
        }

        // Top-suggestion dot (hidden while the "+" is showing)
        if (pad.top && plusAlpha < 0.5f)
            drawTopDot (g, { r.getRight() - 9.0f, r.getY() + kAccentBar + 7.0f });

        // "+" add affordance
        if (plusAlpha > 0.01f && r.getWidth() >= 40.0f)
        {
            const auto p = plusBounds (r);
            g.setColour ((plusHover ? Col::accent : Col::background.withAlpha (0.55f)).withMultipliedAlpha (plusAlpha));
            g.fillEllipse (p);
            g.setColour ((plusHover ? Col::accent : Col::outline).withMultipliedAlpha (plusAlpha));
            g.drawEllipse (p.reduced (0.5f), 1.0f);

            const auto c = p.getCentre();
            const float arm = p.getWidth() * 0.24f;
            g.setColour ((plusHover ? Col::background : Col::text).withMultipliedAlpha (plusAlpha));
            g.fillRoundedRectangle (juce::Rectangle<float> (arm * 2.0f, 1.6f).withCentre (c), 0.8f);
            g.fillRoundedRectangle (juce::Rectangle<float> (1.6f, arm * 2.0f).withCentre (c), 0.8f);
        }
    }

    //==============================================================================
    template <typename Fn>
    void forEachPad (Fn&& fn)
    {
        for (size_t r = 0; r < rows.size(); ++r)
            for (size_t i = 0; i < rows[r].pads.size(); ++i)
                fn ((int) (r * (size_t) kColumns + i), rows[r].pads[i]);
    }

    Pad* padFor (int id)
    {
        if (id < 0)
            return nullptr;

        const auto r = (size_t) (id / kColumns);
        const auto i = (size_t) (id % kColumns);
        return r < rows.size() && i < rows[r].pads.size() ? &rows[r].pads[i] : nullptr;
    }

    int padAt (juce::Point<float> p)
    {
        int found = -1;
        forEachPad ([&] (int id, Pad& pad)
        {
            if (found < 0 && pad.bounds.contains (p))
                found = id;
        });
        return found;
    }

    bool plusHit (int id, juce::Point<float> p)
    {
        if (auto* pad = padFor (id))
            return pad->bounds.getWidth() >= 40.0f && plusBounds (pad->bounds).expanded (3.0f).contains (p);

        return false;
    }

    void repaintPad (int id)
    {
        if (auto* pad = padFor (id))
            repaint (pad->bounds.expanded (7.0f).getSmallestIntegerContainer());
    }

    void repaintChord (std::optional<cb::Chord> chord)
    {
        if (! chord.has_value())
            return;

        forEachPad ([&] (int id, Pad& pad)
        {
            if (pad.info.chord == *chord)
                repaintPad (id);
        });
    }

    //==============================================================================
    void updateHover (juce::Point<float> p, bool inside)
    {
        const int id = inside ? padAt (p) : -1;
        const bool plus = id >= 0 && plusHit (id, p);

        if (id != hoverId || plus != hoverPlus)
        {
            repaintPad (hoverId);
            hoverId = id;
            hoverPlus = plus;
            repaintPad (hoverId);
            setMouseCursor (plus ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
            startAnimation();
        }
    }

    void mouseMove (const juce::MouseEvent& e) override  { updateHover (e.position, true); }
    void mouseEnter (const juce::MouseEvent& e) override { updateHover (e.position, true); }
    void mouseExit (const juce::MouseEvent& e) override  { updateHover (e.position, false); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragStarted = false;
        pressedOnPlus = false;
        const int id = padAt (e.position);
        auto* pad = padFor (id);

        if (pad == nullptr || e.mods.isPopupMenu())
            return;

        // Double-clicks only count when both clicks land on the same pad.
        if (e.getNumberOfClicks() == 1)
            firstClickPad = id;

        pressedId = id;
        downPos = e.position;
        pressedOnPlus = plusHit (id, e.position);   // "+" adds on release (if no drag started), see mouseUp

        processor.startPreviewChord (pad->info.chord);
        previewing = true;
        repaintPad (id);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (pressedId < 0 || dragStarted || e.position.getDistanceFrom (downPos) < kDragThreshold)
            return;

        auto* pad = padFor (pressedId);

        if (pad == nullptr)
            return;

        dragStarted = true;
        pressedOnPlus = false;   // a drag that starts on the "+" is just a drag: it must not also add the chord

        if (previewing)
        {
            processor.stopPreview();
            previewing = false;
        }

        const int id = pressedId;
        pressedId = -1;
        repaintPad (id);

        // No hover / "+" decoration on the source pad while it is being dragged around.
        repaintPad (hoverId);
        hoverId = -1;
        hoverPlus = false;
        startAnimation();

        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
        {
            juce::Point<int> offset;
            auto image = makeDragImage (*pad, offset);
            // allowDraggingToExternalWindows: after ~0.7 s outside the plugin the editor turns it into a MIDI file drag.
            container->startDragging (cb::ui::drag::makeChordPayload (pad->info.chord), this, image, true, &offset, &e.source);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (previewing)
        {
            processor.stopPreview();
            previewing = false;
        }

        const int id = pressedId;
        const bool addViaPlus = pressedOnPlus && ! dragStarted && id >= 0 && ! e.mods.isPopupMenu() && plusHit (id, e.position);

        if (pressedId >= 0)
        {
            repaintPad (pressedId);
            pressedId = -1;
        }

        pressedOnPlus = false;
        dragStarted = false;

        if (addViaPlus)
            addChordFromPad (id);

        updateHover (e.position, getLocalBounds().toFloat().contains (e.position));
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        // JUCE also calls this for the 3rd and 4th click of a fast run (sliding window): tapping a pad repeatedly to
        // audition it must add at most one chord.
        if (e.getNumberOfClicks() != 2 || e.mods.isPopupMenu())
            return;

        const auto pos = e.mouseDownPosition;
        const int id = padAt (pos);

        if (id < 0 || id != firstClickPad || plusHit (id, pos))   // the "+" adds on its own clicks
            return;

        addChordFromPad (id);
    }

    juce::ScaledImage makeDragImage (const Pad& pad, juce::Point<int>& offset)
    {
        const float scale = juce::jmax (1.0f, (float) juce::Component::getApproximateScaleFactorForComponent (this));
        const float margin = 12.0f;
        const auto box = pad.bounds.getSmallestIntegerContainer();
        const juce::Point<int> size (box.getWidth(), box.getHeight());

        juce::Image image (juce::Image::ARGB,
                           juce::roundToInt (((float) size.x + 2.0f * margin) * scale),
                           juce::roundToInt (((float) size.y + 2.0f * margin) * scale), true);
        {
            juce::Graphics g (image);
            g.addTransform (juce::AffineTransform::scale (scale));
            const juce::Rectangle<float> r (margin, margin, (float) size.x, (float) size.y);

            juce::Path shape;
            shape.addRoundedRectangle (r.translated (0.0f, 3.0f), kPadRadius);
            juce::DropShadow (juce::Colours::black.withAlpha (0.5f), 10, {}).drawForPath (g, shape);
            drawHalo (g, r, kPadRadius, pad.colour, 0.35f, 4);
            paintPad (g, pad, r, 1.0f, true, false, 0.0f, false);
        }

        offset = (pad.bounds.getTopLeft() - juce::Point<float> (margin, margin) - downPos).roundToInt();
        return juce::ScaledImage (image, (double) scale);
    }

    juce::String getTooltip() override
    {
        if (auto* pad = padFor (hoverId))
        {
            const auto place = addPlaceText();

            if (hoverPlus)
                return "Add " + pad->name + " " + place;

            juce::String tip (pad->tooltip);
            tip << "\nClick to hear " << middleDot() << " double-click to add " << place
                << "\nDrag into the progression, or straight out into FL Studio";
            return tip;
        }

        return {};
    }

    //==============================================================================
    void startAnimation()
    {
        if (! isTimerRunning())
            startTimerHz (60);
    }

    void timerCallback() override
    {
        bool animating = false;

        forEachPad ([&] (int id, Pad& pad)
        {
            bool changed = false;
            const float target = id == hoverId ? 1.0f : 0.0f;

            if (pad.hover != target)
            {
                pad.hover += (target - pad.hover) * 0.35f;

                if (std::abs (target - pad.hover) < 0.02f)
                    pad.hover = target;

                changed = true;
            }

            if (pad.flash > 0.0f)
            {
                pad.flash = pad.flash < 0.03f ? 0.0f : pad.flash * 0.9f;
                changed = true;
            }

            if (changed)
            {
                repaintPad (id);
                animating = true;
            }
        });

        if (! animating)
            stopTimer();
    }

    //==============================================================================
    ChordbloomProcessor& processor;
    cb::Key key;
    std::vector<Row> rows;
    juce::Rectangle<float> legendArea;
    std::vector<std::unique_ptr<AccessibleItem>> proxies;

    std::optional<cb::Chord> context, sounding;
    bool scoredEmpty = true;   // whether the scores were computed for an empty progression
    bool anyGlow = false;

    int hoverId = -1;
    bool hoverPlus = false;
    int pressedId = -1;
    bool pressedOnPlus = false;
    int firstClickPad = -1;
    bool previewing = false;
    bool dragStarted = false;
    juce::Point<float> downPos;
};

//==============================================================================
ChordPaletteView::ChordPaletteView (ChordbloomProcessor& p) : impl (std::make_unique<Impl> (p))
{
    setOpaque (false);
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    setTitle ("Chord palette");
    addAndMakeVisible (*impl);
}

ChordPaletteView::~ChordPaletteView() = default;

void ChordPaletteView::paint (juce::Graphics&) {}   // the Impl surface paints the whole view

void ChordPaletteView::resized() { impl->setBounds (getLocalBounds()); }

void ChordPaletteView::refresh()
{
    if (impl->matchesProcessor())
    {
        // The progression may have become empty (or stopped being empty) without a context change: the glow
        // switches between "good first chords" and "good next chords".
        if (impl->processor.getProgression().empty() != impl->scoredEmpty)
        {
            impl->recomputeScores();
            impl->repaint();
        }

        return;
    }

    impl->rebuild();
    impl->repaint();
}

void ChordPaletteView::setContextChord (std::optional<cb::Chord> chord) { impl->setContext (chord); }
void ChordPaletteView::setSoundingChord (std::optional<cb::Chord> chord) { impl->setSounding (chord); }
