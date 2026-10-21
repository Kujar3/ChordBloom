#include "ui/RollToolbar.h"

#include "ui/Theme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

// Roll toolbar (final-spec 7.2-7.5). Every item is a painted, hit-tested region of this one component (no children),
// laid out at the first of six shrink levels whose measured widths fit the bounds MelodyRoll gives it.
//
// Contract note: RollToolbar.h declares no mouse overrides and no juce::TooltipClient, so the Impl handles the mouse
// as a juce::MouseListener registered on the toolbar (real events reach it exactly like overrides would) and shows its
// own hover tips (same look as the editor's TooltipWindow). Synthetic test events go through
// cb::ui::rolltoolbar::sendMouseEvent below (calling toolbar.mouseDown() directly would reach Component's empty default).

namespace
{
namespace Col = cb::ui::Colours;
using Item = RollToolbarItem;
constexpr int kNumItems = (int) RollToolbarItem::NumItems;

juce::String utf8 (const char* s) { return juce::String (juce::CharPointer_UTF8 (s)); }
const juce::String& middleDot() { static const juce::String s = utf8 ("\xc2\xb7"); return s; }

float widthOf (const juce::Font& f, const juce::String& s) { return juce::GlyphArrangement::getStringWidth (f, s); }

juce::Font tabFont()    { return cb::ui::font (11.5f, 600); }
juce::Font segFont()    { return cb::ui::font (11.5f, 600); }
juce::Font labelFont()  { return cb::ui::font (11.5f, 400); }
juce::Font buttonFont() { return cb::ui::font (12.0f, 600); }
juce::Font detailFont() { return cb::ui::font (12.0f, 400); }

// State-tree properties (final-spec 3.3)
constexpr const char* kGenStyle      = "cb.gen.style";
constexpr const char* kGenDensity    = "cb.gen.density";
constexpr const char* kGenRepetition = "cb.gen.repetition";
constexpr const char* kGenRegister   = "cb.gen.register";
constexpr const char* kGenContour    = "cb.gen.contour";
constexpr const char* kGenResolve    = "cb.gen.resolve";
constexpr const char* kGenSeed       = "cb.gen.seed";
constexpr const char* kSparkMood     = "cbSparkMood";   // ProgressionLane's Spark mood (default Emotional)

// Generator menu values (final-spec 5.4.1)
struct NamedFloat { const char* name; float value; };
struct NamedInt { const char* name; int value; };
constexpr NamedFloat kDensities[]    = { { "Sparse", 0.15f }, { "Medium", 0.5f }, { "Busy", 0.85f } };
constexpr NamedFloat kRepetitions[]  = { { "Loop-like", 0.85f }, { "Balanced", 0.5f }, { "Varied", 0.15f } };
constexpr NamedInt kRanges[]         = { { "Low", -5 }, { "Mid", 0 }, { "High", 5 } };
constexpr NamedFloat kGatePresets[]  = { { "Pluck", 0.45f }, { "Short", 0.65f }, { "Normal", 0.9f }, { "Legato", 1.0f } };
constexpr const char* kOctaveNames[] = { "2 down", "1 down", "Normal", "1 up", "2 up" };

// Menu item ids
enum MenuIds
{
    idArpShape = 100, idArpRate = 200, idArpRhythm = 300, idArpGate = 400, idArpGateCustom = 499, idArpOctave = 500,
    idBassMode = 600,
    idGenStyle = 100, idGenDensity = 200, idGenRange = 300, idGenContour = 400, idGenRepetition = 500,
    idGenResolve = 600, idGenNew = 700, idGenAgain = 701
};

//==============================================================================
// Parameters
float rawParam (const ChordbloomProcessor& p, const char* id, float fallback)
{
    if (auto* v = const_cast<ChordbloomProcessor&> (p).getAPVTS().getRawParameterValue (id))
        return v->load();

    return fallback;
}

int intParam (const ChordbloomProcessor& p, const char* id, int lo, int hi, int fallback)
{
    return juce::jlimit (lo, hi, (int) std::lround (rawParam (p, id, (float) fallback)));
}

// A real user pick: begin / setValueNotifyingHost / end (the raw value is current at once).
void setParamAsUser (ChordbloomProcessor& p, const char* id, float plainValue)
{
    if (auto* param = p.getAPVTS().getParameter (id))
    {
        const float normalised = param->convertTo0to1 (plainValue);

        if (param->getValue() == normalised)
            return;

        param->beginChangeGesture();
        param->setValueNotifyingHost (normalised);
        param->endChangeGesture();
    }
}

cb::ArpShape currentShape (const ChordbloomProcessor& p)
{
    return (cb::ArpShape) intParam (p, cb::ParamIDs::arpShape, 0, cb::numArpShapes - 1, 0);
}

cb::RateDivision currentArpRate (const ChordbloomProcessor& p)
{
    return (cb::RateDivision) intParam (p, cb::ParamIDs::arpRate, 0, cb::numRateDivisions - 1, 2);
}

cb::BassMode currentBassMode (const ChordbloomProcessor& p)
{
    return (cb::BassMode) intParam (p, cb::ParamIDs::bassMode, 0, cb::numBassModes - 1, 1);
}

// The rate suffix applies to the arp and to the bass modes that follow Rate (Octaves, Pulse).
bool patternFollowsRate (cb::Layer layer, cb::BassMode mode)
{
    return layer == cb::Layer::Arp
        || (layer == cb::Layer::Bass && (mode == cb::BassMode::Octaves || mode == cb::BassMode::Pulse));
}

juce::String patternName (const ChordbloomProcessor& p, cb::Layer layer)
{
    if (layer == cb::Layer::Arp)
        return cb::arpShapeName (currentShape (p));

    return cb::bassModeName (currentBassMode (p));
}

juce::String rateSuffix (const ChordbloomProcessor& p, cb::Layer layer)
{
    if (layer == cb::Layer::Arp)
        return cb::rateDivisionName (currentArpRate (p));

    return cb::rateDivisionName ((cb::RateDivision) intParam (p, cb::ParamIDs::rate, 0, cb::numRateDivisions - 1, 1));
}

juce::String layerWord (cb::Layer layer) { return juce::String (cb::layerName (layer)).toLowerCase(); }

//==============================================================================
// Icons
void drawChevronDown (juce::Graphics& g, juce::Point<float> c, juce::Colour colour)
{
    juce::Path p;
    p.startNewSubPath (c.x - 3.5f, c.y - 1.6f);
    p.lineTo (c.x, c.y + 1.9f);
    p.lineTo (c.x + 3.5f, c.y - 1.6f);
    g.setColour (colour);
    g.strokePath (p, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void drawChevronSide (juce::Graphics& g, juce::Point<float> c, bool right, juce::Colour colour)
{
    const float d = right ? 1.0f : -1.0f;
    juce::Path p;
    p.startNewSubPath (c.x - 1.8f * d, c.y - 4.0f);
    p.lineTo (c.x + 2.0f * d, c.y);
    p.lineTo (c.x - 1.8f * d, c.y + 4.0f);
    g.setColour (colour);
    g.strokePath (p, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Path sparklePath (juce::Rectangle<float> r)
{
    auto star = [] (juce::Path& p, juce::Point<float> c, float rad)
    {
        const float w = rad * 0.28f;
        p.startNewSubPath (c.x, c.y - rad);
        p.quadraticTo (c.x + w, c.y - w, c.x + rad, c.y);
        p.quadraticTo (c.x + w, c.y + w, c.x, c.y + rad);
        p.quadraticTo (c.x - w, c.y + w, c.x - rad, c.y);
        p.quadraticTo (c.x - w, c.y - w, c.x, c.y - rad);
        p.closeSubPath();
    };

    const float s = std::min (r.getWidth(), r.getHeight());
    const auto c = r.getCentre();
    juce::Path p;
    star (p, { c.x - s * 0.08f, c.y + s * 0.06f }, s * 0.42f);
    star (p, { c.x + s * 0.34f, c.y - s * 0.32f }, s * 0.17f);
    return p;
}

// Circular arrow (Reset, "Generate again"): an open arc with a head at its clockwise end.
juce::Path resetArrowPath (juce::Rectangle<float> r)
{
    const float s = std::min (r.getWidth(), r.getHeight());
    const auto c = r.getCentre();
    const float rad = s * 0.36f;
    const float start = juce::MathConstants<float>::pi * 0.30f, end = juce::MathConstants<float>::pi * 1.95f;
    juce::Path p;
    p.addCentredArc (c.x, c.y, rad, rad, 0.0f, start, end, true);

    const juce::Point<float> tip (c.x + rad * std::sin (end), c.y - rad * std::cos (end));
    const float a = end + juce::MathConstants<float>::halfPi;   // tangent direction (clockwise)
    const juce::Point<float> dir (std::sin (a), -std::cos (a));
    const juce::Point<float> normal (-dir.y, dir.x);
    const float len = s * 0.26f;
    p.startNewSubPath (tip - dir * len + normal * len * 0.75f);
    p.lineTo (tip);
    p.lineTo (tip - dir * len - normal * len * 0.75f);
    return p;
}

void drawResetIcon (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour colour)
{
    g.setColour (colour);
    g.strokePath (resetArrowPath (r), juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

// Small pencil: the "edited" mark.
void drawPencil (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour colour)
{
    const float s = std::min (r.getWidth(), r.getHeight());
    const auto c = r.getCentre();
    juce::Path body;
    body.addRoundedRectangle (-s * 0.24f, -s * 0.17f, s * 0.80f, s * 0.34f, s * 0.07f);
    body.addTriangle (-s * 0.30f, -s * 0.17f, -s * 0.30f, s * 0.17f, -s * 0.62f, 0.0f);
    body.applyTransform (juce::AffineTransform::rotation (-juce::MathConstants<float>::pi * 0.25f).translated (c.x, c.y));
    g.setColour (colour);
    g.fillPath (body);
}

// Velocity lane: three lollipop stalks.
void drawLollipops (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour colour)
{
    const auto c = r.getCentre();
    const float base = c.y + 5.5f;
    const float heights[] = { 6.0f, 10.0f, 7.5f };
    g.setColour (colour);

    for (int i = 0; i < 3; ++i)
    {
        const float x = c.x + (float) (i - 1) * 4.5f;
        g.fillRect (juce::Rectangle<float> (x - 0.6f, base - heights[i], 1.2f, heights[i]));
        g.fillEllipse (juce::Rectangle<float> (3.6f, 3.6f).withCentre ({ x, base - heights[i] }));
    }
}

void drawDots (juce::Graphics& g, juce::Point<float> c, juce::Colour colour)
{
    g.setColour (colour);

    for (int i = -1; i <= 1; ++i)
        g.fillEllipse (juce::Rectangle<float> (3.0f, 3.0f).withCentre ({ c.x + (float) i * 4.5f, c.y }));
}

void paintInset (juce::Graphics& g, juce::Rectangle<float> b, float radius)
{
    g.setColour (Col::background.brighter (0.035f));
    g.fillRoundedRectangle (b, radius);
    g.setColour (Col::outline.withAlpha (0.9f));
    g.drawRoundedRectangle (b.reduced (0.5f), radius, 1.0f);
}

void paintSelectedSegment (juce::Graphics& g, juce::Rectangle<float> seg, float radius)
{
    g.setGradientFill (juce::ColourGradient::vertical (Col::panelHover.brighter (0.06f), seg.getY(),
                                                       Col::panelHover.darker (0.04f), seg.getBottom()));
    g.fillRoundedRectangle (seg, radius);
    g.setColour (Col::accent.withAlpha (0.35f));
    g.drawRoundedRectangle (seg.reduced (0.5f), radius, 1.0f);
}

// Raised pill surface (browser, Reset, velocity toggle, "...").
void paintRaised (juce::Graphics& g, juce::Rectangle<float> r, float radius, bool hot)
{
    const auto base = hot ? Col::panelHover : Col::panelRaised;
    g.setGradientFill (juce::ColourGradient::vertical (base.brighter (0.04f), r.getY(), base.darker (0.08f), r.getBottom()));
    g.fillRoundedRectangle (r, radius);
    g.setColour (hot ? Col::outline.brighter (0.25f) : Col::outline);
    g.drawRoundedRectangle (r.reduced (0.5f), radius - 0.5f, 1.0f);
}

//==============================================================================
// Style item of the generator menu: the name plus its description line.
class StyleMenuItem final : public juce::PopupMenu::CustomComponent
{
public:
    StyleMenuItem (const juce::String& n, const juce::String& d, bool isTicked)
        : juce::PopupMenu::CustomComponent (true), name (n), description (d), ticked (isTicked)
    {
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
    }

    void getIdealSize (int& idealWidth, int& idealHeight) override
    {
        const float textW = std::max (widthOf (cb::ui::font (13.5f), name), widthOf (cb::ui::font (11.5f), description));
        idealWidth = (int) std::ceil (textW) + 4 + 22 + 18;
        idealHeight = 42;
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (4.0f, 1.0f);

        if (isItemHighlighted())
        {
            g.setColour (Col::panelHover);
            g.fillRoundedRectangle (r, 5.0f);
        }

        auto tickArea = r.removeFromLeft (22.0f);
        const float nameY = r.getY() + 4.0f;

        if (ticked)
        {
            const auto c = juce::Point<float> (tickArea.getCentreX() + 2.0f, nameY + 9.0f);
            juce::Path tick;
            tick.startNewSubPath (c.x - 3.5f, c.y + 0.2f);
            tick.lineTo (c.x - 1.0f, c.y + 2.8f);
            tick.lineTo (c.x + 3.8f, c.y - 2.8f);
            g.setColour (Col::accent);
            g.strokePath (tick, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        g.setFont (cb::ui::font (13.5f));
        g.setColour (ticked ? Col::accent.brighter (0.25f) : Col::text.withAlpha (0.92f));
        g.drawText (name, juce::Rectangle<float> (r.getX(), nameY, r.getWidth(), 18.0f), juce::Justification::centredLeft, true);
        g.setFont (cb::ui::font (11.5f));
        g.setColour (Col::textDim);
        g.drawText (description, juce::Rectangle<float> (r.getX(), nameY + 18.0f, r.getWidth(), 15.0f),
                    juce::Justification::centredLeft, true);
    }

private:
    juce::String name, description;
    bool ticked = false;
};

std::unique_ptr<juce::Drawable> iconDrawable (const juce::Path& path, bool stroke)
{
    auto d = std::make_unique<juce::DrawablePath>();
    d->setPath (path);

    if (stroke)
    {
        d->setFill (juce::FillType (juce::Colours::transparentBlack));
        d->setStrokeFill (juce::FillType (Col::accent));
        d->setStrokeType (juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    else
    {
        d->setFill (juce::FillType (Col::accent));
    }

    return d;
}

//==============================================================================
// A hover tip drawn like the editor's tooltips (RollToolbar is not a juce::TooltipClient, see the note at the top).
class HoverTip final : public juce::Component
{
public:
    HoverTip()
    {
        setInterceptsMouseClicks (false, false);
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
        setAlwaysOnTop (true);
    }

    juce::String text;

    void paint (juce::Graphics& g) override { getLookAndFeel().drawTooltip (g, text, getWidth(), getHeight()); }
};

// What the test hooks below need from an Impl (Impl itself is private to RollToolbar).
struct ToolbarHooks : public juce::MouseListener
{
    virtual juce::String tooltipAt (juce::Point<float>) = 0;
    virtual juce::String browserText() = 0;   // the browser's name + rate suffix as painted ("" on the Melody tab)
};

std::map<const juce::Component*, ToolbarHooks*>& hookRegistry()   // message thread only
{
    static std::map<const juce::Component*, ToolbarHooks*> registry;
    return registry;
}
} // namespace

//==============================================================================
// Menu builders, result handlers and test hooks. Not in the frozen header: SelfTestShell.cpp declares them to inspect
// what the async menus show (final-spec 7.4 / 7.5) and to send synthetic mouse events.
namespace cb::ui::rolltoolbar
{
juce::PopupMenu createPatternMenu (ChordbloomProcessor& p, cb::Layer layer)
{
    juce::PopupMenu menu;

    if (layer == cb::Layer::Arp)
    {
        const auto shape = currentShape (p);
        juce::String family;

        for (int i = 0; i < cb::numArpShapes; ++i)
        {
            const auto s = (cb::ArpShape) i;

            if (family != cb::arpShapeFamily (s))
            {
                family = cb::arpShapeFamily (s);
                menu.addSectionHeader (family);
            }

            juce::PopupMenu::Item item (cb::arpShapeName (s));
            item.itemID = idArpShape + i;
            item.isTicked = s == shape;

            if (s == cb::ArpShape::Alternate13 || s == cb::ArpShape::Alternate15)
                item.shortcutKeyDescription = "1/8";   // grey: a suggestion only, the shape never changes the rate

            menu.addItem (std::move (item));
        }

        menu.addSeparator();

        juce::PopupMenu rates, rhythms, gates, octaves;
        const auto rate = currentArpRate (p);

        for (int i = 0; i < cb::numRateDivisions; ++i)
            rates.addItem (idArpRate + i, cb::rateDivisionName ((cb::RateDivision) i), true, (int) rate == i);

        const int rhythm = intParam (p, cb::ParamIDs::arpRhythm, 0, cb::numArpRhythms - 1, 0);

        for (int i = 0; i < cb::numArpRhythms; ++i)
            rhythms.addItem (idArpRhythm + i, cb::arpRhythmName ((cb::ArpRhythm) i), true, rhythm == i);

        const float gate = rawParam (p, cb::ParamIDs::arpGate, 0.9f);
        bool gateTicked = false;

        for (int i = 0; i < (int) std::size (kGatePresets); ++i)
        {
            const bool tick = std::abs (gate - kGatePresets[i].value) <= 0.02f + 1.0e-6f;
            gateTicked = gateTicked || tick;
            gates.addItem (idArpGate + i, juce::String (kGatePresets[i].name) + " "
                                              + juce::String (juce::roundToInt (kGatePresets[i].value * 100.0f)) + "%",
                           true, tick);
        }

        if (! gateTicked)
            gates.addItem (idArpGateCustom, "Custom " + juce::String (juce::roundToInt (gate * 100.0f)) + "%", false, true);

        const int octave = intParam (p, cb::ParamIDs::arpOctave, -2, 2, 0);

        for (int i = 0; i < 5; ++i)
            octaves.addItem (idArpOctave + i, kOctaveNames[i], true, octave == i - 2);

        auto addSub = [&menu] (const juce::String& name, juce::PopupMenu sub, const juce::String& current)
        {
            juce::PopupMenu::Item item (name);
            item.subMenu = std::make_unique<juce::PopupMenu> (std::move (sub));
            item.shortcutKeyDescription = current;
            menu.addItem (std::move (item));
        };

        addSub ("Rate", std::move (rates), cb::rateDivisionName (rate));
        addSub ("Rhythm", std::move (rhythms), cb::arpRhythmName ((cb::ArpRhythm) rhythm));
        addSub ("Gate", std::move (gates), juce::String (juce::roundToInt (gate * 100.0f)) + "%");
        addSub ("Octave", std::move (octaves), kOctaveNames[octave + 2]);
        return menu;
    }

    const auto mode = currentBassMode (p);
    menu.addItem (idBassMode, "Off", true, mode == cb::BassMode::Off);
    juce::String family;

    for (auto m : cb::bassModeBrowseOrder())
    {
        if (family != cb::bassModeFamily (m))
        {
            family = cb::bassModeFamily (m);
            menu.addSectionHeader (family);
        }

        juce::PopupMenu::Item item (cb::bassModeName (m));
        item.itemID = idBassMode + (int) m;
        item.isTicked = m == mode;

        if (m == cb::BassMode::OctaveBounce8 || m == cb::BassMode::Driving8)
            item.shortcutKeyDescription = "fixed 8ths";

        menu.addItem (std::move (item));
    }

    return menu;
}

// A pattern menu result as a real user pick: the parameter first (host gesture), then beginUserLayerPick (bass Off only
// sets Off). false for 0 / unknown ids.
bool applyPatternMenuResult (ChordbloomProcessor& p, cb::Layer layer, int result)
{
    namespace id = cb::ParamIDs;

    if (layer == cb::Layer::Arp)
    {
        if (result >= idArpShape && result < idArpShape + cb::numArpShapes)
            setParamAsUser (p, id::arpShape, (float) (result - idArpShape));
        else if (result >= idArpRate && result < idArpRate + cb::numRateDivisions)
            setParamAsUser (p, id::arpRate, (float) (result - idArpRate));
        else if (result >= idArpRhythm && result < idArpRhythm + cb::numArpRhythms)
            setParamAsUser (p, id::arpRhythm, (float) (result - idArpRhythm));
        else if (result >= idArpGate && result < idArpGate + (int) std::size (kGatePresets))
            setParamAsUser (p, id::arpGate, kGatePresets[result - idArpGate].value);
        else if (result >= idArpOctave && result < idArpOctave + 5)
            setParamAsUser (p, id::arpOctave, (float) (result - idArpOctave - 2));
        else
            return false;

        p.beginUserLayerPick (cb::Layer::Arp);
        return true;
    }

    if (layer != cb::Layer::Bass || result < idBassMode || result >= idBassMode + cb::numBassModes)
        return false;

    const int mode = result - idBassMode;
    setParamAsUser (p, id::bassMode, (float) mode);

    if (mode != (int) cb::BassMode::Off)
        p.beginUserLayerPick (cb::Layer::Bass);

    return true;
}

juce::PopupMenu createGeneratorMenu (ChordbloomProcessor& p, const cb::MelodyGenSettings& s)
{
    const bool hasChords = ! p.getProgression().empty();
    const bool seedStored = (int) p.getAPVTS().state.getProperty (kGenSeed, 0) != 0;

    juce::PopupMenu menu;
    menu.addSectionHeader ("Melody generator");

    juce::PopupMenu styles, densities, ranges, contours, repetitions;

    for (int i = 0; i < cb::numMelodyStyles; ++i)
    {
        const auto st = (cb::MelodyStyle) i;
        juce::PopupMenu::Item item (cb::melodyStyleName (st));
        item.itemID = idGenStyle + i;
        item.isTicked = st == s.style;
        item.customComponent = new StyleMenuItem (cb::melodyStyleName (st), cb::melodyStyleDescription (st), item.isTicked);
        styles.addItem (std::move (item));
    }

    auto near = [] (float value, float target) { return std::abs (value - target) <= 0.02f; };
    juce::String densityName = "Custom", rangeName = "Custom", repetitionName = "Custom";

    for (int i = 0; i < (int) std::size (kDensities); ++i)
    {
        const bool tick = near (s.density, kDensities[i].value);
        densityName = tick ? juce::String (kDensities[i].name) : densityName;
        densities.addItem (idGenDensity + i, kDensities[i].name, true, tick);
    }

    for (int i = 0; i < (int) std::size (kRanges); ++i)
    {
        const bool tick = s.registerShift == kRanges[i].value;
        rangeName = tick ? juce::String (kRanges[i].name) : rangeName;
        ranges.addItem (idGenRange + i, kRanges[i].name, true, tick);
    }

    for (int i = 0; i < cb::numMelodyContours; ++i)
        contours.addItem (idGenContour + i, cb::melodyContourName ((cb::MelodyContour) i), true, (int) s.contour == i);

    for (int i = 0; i < (int) std::size (kRepetitions); ++i)
    {
        const bool tick = near (s.repetition, kRepetitions[i].value);
        repetitionName = tick ? juce::String (kRepetitions[i].name) : repetitionName;
        repetitions.addItem (idGenRepetition + i, kRepetitions[i].name, true, tick);
    }

    auto addSub = [&menu] (const juce::String& name, juce::PopupMenu sub, const juce::String& current)
    {
        juce::PopupMenu::Item item (name);
        item.subMenu = std::make_unique<juce::PopupMenu> (std::move (sub));
        item.shortcutKeyDescription = current;
        menu.addItem (std::move (item));
    };

    addSub ("Style", std::move (styles), cb::melodyStyleShortName (s.style));
    addSub ("Density", std::move (densities), densityName);
    addSub ("Range", std::move (ranges), rangeName);
    addSub ("Contour", std::move (contours), cb::melodyContourName (s.contour));
    addSub ("Repetition", std::move (repetitions), repetitionName);
    menu.addItem (idGenResolve, "End on the home note", true, s.resolveEnding);
    menu.addSeparator();

    juce::PopupMenu::Item fresh ("New melody");
    fresh.itemID = idGenNew;
    fresh.isEnabled = hasChords;
    fresh.image = iconDrawable (sparklePath ({ 0.0f, 0.0f, 13.0f, 13.0f }), false);
    menu.addItem (std::move (fresh));

    juce::PopupMenu::Item again ("Generate again (same idea)");
    again.itemID = idGenAgain;
    again.isEnabled = hasChords && seedStored;
    again.image = iconDrawable (resetArrowPath ({ 0.0f, 0.0f, 13.0f, 13.0f }), true);
    menu.addItem (std::move (again));
    return menu;
}

// A generator menu result through the toolbar's public API (a settings pick regenerates an untouched result).
void applyGeneratorMenuResult (RollToolbar& toolbar, ChordbloomProcessor& p, int result)
{
    auto s = toolbar.getGenerateSettings();

    if (result >= idGenStyle && result < idGenStyle + cb::numMelodyStyles)
        s.style = (cb::MelodyStyle) (result - idGenStyle);
    else if (result >= idGenDensity && result < idGenDensity + (int) std::size (kDensities))
        s.density = kDensities[result - idGenDensity].value;
    else if (result >= idGenRange && result < idGenRange + (int) std::size (kRanges))
        s.registerShift = kRanges[result - idGenRange].value;
    else if (result >= idGenContour && result < idGenContour + cb::numMelodyContours)
        s.contour = (cb::MelodyContour) (result - idGenContour);
    else if (result >= idGenRepetition && result < idGenRepetition + (int) std::size (kRepetitions))
        s.repetition = kRepetitions[result - idGenRepetition].value;
    else if (result == idGenResolve)
        s.resolveEnding = ! s.resolveEnding;
    else if (result == idGenNew)
    {
        toolbar.generateNewMelody();
        return;
    }
    else if (result == idGenAgain)
    {
        const auto seed = (std::uint32_t) (int) p.getAPVTS().state.getProperty (kGenSeed, 0);

        if (seed != 0)
            toolbar.generateMelody (seed);

        return;
    }
    else
    {
        return;
    }

    toolbar.setGenerateSettings (s);
}

// Where a hover tip goes: the editor, like the editor's own TooltipWindow. Never the top-level component: in the
// VST3 wrapper that is JUCE's ContentWrapperComponent, whose childBoundsChanged sends the host a resizeView for any
// child (so every tip shown would). The top level only without an editor around (a bare toolbar in a test). Also a
// test hook.
juce::Component* tipParentFor (juce::Component& c)
{
    if (auto* editor = c.findParentComponentOfClass<juce::AudioProcessorEditor>())
        return editor;

    return c.getTopLevelComponent();
}

// Test hook: a synthetic mouse event, routed exactly like a real one. kind: 0 down, 1 up, 2 move, 3 exit, 4 drag.
void sendMouseEvent (RollToolbar& toolbar, const juce::MouseEvent& e, int kind)
{
    const auto it = hookRegistry().find (&toolbar);

    if (it == hookRegistry().end())
        return;

    switch (kind)
    {
        case 0:  it->second->mouseDown (e); break;
        case 1:  it->second->mouseUp (e); break;
        case 2:  it->second->mouseMove (e); break;
        case 3:  it->second->mouseExit (e); break;
        default: it->second->mouseDrag (e); break;
    }
}

// Test hook: the pattern browser's text as painted, e.g. "Up 3 (middle dot) 1/16" ("" on the Melody tab).
juce::String getBrowserText (RollToolbar& toolbar)
{
    const auto it = hookRegistry().find (&toolbar);
    return it == hookRegistry().end() ? juce::String() : it->second->browserText();
}

// Test hook: the tip the toolbar shows when hovering at `position` (toolbar coordinates).
juce::String getTooltipAt (RollToolbar& toolbar, juce::Point<float> position)
{
    const auto it = hookRegistry().find (&toolbar);
    return it == hookRegistry().end() ? juce::String() : it->second->tooltipAt (position);
}
} // namespace cb::ui::rolltoolbar

//==============================================================================
struct RollToolbar::Impl final : public ToolbarHooks,
                                 private juce::Timer
{
    Impl (RollToolbar& o, ChordbloomProcessor& p) : owner (o), processor (p)
    {
        // Measured widths (final-spec 7.2: the design values, widened only if the text needs more at the toolbar font).
        auto tabWidth = [] (const char* name, float design, bool mark)
        {
            return std::max (design, std::ceil (20.0f + widthOf (tabFont(), name) + (mark ? 3.0f + 8.0f : 0.0f) + 6.0f));
        };

        float widestStyle = 0.0f;

        for (int i = 0; i < cb::numMelodyStyles; ++i)
            widestStyle = std::max (widestStyle, widthOf (detailFont(), cb::melodyStyleShortName ((cb::MelodyStyle) i)));

        stylePartW = (int) std::max (78.0f, std::ceil (9.0f + widestStyle + 4.0f + 9.0f + 8.0f));   // pad, text, gap, chevron, pad
        generateBodyW = (int) std::max (98.0f, std::ceil (12.0f + 13.0f + 6.0f + widthOf (buttonFont(), "Generate") + 12.0f));
        resetFullW = (int) std::max (74.0f, std::ceil (12.0f + 12.0f + 6.0f + widthOf (buttonFont(), "Reset") + 12.0f));
        tabFull = { tabWidth ("Melody", 74.0f, false), tabWidth ("Arp", 54.0f, true), tabWidth ("Bass", 60.0f, true) };
        tabCompact = { tabWidth ("Melody", 66.0f, false), tabWidth ("Arp", 50.0f, true), tabWidth ("Bass", 54.0f, true) };
        // The compact segments drop some padding: never wider than the full ones.
        for (size_t i = 0; i < 3; ++i)
            tabCompact[i] = std::min (tabCompact[i], tabFull[i]);

        hookRegistry()[&owner] = this;
    }

    ~Impl() override
    {
        stopTimer();
        hideTip();
        hookRegistry().erase (&owner);
    }

    RollToolbar& owner;
    ChordbloomProcessor& processor;

    cb::Layer layer = cb::Layer::Melody;
    cb::MelodySnap snap = cb::MelodySnap::Scale;
    cb::RateDivision grid = cb::RateDivision::Eighth;
    bool velocityLaneOn = false;

    struct State
    {
        std::array<bool, 3> on {}, custom {};
        bool hasChords = false;
        int arpShape = 0, arpRate = 2, bassMode = 1, chordRate = 1, style = 1;

        bool operator== (const State& o) const
        {
            return on == o.on && custom == o.custom && hasChords == o.hasChords && arpShape == o.arpShape
                && arpRate == o.arpRate && bassMode == o.bassMode && chordRate == o.chordRate && style == o.style;
        }
        bool operator!= (const State& o) const { return ! (*this == o); }
    };
    State state;

    // Measured widths
    int stylePartW = 78, generateBodyW = 98, resetFullW = 74;
    std::array<float, 3> tabFull {}, tabCompact {};

    // Layout result
    int level = 0;
    std::array<juce::Rectangle<int>, kNumItems> bounds;
    juce::Rectangle<int> snapCaption, gridCaption;
    bool compactTabs = false, browserCompact = false, resetIconOnly = false, styleText = true, generateIconOnly = false;

    int hoverItem = -1, hoverSegment = -1, pressedItem = -1;
    std::array<bool, 3> hintShown {};

    // Hover tip
    std::unique_ptr<HoverTip> tip;
    juce::Point<float> tipMouse;
    juce::uint32 hoverSince = 0;
    bool tipSuppressed = false;   // after a click, until the mouse moves to another item

    //==============================================================================
    static int index (Item i) { return (int) i; }
    juce::Rectangle<int>& at (Item i) { return bounds[(size_t) index (i)]; }
    const juce::Rectangle<int>& at (Item i) const { return bounds[(size_t) index (i)]; }

    bool showsReset (cb::Layer l) const { return l != cb::Layer::Melody && state.custom[(size_t) l]; }

    State readState() const
    {
        State s;

        for (int i = 0; i < 3; ++i)
        {
            s.on[(size_t) i] = processor.isLayerOn ((cb::Layer) i);
            s.custom[(size_t) i] = processor.isLayerCustom ((cb::Layer) i);
        }

        s.hasChords = ! processor.getProgression().empty();
        s.arpShape = (int) currentShape (processor);
        s.arpRate = (int) currentArpRate (processor);
        s.bassMode = (int) currentBassMode (processor);
        s.chordRate = intParam (processor, cb::ParamIDs::rate, 0, cb::numRateDivisions - 1, 1);
        s.style = (int) owner.getGenerateSettings().style;
        return s;
    }

    void refresh()
    {
        const auto s = readState();

        if (s != state)
        {
            const bool relayout = s.custom != state.custom;
            state = s;

            if (relayout)
                layout();

            owner.repaint();
        }
    }

    //==============================================================================
    // Layout (final-spec 7.2): the first level that fits wins.
    struct Widths
    {
        int tabs = 0, main = 0, reset = 0, snap = 0, grid = 0, snapCap = 0, gridCap = 0, vel = 0, more = 30, gap = 12;

        int total() const
        {
            const int t = tabs + gap + main + (reset > 0 ? 6 + reset : 0) + gap + snapCap + snap + gap + gridCap + grid
                        + gap + more;
            return t + (vel > 0 ? vel + 6 : 0);
        }
    };

    // Widths of level `lv` for the tab `l`; `compact` forces the compact tabs below level 3 (see layout()).
    Widths widthsFor (int lv, cb::Layer l, bool compact) const
    {
        Widths w;
        const bool small = compact || lv >= 3;
        const auto& tabs = small ? tabCompact : tabFull;
        w.tabs = (int) std::ceil (tabs[0] + tabs[1] + tabs[2]) + (small ? 2 : 4);

        if (l == cb::Layer::Melody)
            w.main = lv <= 3 ? generateBodyW + stylePartW : (lv == 4 ? generateBodyW + 26 : 64);
        else
            w.main = lv <= 2 ? 168 : 140;

        w.reset = showsReset (l) ? (lv == 0 ? resetFullW : 30) : 0;
        w.snapCap = lv <= 1 ? 36 : 0;
        w.gridCap = lv <= 1 ? 34 : 0;
        w.snap = lv <= 3 ? 144 : 120;
        w.grid = lv <= 3 ? 152 : 128;
        w.vel = lv <= 4 ? 30 : 0;
        w.gap = lv <= 1 ? 12 : 10;
        return w;
    }

    void layout()
    {
        for (auto& b : bounds)
            b = {};

        snapCaption = gridCaption = {};
        const int width = owner.getWidth(), h = owner.getHeight();

        if (width <= 0 || h <= 0)
            return;

        // The first level that fits wins.
        const auto levelFor = [this, width] (cb::Layer l, bool compact)
        {
            for (int lv = 0; lv < 5; ++lv)
                if (widthsFor (lv, l, compact).total() <= width)
                    return lv;

            return 5;
        };

        // The tab strip is laid out from the width alone: compact as soon as any of the three tabs (in its current
        // custom state) needs level 3 or more here, so switching tabs never moves the tabs under the mouse. Only the
        // main control, Reset and the right group vary per tab. (Measured: the compact Arp / Bass tabs stay at their full
        // 54 / 60 px, as the pencil needs 20 + text + 11 + 6, so a custom Arp / Bass at level 3 needs about 759 px.)
        bool compact = false;

        for (int i = 0; i < 3; ++i)
            compact = compact || levelFor ((cb::Layer) i, false) >= 3;

        // With the tabs compact a tab never takes level 0 or 1 (captions, 12 px gaps): keeps the level monotonic in the
        // width when the compact flag drops off.
        level = compact ? std::max (2, levelFor (layer, true)) : levelFor (layer, false);
        const auto w = widthsFor (level, layer, compact);
        compactTabs = compact;
        browserCompact = level >= 3;
        resetIconOnly = level >= 1;
        styleText = level <= 3;
        generateIconOnly = level >= 5;

        // Left group: tabs, then the layer's main control (+ Reset)
        int x = 0;
        at (Item::Tabs) = { x, 0, w.tabs, h };
        {
            const auto& tw = compactTabs ? tabCompact : tabFull;
            const int pad = compactTabs ? 1 : 2;
            float tx = (float) (x + pad);
            const Item tabItems[] = { Item::TabMelody, Item::TabArp, Item::TabBass };
            const Item ledItems[] = { Item::LedMelody, Item::LedArp, Item::LedBass };

            for (int i = 0; i < 3; ++i)
            {
                const int x0 = juce::roundToInt (tx);
                const int x1 = i == 2 ? x + w.tabs - pad : juce::roundToInt (tx + tw[(size_t) i]);
                at (tabItems[i]) = { x0, 0, x1 - x0, h };
                at (ledItems[i]) = { x0, 0, 20, h };
                tx += tw[(size_t) i];
            }
        }
        x += w.tabs + w.gap;

        if (layer == cb::Layer::Melody)
        {
            const int menuW = styleText ? stylePartW : 26;
            at (Item::Generate) = { x, 0, w.main - menuW, h };
            at (Item::GenerateMenu) = { x + w.main - menuW, 0, menuW, h };
        }
        else
        {
            at (Item::BrowserPrev) = { x, 0, 22, h };
            at (Item::BrowserName) = { x + 22, 0, w.main - 44, h };
            at (Item::BrowserNext) = { x + w.main - 22, 0, 22, h };
        }
        x += w.main;

        if (w.reset > 0)
            at (Item::Reset) = { x + 6, 0, w.reset, h };

        // Right group, right to left: "...", velocity toggle, Grid, Snap (spare width opens up between the groups)
        int r = width;
        at (Item::More) = { r - w.more, 0, w.more, h };
        r -= w.more;

        if (w.vel > 0)
        {
            r -= 6;
            at (Item::VelocityLane) = { r - w.vel, 0, w.vel, h };
            r -= w.vel;
        }

        r -= w.gap;
        at (Item::Grid) = { r - w.grid, 0, w.grid, h };
        r -= w.grid;

        if (w.gridCap > 0)
        {
            gridCaption = { r - w.gridCap, 0, w.gridCap - 6, h };
            r -= w.gridCap;
        }

        r -= w.gap;
        at (Item::Snap) = { r - w.snap, 0, w.snap, h };
        r -= w.snap;

        if (w.snapCap > 0)
            snapCaption = { r - w.snapCap, 0, w.snapCap - 6, h };
    }

    //==============================================================================
    // Hit testing
    int itemAt (juce::Point<float> p) const
    {
        // LEDs before their tabs.
        const Item order[] = { Item::LedMelody, Item::LedArp, Item::LedBass, Item::TabMelody, Item::TabArp, Item::TabBass,
                               Item::Generate, Item::GenerateMenu, Item::BrowserPrev, Item::BrowserName, Item::BrowserNext,
                               Item::Reset, Item::Snap, Item::Grid, Item::VelocityLane, Item::More };

        for (auto item : order)
            if (at (item).toFloat().contains (p))
                return index (item);

        return -1;
    }

    static int segmentAt (juce::Rectangle<int> b, int n, float x)
    {
        const float segW = ((float) b.getWidth() - 4.0f) / (float) n;
        return juce::jlimit (0, n - 1, (int) ((x - (float) b.getX() - 2.0f) / segW));
    }

    static cb::Layer layerOfTab (int item)
    {
        if (item == index (Item::TabArp) || item == index (Item::LedArp))
            return cb::Layer::Arp;

        if (item == index (Item::TabBass) || item == index (Item::LedBass))
            return cb::Layer::Bass;

        return cb::Layer::Melody;
    }

    static bool isTabOrLed (int item) { return item >= index (Item::TabMelody) && item <= index (Item::LedBass); }
    static bool isLed (int item)      { return item >= index (Item::LedMelody) && item <= index (Item::LedBass); }
    static bool isTab (int item)      { return item >= index (Item::TabMelody) && item <= index (Item::TabBass); }

    bool isActionable (int item) const
    {
        if (item == index (Item::Generate))
            return state.hasChords;

        return item >= 0;
    }

    //==============================================================================
    // Mouse (final-spec 7: a right-click only opens a tab's menu and never acts as a left click)
    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto pos = e.getEventRelativeTo (&owner).position;
        const int item = itemAt (pos);
        pressedItem = -1;
        hideTip();
        tipSuppressed = true;

        if (e.mods.isPopupMenu())
        {
            if (isTabOrLed (item) && owner.onTabMenu != nullptr)
                owner.onTabMenu (layerOfTab (item));

            return;
        }

        if (isLed (item))
        {
            const auto l = layerOfTab (item);
            processor.setLayerOn (l, ! processor.isLayerOn (l));
            refresh();
        }
        else if (isTab (item))
        {
            if (owner.onLayerChosen != nullptr)
                owner.onLayerChosen (layerOfTab (item));
        }
        else if (item == index (Item::Snap))
        {
            const auto s = (cb::MelodySnap) segmentAt (at (Item::Snap), cb::numMelodySnapModes, pos.x);
            snap = s;
            owner.repaint();

            if (owner.onSnapChosen != nullptr)
                owner.onSnapChosen (s);
        }
        else if (item == index (Item::Grid))
        {
            const auto gr = (cb::RateDivision) segmentAt (at (Item::Grid), cb::numRateDivisions, pos.x);
            grid = gr;
            owner.repaint();

            if (owner.onGridChosen != nullptr)
                owner.onGridChosen (gr);
        }
        else if (item == index (Item::GenerateMenu))
        {
            showGeneratorMenu();
        }
        else if (item == index (Item::BrowserName))
        {
            showPatternMenu();
        }
        else if (item == index (Item::More))
        {
            if (owner.onMoreClicked != nullptr)
                owner.onMoreClicked (owner.localAreaToGlobal (at (Item::More)));
        }
        else if (item >= 0)
        {
            pressedItem = item;   // Generate, arrows, Reset, velocity toggle: act on mouse-up inside the same item
            owner.repaint();
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const int pressed = pressedItem;
        pressedItem = -1;

        if (pressed < 0 || e.mods.isPopupMenu())
            return;

        owner.repaint();

        if (itemAt (e.getEventRelativeTo (&owner).position) != pressed || ! isActionable (pressed))
            return;

        if (pressed == index (Item::Generate))
        {
            owner.generateNewMelody();
        }
        else if (pressed == index (Item::BrowserPrev) || pressed == index (Item::BrowserNext))
        {
            owner.stepPattern (pressed == index (Item::BrowserNext) ? 1 : -1);
        }
        else if (pressed == index (Item::Reset))
        {
            processor.resetLayer (layer);
            refresh();
        }
        else if (pressed == index (Item::VelocityLane))
        {
            velocityLaneOn = ! velocityLaneOn;
            owner.repaint();

            if (owner.onVelocityLaneToggled != nullptr)
                owner.onVelocityLaneToggled (velocityLaneOn);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override  { updateHover (e.getEventRelativeTo (&owner).position); }
    void mouseEnter (const juce::MouseEvent& e) override { updateHover (e.getEventRelativeTo (&owner).position); }
    void mouseExit (const juce::MouseEvent&) override    { updateHover ({ -1000.0f, -1000.0f }); }

    void updateHover (juce::Point<float> p)
    {
        const int item = itemAt (p);
        int segment = -1;

        if (item == index (Item::Snap))
            segment = segmentAt (at (Item::Snap), cb::numMelodySnapModes, p.x);
        else if (item == index (Item::Grid))
            segment = segmentAt (at (Item::Grid), cb::numRateDivisions, p.x);

        if (item != hoverItem)
        {
            hideTip();
            tipSuppressed = false;
            hoverSince = juce::Time::getMillisecondCounter();
        }

        tipMouse = p;

        if (item != hoverItem || segment != hoverSegment)
        {
            hoverItem = item;
            hoverSegment = segment;
            owner.setMouseCursor (isActionable (item) ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
            owner.repaint();
        }

        if (item >= 0 && ! isTimerRunning())
            startTimer (100);
    }

    //==============================================================================
    // Hover tips (the editor's TooltipWindow look and delay)
    juce::String tooltipAt (juce::Point<float> p) override
    {
        const int item = itemAt (p);
        return item >= 0 ? tooltipFor (item) : juce::String();
    }

    // The browser's name and its rate suffix (full width only; the arp and the Octaves / Pulse bass).
    std::pair<juce::String, juce::String> browserLabel() const
    {
        const auto label = patternName (processor, layer);
        const bool suffix = ! browserCompact && patternFollowsRate (layer, currentBassMode (processor));
        return { label, suffix ? " " + middleDot() + " " + rateSuffix (processor, layer) : juce::String() };
    }

    juce::String browserText() override
    {
        if (layer == cb::Layer::Melody)
            return {};

        const auto text = browserLabel();
        return text.first + text.second;
    }

    void timerCallback() override
    {
        if (hoverItem < 0 || ! owner.isShowing())
        {
            hideTip();
            stopTimer();
            return;
        }

        if (tip != nullptr || tipSuppressed || juce::ModifierKeys::getCurrentModifiersRealtime().isAnyMouseButtonDown())
            return;

        if (juce::Time::getMillisecondCounter() - hoverSince >= 700)
            showTip();
    }

    void showTip()
    {
        auto* top = cb::ui::rolltoolbar::tipParentFor (owner);
        const auto text = tooltipFor (hoverItem);

        if (top == nullptr || top == &owner || text.isEmpty())
            return;

        tip = std::make_unique<HoverTip>();
        tip->text = text;
        tip->setLookAndFeel (&owner.getLookAndFeel());
        const auto screenPos = owner.localPointToGlobal (tipMouse.toInt());
        const auto screenArea = top->getScreenBounds();
        const auto tipArea = owner.getLookAndFeel().getTooltipBounds (text, screenPos, screenArea);
        top->addAndMakeVisible (*tip);
        tip->setBounds (top->getLocalArea (nullptr, tipArea));
        tip->toFront (false);
    }

    void hideTip()
    {
        if (tip != nullptr)
        {
            if (auto* parent = tip->getParentComponent())
                parent->removeChildComponent (tip.get());

            tip->setLookAndFeel (nullptr);
            tip.reset();
        }
    }

    //==============================================================================
    // Actions
    void afterPick (cb::Layer l, bool wasCustom)
    {
        refresh();

        if (wasCustom && ! processor.isLayerCustom (l) && ! hintShown[(size_t) l])
        {
            hintShown[(size_t) l] = true;   // each hint kind once per editor session

            if (owner.onHint != nullptr)
                owner.onHint (juce::String (cb::layerName (l)) + " follows the pattern again " + middleDot()
                              + " Undo brings your edits back");
        }
    }

    void stepPattern (int direction)
    {
        if (layer == cb::Layer::Melody || direction == 0)
            return;

        const auto l = layer;
        const bool wasCustom = processor.isLayerCustom (l);
        const int dir = direction > 0 ? 1 : -1;

        if (l == cb::Layer::Arp)
        {
            const int next = ((int) currentShape (processor) + dir + cb::numArpShapes) % cb::numArpShapes;
            setParamAsUser (processor, cb::ParamIDs::arpShape, (float) next);
        }
        else
        {
            const auto order = cb::bassModeBrowseOrder();
            const auto mode = currentBassMode (processor);
            auto next = dir > 0 ? order.front() : order.back();   // from Off: > Root, < Dotted quarters

            if (mode != cb::BassMode::Off && ! order.empty())
            {
                const auto it = std::find (order.begin(), order.end(), mode);
                const int i = it == order.end() ? 0 : (int) (it - order.begin());
                next = order[(size_t) ((i + dir + (int) order.size()) % (int) order.size())];
            }

            setParamAsUser (processor, cb::ParamIDs::bassMode, (float) next);
        }

        processor.beginUserLayerPick (l);   // after the parameter: the reset's single publish renders the new pattern
        afterPick (l, wasCustom);
    }

    void showPatternMenu()
    {
        if (layer == cb::Layer::Melody)
            return;

        auto menu = cb::ui::rolltoolbar::createPatternMenu (processor, layer);
        menu.setLookAndFeel (&owner.getLookAndFeel());
        juce::Component::SafePointer<RollToolbar> safe (&owner);
        const auto l = layer;

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (owner.localAreaToGlobal (at (Item::BrowserName))),
                            [safe, l] (int result)
                            {
                                if (safe == nullptr || result == 0)
                                    return;

                                auto& d = *safe->impl;
                                const bool wasCustom = d.processor.isLayerCustom (l);

                                if (cb::ui::rolltoolbar::applyPatternMenuResult (d.processor, l, result))
                                    d.afterPick (l, wasCustom);
                            });
    }

    void showGeneratorMenu()
    {
        auto menu = cb::ui::rolltoolbar::createGeneratorMenu (processor, owner.getGenerateSettings());
        menu.setLookAndFeel (&owner.getLookAndFeel());
        juce::Component::SafePointer<RollToolbar> safe (&owner);

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (owner.localAreaToGlobal (at (Item::GenerateMenu))),
                            [safe] (int result)
                            {
                                if (safe != nullptr && result != 0)
                                {
                                    cb::ui::rolltoolbar::applyGeneratorMenuResult (*safe, safe->impl->processor, result);
                                    safe->impl->refresh();
                                }
                            });
    }

    //==============================================================================
    // Tooltips (final-spec 7.3 readouts)
    juce::String tabTooltip (cb::Layer l) const
    {
        const int ch = cb::layerMidiChannel (l);
        const bool on = processor.isLayerOn (l);
        const int notes = (int) processor.getLayerNotes (l).notes.size();
        const juce::String noteText = juce::String (notes) + (notes == 1 ? " note" : " notes");

        if (l == cb::Layer::Melody)
            return "Melody: your lead line, " + noteText + " (MIDI ch " + juce::String (ch) + ")" + (on ? "" : ", muted")
                 + ". The dot " + (on ? "mutes" : "plays") + " it.";

        juce::String text = juce::String (cb::layerName (l)) + ": ";

        if (processor.isLayerCustom (l))
        {
            const auto edited = processor.getEditedSlots (l);
            const int n = (int) std::count (edited.begin(), edited.end(), true);
            text << "edited " << n << "/" << (int) edited.size() << " chords, " << noteText << " (MIDI ch " << ch << ")"
                 << (on ? "" : ", off") << ". Reset follows the chords again; the dot turns it " << (on ? "off." : "on.");
            return text;
        }

        text << "auto, " << patternName (processor, l);

        if (patternFollowsRate (l, currentBassMode (processor)))
            text << " at " << rateSuffix (processor, l);

        text << ", " << (on ? "on" : "off") << " (MIDI ch " << ch << "). The dot turns it " << (on ? "off." : "on.");
        return text;
    }

    juce::String tooltipFor (int item) const
    {
        const auto styleName = juce::String (cb::melodyStyleName (owner.getGenerateSettings().style));
        const auto word = layerWord (layer);

        if (isTabOrLed (item))
            return tabTooltip (layerOfTab (item));

        switch ((Item) item)
        {
            case Item::Generate:
                return state.hasChords ? "Generate a new melody over the chords (" + styleName
                                             + "). Every click gives a new one; Undo brings the last one back."
                                       : juce::String ("Add chords first");
            case Item::GenerateMenu:
                return "Melody generator: style (" + styleName + "), density, range, contour and repetition, "
                       "or the same idea again";
            case Item::BrowserPrev: return "Previous " + word + " pattern";
            case Item::BrowserNext: return "Next " + word + " pattern";
            case Item::BrowserName:
            {
                juce::String text = juce::String (cb::layerName (layer)) + " pattern: " + patternName (processor, layer);

                if (layer == cb::Layer::Arp)
                {
                    const auto rhythm = (cb::ArpRhythm) intParam (processor, cb::ParamIDs::arpRhythm, 0, cb::numArpRhythms - 1, 0);
                    text << " at " << rateSuffix (processor, layer) << ", " << cb::arpRhythmName (rhythm) << ", gate "
                         << juce::roundToInt (rawParam (processor, cb::ParamIDs::arpGate, 0.9f) * 100.0f)
                         << "%.\nClick for every shape, the rate, rhythm, gate and octave.";
                }
                else
                {
                    const auto mode = currentBassMode (processor);

                    if (mode == cb::BassMode::OctaveBounce8 || mode == cb::BassMode::Driving8)
                        text << " (fixed 8ths: ignores Rate)";
                    else if (cb::isBassPattern (mode))
                        text << " (a fixed groove: ignores Rate)";
                    else if (patternFollowsRate (layer, mode))
                        text << " (follows Rate: " << rateSuffix (processor, layer) << ")";

                    text << ".\nClick for every bass pattern.";
                }

                if (state.custom[(size_t) layer])
                    text << " Picking one resets your edits (Undo brings them back).";

                return text;
            }
            case Item::Reset:
                return "Reset: the " + word + " follows the chords and its pattern again (Undo brings your edits back)";
            case Item::Snap:  return "Snap: where drawn notes land (anywhere, on the key's scale, or on the chord's tones)";
            case Item::Grid:  return "Grid: the time grid and note length for drawing (hold Alt for no grid)";
            case Item::VelocityLane:
                return velocityLaneOn ? juce::String ("Hide the velocity lane")
                                      : juce::String ("Show the velocity lane: drag a stalk to set a note's velocity");
            case Item::More:  return "More: selection, copy and paste, transpose, velocity and view options";
            case Item::Tabs: case Item::TabMelody: case Item::TabArp: case Item::TabBass: case Item::LedMelody:
            case Item::LedArp: case Item::LedBass: case Item::NumItems:
            default: break;
        }

        return {};
    }

    //==============================================================================
    // Painting
    void paint (juce::Graphics& g) const
    {
        if (owner.getWidth() <= 0)
            return;

        paintTabs (g);

        if (layer == cb::Layer::Melody)
            paintGenerate (g);
        else
            paintBrowser (g);

        if (! at (Item::Reset).isEmpty())
            paintReset (g);

        paintSegments (g, Item::Snap, { "Off", "Scale", "Chord" }, (int) snap, snapCaption, "Snap");
        paintSegments (g, Item::Grid, { "1/4", "1/8", "1/16", "1/8T" }, (int) grid, gridCaption, "Grid");

        if (! at (Item::VelocityLane).isEmpty())
            paintVelocityToggle (g);

        paintMore (g);
    }

    void paintTabs (juce::Graphics& g) const
    {
        const auto all = at (Item::Tabs).toFloat();
        const float radius = std::min (7.0f, all.getHeight() * 0.5f);
        paintInset (g, all, radius);

        const Item tabItems[] = { Item::TabMelody, Item::TabArp, Item::TabBass };
        const Item ledItems[] = { Item::LedMelody, Item::LedArp, Item::LedBass };
        const float pad = compactTabs ? 1.0f : 2.0f;

        for (int i = 0; i < 3; ++i)
        {
            const auto l = (cb::Layer) i;
            const auto seg = at (tabItems[i]).toFloat().reduced (0.0f, pad);
            const bool selected = l == layer;
            const bool hoverTab = hoverItem == index (tabItems[i]);
            const bool hoverLed = hoverItem == index (ledItems[i]);

            if (selected)
            {
                paintSelectedSegment (g, seg, radius - 1.5f);
            }
            else if (hoverTab || hoverLed)
            {
                g.setColour (Col::panelRaised.withAlpha (0.8f));
                g.fillRoundedRectangle (seg, radius - 1.5f);
            }
            else if (i > 0 && (int) layer != i - 1)
            {
                g.setColour (Col::outline.withAlpha (0.55f));
                g.fillRect (juce::Rectangle<float> (seg.getX() - 0.5f, seg.getY() + seg.getHeight() * 0.28f,
                                                    1.0f, seg.getHeight() * 0.44f));
            }

            // LED: lit = audible (isLayerOn), filled with a soft glow; unlit = a faint ring.
            const auto ledCentre = juce::Point<float> (std::round (seg.getX() + 11.0f), seg.getCentreY());
            const auto colour = cb::ui::layerColour (l);

            if (state.on[(size_t) i])
            {
                g.setColour (colour.withAlpha (hoverLed ? 0.26f : 0.16f));
                g.fillEllipse (juce::Rectangle<float> (13.0f, 13.0f).withCentre (ledCentre));
                g.setColour (colour.withAlpha (0.35f));
                g.fillEllipse (juce::Rectangle<float> (9.5f, 9.5f).withCentre (ledCentre));
                g.setColour (colour.brighter (hoverLed ? 0.25f : 0.0f));
                g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (ledCentre));
            }
            else
            {
                g.setColour (hoverLed ? colour.withAlpha (0.85f) : Col::textFaint.brighter (0.1f));
                g.drawEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre (ledCentre), 1.2f);
            }

            // Name + edited mark ("*" when the pencil does not fit)
            const auto name = juce::String (cb::layerName (l));
            const auto f = tabFont();
            const float textX = seg.getX() + 20.0f;
            const float nameW = widthOf (f, name);
            const bool edited = l != cb::Layer::Melody && state.custom[(size_t) i];
            const float room = seg.getRight() - textX - 3.0f;
            const bool pencil = edited && nameW + 3.0f + 8.0f <= room;

            g.setFont (f);
            g.setColour (selected ? Col::text : (hoverTab ? Col::text.withAlpha (0.9f) : Col::textDim));
            g.drawText (name + (edited && ! pencil ? "*" : ""), juce::Rectangle<float> (textX, seg.getY(), room + 2.0f, seg.getHeight()),
                        juce::Justification::centredLeft, false);

            if (pencil)
                drawPencil (g, juce::Rectangle<float> (textX + nameW + 3.5f, seg.getCentreY() - 3.5f, 7.0f, 7.0f),
                            selected ? Col::textDim.brighter (0.25f) : Col::textDim);
        }
    }

    void paintGenerate (juce::Graphics& g) const
    {
        const auto body = at (Item::Generate).toFloat();
        const auto menuZone = at (Item::GenerateMenu).toFloat();
        const auto r = body.getUnion (menuZone).reduced (0.5f);
        const float radius = r.getHeight() * 0.5f;
        const bool enabled = state.hasChords;
        const float alpha = enabled ? 1.0f : 0.42f;
        const bool hotBody = enabled && hoverItem == index (Item::Generate);
        const bool hotMenu = hoverItem == index (Item::GenerateMenu);
        const bool down = pressedItem == index (Item::Generate);

        // Spark styling: raised surface with a mint tint and outline, the sparkle in mint. Without chords only the body
        // is disabled (the style menu still stores settings), so the tint and outline are dimmed in the body zone only
        // and the two halves read as two states.
        g.setGradientFill (juce::ColourGradient::vertical (Col::panelRaised, r.getY(), Col::panelRaised.darker (0.08f), r.getBottom()));
        g.fillRoundedRectangle (r, radius);

        const auto edge = Col::accent.withAlpha (hotBody || hotMenu ? 0.75f : 0.45f);
        const float tint = down ? 0.24f : (hotBody || hotMenu ? 0.17f : 0.10f);

        for (const bool bodyZone : { true, false })
        {
            juce::Graphics::ScopedSaveState save (g);
            const auto zone = bodyZone ? body : menuZone;
            const bool hot = bodyZone ? hotBody : hotMenu;
            const float zoneAlpha = bodyZone ? alpha : 1.0f;
            g.reduceClipRegion (zone.withY (r.getY() - 1.0f).withBottom (r.getBottom() + 1.0f).getSmallestIntegerContainer());
            g.setColour (Col::accent.withAlpha (tint * zoneAlpha));
            g.fillRoundedRectangle (r, radius);
            g.setColour (edge.withMultipliedAlpha (bodyZone && ! enabled ? 0.6f : 1.0f));
            g.drawRoundedRectangle (r.reduced (0.5f), radius - 0.5f, 1.0f);

            if (hot)
            {
                juce::Path clip;
                clip.addRoundedRectangle (r, radius);
                g.reduceClipRegion (clip);
                g.setColour (juce::Colours::white.withAlpha (0.05f));
                g.fillRect (zone);
            }
        }

        g.setColour (edge.withMultipliedAlpha (0.6f));
        g.fillRect (juce::Rectangle<float> (menuZone.getX(), r.getY() + 5.0f, 1.0f, r.getHeight() - 10.0f));

        // Body: sparkle + "Generate" (sparkle only at level 5)
        const auto f = buttonFont();
        const float labelW = widthOf (f, "Generate");
        const float contentW = generateIconOnly ? 13.0f : 13.0f + 6.0f + labelW;
        const float x = std::round (body.getCentreX() - contentW * 0.5f);
        g.setColour (Col::accent.withMultipliedAlpha (alpha));
        g.fillPath (sparklePath ({ x, r.getCentreY() - 6.5f, 13.0f, 13.0f }));

        if (! generateIconOnly)
        {
            g.setFont (f);
            g.setColour (Col::text.withMultipliedAlpha (alpha));
            g.drawText ("Generate", juce::Rectangle<float> (x + 19.0f, r.getY(), labelW + 2.0f, r.getHeight()),
                        juce::Justification::centredLeft, false);
        }

        // Menu part: the style's short name + chevron (chevron only from level 4)
        const auto ink = hotMenu ? Col::text : Col::textDim.brighter (0.15f);

        if (styleText)
        {
            const auto style = juce::String (cb::melodyStyleShortName (owner.getGenerateSettings().style));
            const auto zone = menuZone.withTrimmedLeft (9.0f).withTrimmedRight (8.0f);
            g.setFont (detailFont());
            g.setColour (ink);
            g.drawFittedText (style, zone.withTrimmedRight (13.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.85f);
            drawChevronDown (g, { zone.getRight() - 4.0f, r.getCentreY() }, ink);
        }
        else
        {
            drawChevronDown (g, { menuZone.getCentreX() - 1.0f, r.getCentreY() }, ink);
        }
    }

    void paintBrowser (juce::Graphics& g) const
    {
        const auto prev = at (Item::BrowserPrev).toFloat();
        const auto name = at (Item::BrowserName).toFloat();
        const auto next = at (Item::BrowserNext).toFloat();
        const auto r = prev.getUnion (next).reduced (0.5f);
        const float radius = std::min (7.0f, r.getHeight() * 0.5f);
        const bool hot = hoverItem == index (Item::BrowserPrev) || hoverItem == index (Item::BrowserName)
                      || hoverItem == index (Item::BrowserNext);
        paintRaised (g, r, radius, false);

        auto zoneHot = [&] (Item item, juce::Rectangle<float> zone)
        {
            const bool isHot = hoverItem == index (item);
            const bool down = pressedItem == index (item);

            if (isHot || down)
            {
                juce::Graphics::ScopedSaveState save (g);
                juce::Path clip;
                clip.addRoundedRectangle (r, radius);
                g.reduceClipRegion (clip);
                g.setColour (juce::Colours::white.withAlpha (down ? 0.10f : 0.06f));
                g.fillRect (zone);
            }

            return isHot;
        };

        const bool hotPrev = zoneHot (Item::BrowserPrev, prev);
        const bool hotName = zoneHot (Item::BrowserName, name);
        const bool hotNext = zoneHot (Item::BrowserNext, next);

        if (hot)
        {
            g.setColour (Col::outline.brighter (0.25f));
            g.drawRoundedRectangle (r.reduced (0.5f), radius - 0.5f, 1.0f);
        }

        g.setColour (Col::outline.withAlpha (0.9f));
        g.fillRect (juce::Rectangle<float> (name.getX(), r.getY() + 5.0f, 1.0f, r.getHeight() - 10.0f));
        g.fillRect (juce::Rectangle<float> (name.getRight() - 1.0f, r.getY() + 5.0f, 1.0f, r.getHeight() - 10.0f));

        drawChevronSide (g, prev.getCentre().translated (0.5f, 0.0f), false, hotPrev ? Col::text : Col::textDim);
        drawChevronSide (g, next.getCentre().translated (-0.5f, 0.0f), true, hotNext ? Col::text : Col::textDim);

        // Name (fitted, min scale 0.8) + the rate suffix at full width for the arp and the Octaves / Pulse bass + chevron
        const auto mode = currentBassMode (processor);
        const auto [label, suffixText] = browserLabel();
        const bool suffix = suffixText.isNotEmpty();
        const auto zone = name.withTrimmedLeft (8.0f).withTrimmedRight (7.0f);
        const float avail = zone.getWidth() - 13.0f;

        auto f = buttonFont();
        const auto df = detailFont();
        float labelW = widthOf (f, label);
        const float suffixW = suffix ? widthOf (df, suffixText) : 0.0f;

        if (labelW + suffixW > avail)
        {
            const float scale = juce::jlimit (0.8f, 1.0f, (avail - suffixW) / std::max (1.0f, labelW));
            f = f.withHeight (f.getHeight() * scale);
            labelW = widthOf (f, label);
        }

        const bool off = layer == cb::Layer::Bass && mode == cb::BassMode::Off;
        const auto ink = off ? Col::textDim : (hotName ? Col::text : Col::text.withAlpha (0.92f));
        g.setFont (f);
        g.setColour (ink);

        if (labelW + suffixW <= avail + 0.5f)
        {
            const float x = std::round (zone.getX() + (avail - labelW - suffixW) * 0.5f);
            g.drawText (label, juce::Rectangle<float> (x, r.getY(), labelW + 2.0f, r.getHeight()), juce::Justification::centredLeft, false);

            if (suffix)
            {
                g.setFont (df);
                g.setColour (Col::textFaint.brighter (0.35f));
                g.drawText (suffixText, juce::Rectangle<float> (x + labelW, r.getY(), suffixW + 2.0f, r.getHeight()),
                            juce::Justification::centredLeft, false);
            }
        }
        else
        {
            g.drawFittedText (label, juce::Rectangle<float> (zone.getX(), r.getY(), avail, r.getHeight()).toNearestInt(),
                              juce::Justification::centred, 1, 1.0f);
        }

        drawChevronDown (g, { zone.getRight() - 4.0f, r.getCentreY() }, hotName ? Col::text : Col::textDim);
    }

    void paintReset (juce::Graphics& g) const
    {
        const auto r = at (Item::Reset).toFloat().reduced (0.5f);
        const float radius = r.getHeight() * 0.5f;
        const bool hot = hoverItem == index (Item::Reset);
        const bool down = pressedItem == index (Item::Reset);
        paintRaised (g, r, radius, hot || down);
        g.setColour (Col::accent.withAlpha (down ? 0.18f : (hot ? 0.12f : 0.06f)));
        g.fillRoundedRectangle (r, radius);
        g.setColour (Col::accent.withAlpha (hot ? 0.7f : 0.38f));
        g.drawRoundedRectangle (r.reduced (0.5f), radius - 0.5f, 1.0f);

        const auto f = buttonFont();
        const float labelW = widthOf (f, "Reset");
        const float contentW = resetIconOnly ? 12.0f : 12.0f + 6.0f + labelW;
        const float x = std::round (r.getCentreX() - contentW * 0.5f);
        drawResetIcon (g, { x, r.getCentreY() - 6.0f, 12.0f, 12.0f }, Col::accent);

        if (! resetIconOnly)
        {
            g.setFont (f);
            g.setColour (hot ? Col::text : Col::text.withAlpha (0.9f));
            g.drawText ("Reset", juce::Rectangle<float> (x + 18.0f, r.getY(), labelW + 2.0f, r.getHeight()),
                        juce::Justification::centredLeft, false);
        }
    }

    void paintSegments (juce::Graphics& g, Item item, std::initializer_list<const char*> labels, int selected,
                        juce::Rectangle<int> caption, const char* captionText) const
    {
        if (! caption.isEmpty())
        {
            g.setFont (labelFont());
            g.setColour (Col::textDim);
            g.drawText (captionText, caption, juce::Justification::centredRight, false);
        }

        const auto b = at (item).toFloat();
        const float radius = std::min (6.0f, b.getHeight() * 0.5f);
        paintInset (g, b, radius);

        const int n = (int) labels.size();
        const float segW = (b.getWidth() - 4.0f) / (float) n;
        g.setFont (segFont());
        int i = 0;

        for (const char* label : labels)
        {
            const auto seg = juce::Rectangle<float> (b.getX() + 2.0f + segW * (float) i, b.getY() + 2.0f, segW, b.getHeight() - 4.0f);
            const bool isSelected = i == selected;
            const bool isHover = hoverItem == index (item) && hoverSegment == i;

            if (isSelected)
            {
                paintSelectedSegment (g, seg, radius - 1.5f);
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
            g.drawFittedText (label, seg.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.75f);
            ++i;
        }
    }

    void paintVelocityToggle (juce::Graphics& g) const
    {
        const auto r = at (Item::VelocityLane).toFloat().reduced (0.5f);
        const float radius = std::min (7.0f, r.getHeight() * 0.5f);
        const bool hot = hoverItem == index (Item::VelocityLane);
        paintRaised (g, r, radius, hot);

        if (velocityLaneOn)
        {
            g.setColour (Col::accent.withAlpha (0.16f));
            g.fillRoundedRectangle (r, radius);
            g.setColour (Col::accent.withAlpha (0.55f));
            g.drawRoundedRectangle (r.reduced (0.5f), radius - 0.5f, 1.0f);
        }

        drawLollipops (g, r, velocityLaneOn ? Col::accent : (hot ? Col::text : Col::textDim));
    }

    void paintMore (juce::Graphics& g) const
    {
        const auto r = at (Item::More).toFloat().reduced (0.5f);
        const float radius = std::min (7.0f, r.getHeight() * 0.5f);
        const bool hot = hoverItem == index (Item::More);
        paintRaised (g, r, radius, hot);
        drawDots (g, r.getCentre(), hot ? Col::text : Col::textDim);
    }
};

//==============================================================================
RollToolbar::RollToolbar (ChordbloomProcessor& p) : impl (std::make_unique<Impl> (*this, p))
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    setOpaque (false);
    setTitle ("Roll toolbar");
    setDescription ("Layer tabs, Generate or the pattern browser, Snap, Grid and the roll menu");
    addMouseListener (impl.get(), false);
    impl->state = impl->readState();
}

RollToolbar::~RollToolbar()
{
    removeMouseListener (impl.get());
}

void RollToolbar::paint (juce::Graphics& g) { impl->paint (g); }
void RollToolbar::resized() { impl->layout(); }

void RollToolbar::refresh() { impl->refresh(); }

void RollToolbar::setLayer (cb::Layer layer)
{
    if (layer == cb::Layer::NumLayers || layer == impl->layer)
        return;

    impl->layer = layer;
    impl->pressedItem = -1;
    impl->state = impl->readState();
    impl->layout();
    repaint();
}

void RollToolbar::setSnap (cb::MelodySnap snap)
{
    if (impl->snap != snap)
    {
        impl->snap = snap;
        repaint();
    }
}

void RollToolbar::setGrid (cb::RateDivision grid)
{
    if (impl->grid != grid)
    {
        impl->grid = grid;
        repaint();
    }
}

void RollToolbar::setVelocityLaneOn (bool on)
{
    if (impl->velocityLaneOn != on)
    {
        impl->velocityLaneOn = on;
        repaint();
    }
}

//==============================================================================
void RollToolbar::generateNewMelody()
{
    if (impl->processor.getProgression().empty())
        return;

    // juce::Random appears only here (final-spec 6.6): the core generator is seeded and deterministic.
    for (int attempt = 0; attempt < 8; ++attempt)
        if (generateMelody ((std::uint32_t) juce::Random::getSystemRandom().nextInt() | 1u))
            return;
}

bool RollToolbar::generateMelody (std::uint32_t seed)
{
    auto& p = impl->processor;

    if (seed == 0 || ! p.generateMelody (getGenerateSettings(), seed))
        return false;

    p.getAPVTS().state.setProperty (kGenSeed, (int) seed, nullptr);
    impl->refresh();

    if (onMelodyGenerated != nullptr)
        onMelodyGenerated();

    return true;
}

cb::MelodyGenSettings RollToolbar::getGenerateSettings() const
{
    const auto& state = impl->processor.getAPVTS().state;
    cb::MelodyGenSettings s;

    if (state.hasProperty (kGenStyle))
    {
        s.style = (cb::MelodyStyle) juce::jlimit (0, cb::numMelodyStyles - 1, (int) state.getProperty (kGenStyle));
    }
    else
    {
        const int mood = juce::jlimit (0, cb::numMoods - 1, (int) state.getProperty (kSparkMood, (int) cb::Mood::Emotional));
        s.style = cb::melodyStyleForMood ((cb::Mood) mood);
    }

    s.density = (float) juce::jlimit (0.0, 1.0, (double) state.getProperty (kGenDensity, 0.5));
    s.repetition = (float) juce::jlimit (0.0, 1.0, (double) state.getProperty (kGenRepetition, 0.5));
    s.registerShift = juce::jlimit (-12, 12, (int) state.getProperty (kGenRegister, 0));
    s.contour = (cb::MelodyContour) juce::jlimit (0, cb::numMelodyContours - 1, (int) state.getProperty (kGenContour, 0));
    s.resolveEnding = (bool) state.getProperty (kGenResolve, true);
    return s;
}

void RollToolbar::setGenerateSettings (const cb::MelodyGenSettings& s)
{
    auto& p = impl->processor;
    auto& state = p.getAPVTS().state;
    const auto old = getGenerateSettings();

    // Only what changed is written, so an untouched style keeps following the Spark mood.
    if (s.style != old.style || state.hasProperty (kGenStyle))
        state.setProperty (kGenStyle, (int) s.style, nullptr);
    if (s.density != old.density)
        state.setProperty (kGenDensity, (double) juce::jlimit (0.0f, 1.0f, s.density), nullptr);
    if (s.repetition != old.repetition)
        state.setProperty (kGenRepetition, (double) juce::jlimit (0.0f, 1.0f, s.repetition), nullptr);
    if (s.registerShift != old.registerShift)
        state.setProperty (kGenRegister, juce::jlimit (-12, 12, s.registerShift), nullptr);
    if (s.contour != old.contour)
        state.setProperty (kGenContour, (int) s.contour, nullptr);
    if (s.resolveEnding != old.resolveEnding)
        state.setProperty (kGenResolve, s.resolveEnding, nullptr);

    impl->refresh();
    repaint();

    // The user hears the change on the same idea, but only while the melody is the untouched last result: hand edits,
    // undo / redo and loads are never replaced except by an explicit Generate command (final-spec 7.4). The processor
    // decides after applying a load still pending from another thread (which forgets the generator result).
    if (s != old && p.regenerateLastIdea (getGenerateSettings()))
    {
        state.setProperty (kGenSeed, (int) p.getLastGeneratorSeed(), nullptr);

        if (onMelodyGenerated != nullptr)
            onMelodyGenerated();
    }
}

void RollToolbar::stepPattern (int direction) { impl->stepPattern (direction); }

int RollToolbar::getLayoutLevel() const noexcept { return impl->level; }

juce::Rectangle<int> RollToolbar::getItemBounds (RollToolbarItem item) const
{
    if (item == RollToolbarItem::NumItems)
        return {};

    return impl->at (item);
}
