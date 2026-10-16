#include "ui/ChordPaletteView.h"

#include "ui/DragPayload.h"
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

struct Pad
{
    cb::PaletteChord info;
    juce::String name, roman, tooltip;
    juce::Colour colour;
    juce::Rectangle<float> bounds;
    juce::Font nameFont { juce::FontOptions() };
    juce::Font romanFont { juce::FontOptions() };
    bool showRoman = true;

    float score = 0.0f;   // suggestion strength for the current context chord
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
                                private juce::Timer
{
    explicit Impl (ChordbloomProcessor& p) : processor (p)
    {
        setOpaque (false);
        rebuild();
    }

    ~Impl() override
    {
        stopTimer();

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
            pressedId = -1;

        if (padFor (hoverId) == nullptr)
            hoverId = -1;

        recomputeScores();
        layout();
    }

    juce::String makeTooltip (const Pad& pad) const
    {
        juce::String tip;
        tip << pad.name << "  (" << pad.roman << ")  " << middleDot() << "  " << cb::functionName (pad.info.function);

        if (! pad.info.hint.empty())
            tip << "\n" << toJuce (pad.info.hint);

        tip << "\nClick to hear " << middleDot() << " double-click to add " << middleDot() << " drag into the progression";
        return tip;
    }

    //==============================================================================
    void setContext (std::optional<cb::Chord> chord)
    {
        if (chord == context)
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
        struct Candidate
        {
            float score;
            cb::Chord chord;
        };

        std::vector<Candidate> candidates;

        for (auto& row : rows)
        {
            for (auto& pad : row.pads)
            {
                pad.top = false;
                pad.score = context.has_value() ? juce::jlimit (0.0f, 1.0f, cb::transitionScore (*context, pad.info.chord, key)) : 0.0f;

                if (pad.score >= kMinSuggestScore)
                {
                    const bool known = std::any_of (candidates.begin(), candidates.end(),
                                                    [&] (const Candidate& c) { return c.chord == pad.info.chord; });
                    if (! known)
                        candidates.push_back ({ pad.score, pad.info.chord });
                }
            }
        }

        std::stable_sort (candidates.begin(), candidates.end(),
                          [] (const Candidate& a, const Candidate& b) { return a.score > b.score; });

        if (candidates.size() > (size_t) kTopSuggestions)
            candidates.resize ((size_t) kTopSuggestions);

        for (auto& row : rows)
            for (auto& pad : row.pads)
                pad.top = std::any_of (candidates.begin(), candidates.end(),
                                       [&] (const Candidate& c) { return c.chord == pad.info.chord; });
    }

    //==============================================================================
    void resized() override { layout(); }

    void layout()
    {
        auto area = getLocalBounds().toFloat().reduced (12.0f, 10.0f);
        const int numRows = (int) rows.size();

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
                pad.showRoman = pad.bounds.getHeight() >= 32.0f;
            }
        }
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

    bool isLit (int id, const Pad& pad) const
    {
        return id == pressedId || (sounding.has_value() && *sounding == pad.info.chord);
    }

    void paintHalo (juce::Graphics& g, const Pad& pad, bool lit) const
    {
        const auto r = pad.bounds;

        if (lit)
            drawHalo (g, r, kPadRadius, pad.colour, 0.30f, 4);

        if (context.has_value() && pad.score >= kMinSuggestScore)
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
        {
            const auto c = juce::Point<float> (r.getRight() - 9.0f, r.getY() + kAccentBar + 7.0f);
            g.setColour (Col::accent.withAlpha (0.25f));
            g.fillEllipse (juce::Rectangle<float> (9.0f, 9.0f).withCentre (c));
            g.setColour (Col::accent);
            g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre (c));
        }

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
        const int id = padAt (e.position);
        auto* pad = padFor (id);

        if (pad == nullptr || e.mods.isPopupMenu())
            return;

        pressedId = id;
        downPos = e.position;
        const auto chord = pad->info.chord;

        if (plusHit (id, e.position))
            append (id);

        processor.startPreviewChord (chord);
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
            container->startDragging (cb::ui::drag::makeChordPayload (pad->info.chord), this, image, false, &offset, &e.source);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (previewing)
        {
            processor.stopPreview();
            previewing = false;
        }

        if (pressedId >= 0)
        {
            repaintPad (pressedId);
            pressedId = -1;
        }

        dragStarted = false;
        updateHover (e.position, getLocalBounds().toFloat().contains (e.position));
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int id = padAt (e.position);

        if (id < 0 || e.mods.isPopupMenu() || plusHit (id, e.position))   // the "+" already added on mouse down
            return;

        append (id);
    }

    void append (int id)
    {
        auto* pad = padFor (id);

        if (pad == nullptr)
            return;

        const bool added = processor.addChord (pad->info.chord);

        // addChord() may have refreshed us synchronously: look the pad up again.
        if (auto* p = padFor (id))
        {
            p->flash = 1.0f;
            p->flashError = ! added && processor.getProgression().slots.size() >= (size_t) cb::kMaxSlots;
            startAnimation();
        }
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
            return hoverPlus ? "Add " + pad->name + " to the end of the progression" : pad->tooltip;

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

    std::optional<cb::Chord> context, sounding;

    int hoverId = -1;
    bool hoverPlus = false;
    int pressedId = -1;
    bool previewing = false;
    bool dragStarted = false;
    juce::Point<float> downPos;
};

//==============================================================================
ChordPaletteView::ChordPaletteView (ChordbloomProcessor& p) : impl (std::make_unique<Impl> (p))
{
    setOpaque (false);
    addAndMakeVisible (*impl);
}

ChordPaletteView::~ChordPaletteView() = default;

void ChordPaletteView::paint (juce::Graphics&) {}   // the Impl surface paints the whole view

void ChordPaletteView::resized() { impl->setBounds (getLocalBounds()); }

void ChordPaletteView::refresh()
{
    if (impl->matchesProcessor())
        return;

    impl->rebuild();
    impl->repaint();
}

void ChordPaletteView::setContextChord (std::optional<cb::Chord> chord) { impl->setContext (chord); }
void ChordPaletteView::setSoundingChord (std::optional<cb::Chord> chord) { impl->setSounding (chord); }
