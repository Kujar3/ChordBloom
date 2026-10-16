#include "ui/HeaderBar.h"

#include "ui/Theme.h"

#include "ChordbloomBinary.h"

namespace
{
namespace Col = cb::ui::Colours;
using APVTS = juce::AudioProcessorValueTreeState;

//==============================================================================
// 2x2 box-filter downscale (premultiplied ARGB), used to build a clean small logo from the 192 px PNG.
juce::Image halveImage (const juce::Image& source)
{
    const auto src = source.convertedToFormat (juce::Image::ARGB);
    const int w = src.getWidth() / 2, h = src.getHeight() / 2;

    if (w < 1 || h < 1)
        return src;

    juce::Image dst (juce::Image::ARGB, w, h, true);
    const juce::Image::BitmapData s (src, juce::Image::BitmapData::readOnly);
    juce::Image::BitmapData d (dst, juce::Image::BitmapData::writeOnly);

    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            juce::uint32 a = 0, r = 0, g = 0, b = 0;

            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx)
                {
                    const auto* p = reinterpret_cast<const juce::PixelARGB*> (s.getPixelPointer (x * 2 + dx, y * 2 + dy));
                    a += p->getAlpha();
                    r += p->getRed();
                    g += p->getGreen();
                    b += p->getBlue();
                }

            auto* q = reinterpret_cast<juce::PixelARGB*> (d.getPixelPointer (x, y));
            q->setARGB ((juce::uint8) ((a + 2) / 4), (juce::uint8) ((r + 2) / 4),
                        (juce::uint8) ((g + 2) / 4), (juce::uint8) ((b + 2) / 4));
        }
    }

    return dst;
}

//==============================================================================
// Small icon buttons (undo, redo, preview sound on/off, help). Right-clicks are ignored (never act as a click).
class IconButton final : public juce::Button
{
public:
    enum class Icon { undo, redo, speaker, help };

    IconButton (const juce::String& name, Icon iconType) : juce::Button (name), icon (iconType)
    {
        setTitle (name);
    }

    void mouseDown (const juce::MouseEvent& e) override { if (! e.mods.isPopupMenu()) juce::Button::mouseDown (e); }
    void mouseDrag (const juce::MouseEvent& e) override { if (! e.mods.isPopupMenu()) juce::Button::mouseDrag (e); }
    void mouseUp (const juce::MouseEvent& e) override   { if (! e.mods.isPopupMenu()) juce::Button::mouseUp (e); }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        const auto b = getLocalBounds().toFloat().reduced (0.5f);
        const bool enabled = isEnabled();
        highlighted = highlighted && enabled;
        down = down && enabled;

        if (highlighted || down || (icon == Icon::help && getToggleState()))
        {
            g.setColour (Col::panelHover.withAlpha (down ? 1.0f : 0.85f));
            g.fillRoundedRectangle (b, 6.0f);
            g.setColour (Col::outline.brighter (0.1f));
            g.drawRoundedRectangle (b, 6.0f, 1.0f);
        }

        const auto box = b.withSizeKeepingCentre (16.0f, 16.0f);
        auto colour = highlighted ? Col::text : Col::textDim;

        if (icon == Icon::speaker)
            colour = getToggleState() ? Col::accent.brighter (highlighted ? 0.15f : 0.0f)
                                      : (highlighted ? Col::textDim.brighter (0.2f) : Col::textFaint.brighter (0.15f));
        else if (icon == Icon::help && getToggleState())
            colour = Col::accent;

        if (! enabled)
            colour = Col::textFaint.withAlpha (0.55f);

        g.setColour (colour);

        if (icon == Icon::speaker)
            paintSpeaker (g, box);
        else if (icon == Icon::help)
            paintHelp (g, box);
        else
            paintArrow (g, box, icon == Icon::redo);
    }

private:
    static float mx (const juce::Rectangle<float>& box, float x, bool mirror)
    {
        return box.getX() + (mirror ? 16.0f - x : x);
    }

    void paintArrow (juce::Graphics& g, juce::Rectangle<float> box, bool mirror) const
    {
        const float y0 = box.getY();
        juce::Path p;
        p.startNewSubPath (mx (box, 3.0f, mirror), y0 + 6.0f);
        p.lineTo (mx (box, 10.0f, mirror), y0 + 6.0f);
        p.cubicTo (mx (box, 12.9f, mirror), y0 + 6.0f, mx (box, 14.5f, mirror), y0 + 7.9f, mx (box, 14.5f, mirror), y0 + 9.8f);
        p.cubicTo (mx (box, 14.5f, mirror), y0 + 11.8f, mx (box, 12.9f, mirror), y0 + 13.5f, mx (box, 10.0f, mirror), y0 + 13.5f);
        p.lineTo (mx (box, 6.0f, mirror), y0 + 13.5f);

        p.startNewSubPath (mx (box, 6.2f, mirror), y0 + 2.6f);
        p.lineTo (mx (box, 2.8f, mirror), y0 + 6.0f);
        p.lineTo (mx (box, 6.2f, mirror), y0 + 9.4f);

        g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void paintSpeaker (juce::Graphics& g, juce::Rectangle<float> box) const
    {
        const float x0 = box.getX(), y0 = box.getY();
        juce::Path body;
        body.startNewSubPath (x0 + 1.5f, y0 + 6.0f);
        body.lineTo (x0 + 4.5f, y0 + 6.0f);
        body.lineTo (x0 + 8.5f, y0 + 2.5f);
        body.lineTo (x0 + 8.5f, y0 + 13.5f);
        body.lineTo (x0 + 4.5f, y0 + 10.0f);
        body.lineTo (x0 + 1.5f, y0 + 10.0f);
        body.closeSubPath();
        g.fillPath (body);
        g.strokePath (body, juce::PathStrokeType (1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        juce::Path extra;

        if (getToggleState())
        {
            extra.addCentredArc (x0 + 8.5f, y0 + 8.0f, 3.3f, 3.3f, 0.0f, 0.75f, 2.39f, true);
            extra.addCentredArc (x0 + 8.5f, y0 + 8.0f, 6.0f, 6.0f, 0.0f, 0.70f, 2.44f, true);
        }
        else
        {
            extra.startNewSubPath (x0 + 11.0f, y0 + 5.5f);
            extra.lineTo (x0 + 15.0f, y0 + 10.5f);
            extra.startNewSubPath (x0 + 15.0f, y0 + 5.5f);
            extra.lineTo (x0 + 11.0f, y0 + 10.5f);
        }

        g.strokePath (extra, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    static void paintHelp (juce::Graphics& g, juce::Rectangle<float> box)
    {
        const auto circle = box.reduced (0.5f);
        g.drawEllipse (circle, 1.4f);
        g.setFont (cb::ui::font (11.0f, 700));
        g.drawText ("?", circle.translated (0.0f, 0.2f), juce::Justification::centred, false);
    }

    Icon icon;
};

//==============================================================================
// I26: a combo box that knows when its selection changed because the user picked from its menu (as opposed to host
// automation, a preset / state load or a validator changing the parameter while the editor is open). Only such picks
// become the user's defaults for new instances (processor.rememberUserDefaults()).
class PickCombo : public juce::ComboBox,
                  private juce::ComboBox::Listener,
                  private juce::Timer
{
public:
    PickCombo() { addListener (this); }
    ~PickCombo() override { removeListener (this); }

    std::function<void()> onUserPick;

    void showPopup() override
    {
        arm();
        juce::ComboBox::showPopup();
    }

protected:
    // Called when the menu opens; the change notification that follows the menu closing counts as the user's pick.
    void arm()
    {
        armed = true;
        closedTicks = 0;
        startTimerHz (20);
    }

private:
    void comboBoxChanged (juce::ComboBox*) override
    {
        if (! armed)
            return;

        disarm();

        if (onUserPick != nullptr)
            onUserPick();
    }

    void timerCallback() override
    {
        // The selection arrives (async) right after the menu closes; a menu dismissed without a pick disarms shortly.
        if (! isPopupActive() && ++closedTicks > 10)
            disarm();
    }

    void disarm()
    {
        armed = false;
        stopTimer();
    }

    bool armed = false;
    int closedTicks = 0;
};

//==============================================================================
// Key root selector (I27): the closed box shows the key's own spelling (Db major, C# minor: Key::tonicName), the menu
// lists both enharmonic spellings ("C# / Db", "D# / Eb") so sample-pack keys like "D# min" or "G# maj" are easy to find.
class KeyRootCombo final : public PickCombo
{
public:
    void showPopup() override
    {
        arm();
        const int selectedId = getSelectedId();
        juce::PopupMenu menu;

        for (int i = 0; i < 12; ++i)
        {
            const juce::String sharp (cb::pitchClassName (i, false)), flat (cb::pitchClassName (i, true));
            juce::PopupMenu::Item item (sharp == flat ? sharp : sharp + " / " + flat);
            item.itemID = i + 1;
            item.isTicked = item.itemID == selectedId;
            menu.addItem (std::move (item));
        }

        menu.setLookAndFeel (&getLookAndFeel());
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this)
                                                      .withItemThatMustBeVisible (selectedId)
                                                      .withInitiallySelectedItem (selectedId)
                                                      .withMinimumWidth (juce::jmax (getWidth(), 120))
                                                      .withMaximumNumColumns (1)
                                                      .withStandardItemHeight (getHeight() - 2),
                            [safe = juce::Component::SafePointer<KeyRootCombo> (this)] (int result)
                            {
                                if (safe == nullptr)
                                    return;

                                safe->hidePopup();

                                if (result != 0)
                                    safe->setSelectedId (result);
                            });
    }
};

//==============================================================================
// Compact drop-down chip bound to a choice parameter: shows a short name ("Both", "Chords", "Bass"), the menu lists the
// parameter's full choice names with a hint on the right. Opens on left or right click (it only ever opens a menu).
class ChoiceChip final : public juce::Component, public juce::SettableTooltipClient
{
public:
    ChoiceChip (juce::RangedAudioParameter& p, juce::StringArray shortLabels, juce::StringArray menuHints,
                juce::String heading, const juce::String& accessibleTitle)
        : shortNames (std::move (shortLabels)), hints (std::move (menuHints)), menuHeading (std::move (heading)),
          attachment (p, [this] (float v)
                      {
                          selected = juce::roundToInt (v);
                          repaint();

                          if (auto* handler = getAccessibilityHandler())
                              handler->notifyAccessibilityEvent (juce::AccessibilityEvent::valueChanged);
                      })
    {
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (&p))
            fullNames = choice->choices;

        while (fullNames.size() < shortNames.size())
            fullNames.add (shortNames[fullNames.size()]);

        setTitle (accessibleTitle);
        setRepaintsOnMouseActivity (true);
        attachment.sendInitialUpdate();
    }

    int getIdealWidth() const
    {
        float w = 0.0f;

        for (const auto& s : shortNames)
            w = juce::jmax (w, cb::ui::textWidth (labelFont(), s));

        return (int) std::ceil (w) + 10 + 22;
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat().reduced (0.5f);
        const float radius = b.getHeight() * 0.5f;
        const bool hover = isMouseOver (true) && isEnabled();

        auto fill = menuOpen ? Col::panelHover : (hover ? Col::panelHover.withAlpha (0.85f) : Col::panelRaised);
        g.setGradientFill (juce::ColourGradient::vertical (fill.brighter (0.04f), b.getY(), fill.darker (0.05f), b.getBottom()));
        g.fillRoundedRectangle (b, radius);
        g.setColour (menuOpen ? Col::accent.withAlpha (0.7f) : Col::outline.brighter (hover ? 0.18f : 0.0f));
        g.drawRoundedRectangle (b, radius, 1.0f);

        auto text = b.withTrimmedLeft (10.0f).withTrimmedRight (20.0f);
        g.setFont (labelFont());
        g.setColour (hover || menuOpen ? Col::text : Col::text.withAlpha (0.88f));
        g.drawFittedText (shortNames[selected], text.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);

        const float cx = b.getRight() - 11.5f, cy = b.getCentreY();
        juce::Path chevron;
        chevron.startNewSubPath (cx - 3.0f, cy - 1.4f);
        chevron.lineTo (cx, cy + 1.6f);
        chevron.lineTo (cx + 3.0f, cy - 1.4f);
        g.setColour (hover || menuOpen ? Col::text : Col::textDim);
        g.strokePath (chevron, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        if (isEnabled() && ! menuOpen)
            showMenu();
    }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        struct Value final : public juce::AccessibilityTextValueInterface
        {
            explicit Value (ChoiceChip& c) : owner (c) {}
            bool isReadOnly() const override { return false; }
            juce::String getCurrentValueAsString() const override { return owner.fullNames[owner.selected]; }

            void setValueAsString (const juce::String& newValue) override
            {
                const int index = owner.fullNames.indexOf (newValue.trim(), true);

                if (index >= 0)
                    owner.attachment.setValueAsCompleteGesture ((float) index);
            }

            ChoiceChip& owner;
        };

        return std::make_unique<juce::AccessibilityHandler> (
            *this, juce::AccessibilityRole::comboBox,
            juce::AccessibilityActions().addAction (juce::AccessibilityActionType::showMenu, [this] { showMenu(); })
                                        .addAction (juce::AccessibilityActionType::press, [this] { showMenu(); }),
            juce::AccessibilityHandler::Interfaces { std::make_unique<Value> (*this) });
    }

private:
    static juce::Font labelFont() { return cb::ui::font (11.5f, 600); }

    void showMenu()
    {
        juce::PopupMenu menu;

        if (menuHeading.isNotEmpty())
            menu.addSectionHeader (menuHeading);

        for (int i = 0; i < fullNames.size(); ++i)
        {
            juce::PopupMenu::Item item (fullNames[i]);
            item.itemID = i + 1;
            item.isTicked = i == selected;
            item.shortcutKeyDescription = hints[i];
            menu.addItem (std::move (item));
        }

        menuOpen = true;
        repaint();

        menu.setLookAndFeel (&getLookAndFeel());
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMinimumWidth (getWidth()),
                            [safe = juce::Component::SafePointer<ChoiceChip> (this)] (int result)
                            {
                                if (safe == nullptr)
                                    return;

                                safe->menuOpen = false;
                                safe->repaint();

                                if (result > 0)
                                    safe->attachment.setValueAsCompleteGesture ((float) (result - 1));
                            });
    }

    juce::StringArray shortNames, fullNames, hints;
    juce::String menuHeading;
    int selected = 0;
    bool menuOpen = false;
    juce::ParameterAttachment attachment;
};

//==============================================================================
// Help card: the three ways to use Chordbloom in FL Studio, plus tips. Text supports **bold** runs.
juce::AttributedString richText (const juce::String& markup, float size, juce::Colour normal, juce::Colour strong)
{
    juce::AttributedString s;
    s.setWordWrap (juce::AttributedString::byWord);
    s.setJustification (juce::Justification::topLeft);
    s.setLineSpacing (2.5f);

    int start = 0;
    bool bold = false;

    for (;;)
    {
        const int index = markup.indexOf (start, "**");
        const auto piece = index < 0 ? markup.substring (start) : markup.substring (start, index);

        if (piece.isNotEmpty())
            s.append (piece, cb::ui::font (size, bold ? 600 : 400), bold ? strong : normal);

        if (index < 0)
            break;

        start = index + 2;
        bold = ! bold;
    }

    return s;
}

class CloseButton final : public juce::Button
{
public:
    CloseButton() : juce::Button ("Close")
    {
        setTitle ("Close help");
        setTooltip ("Close");
    }

    void mouseDown (const juce::MouseEvent& e) override { if (! e.mods.isPopupMenu()) juce::Button::mouseDown (e); }
    void mouseUp (const juce::MouseEvent& e) override   { if (! e.mods.isPopupMenu()) juce::Button::mouseUp (e); }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        const auto b = getLocalBounds().toFloat().reduced (1.0f);

        if (highlighted || down)
        {
            g.setColour (Col::panelHover.withAlpha (down ? 1.0f : 0.8f));
            g.fillEllipse (b);
        }

        const auto c = b.getCentre();
        const float s = 4.0f;
        juce::Path x;
        x.startNewSubPath (c.x - s, c.y - s);
        x.lineTo (c.x + s, c.y + s);
        x.startNewSubPath (c.x + s, c.y - s);
        x.lineTo (c.x - s, c.y + s);
        g.setColour (highlighted ? Col::text : Col::textDim);
        g.strokePath (x, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
};

class HelpCard final : public juce::Component
{
public:
    static constexpr int kDefaultWidth = 640;

    explicit HelpCard (int width, bool paintBackground) : paintsBackground (paintBackground)
    {
        setTitle ("How to use Chordbloom in FL Studio");
        close.onClick = [this] { if (onClose != nullptr) onClose(); };
        addAndMakeVisible (close);

        sections = {
            { "1", "Hear it right here", Col::tonic,
              "Click a pad to hear it, double-click or drag it into the progression, then press **Play**. "
              "The **speaker**, tone menu and volume at the top control Chordbloom's built-in sound." },
            { "2", "Play it on your own synth (MIDI out)", Col::secondary,
              "In Chordbloom's wrapper open **settings (gear) > MIDI** and set **Output port** to a number, e.g. 1. "
              "In your synth's wrapper set **MIDI > Input port** to the same number, then click Chordbloom's "
              "**speaker** off so you only hear your synth. **MIDI out part** sends Chords + Bass, Chords only or "
              "Bass only (channels 1 / 2); for a separate bass synth, clone the Chordbloom channel and set the clone "
              "to **Bass** on its own port." },
            { "3", "Drag it into FL", Col::subdominant,
              "Drag the **Chords**, **Bass** or **Both** chips (bottom right) onto the playlist, a channel or the piano "
              "roll, or drag a single pad or progression block straight out. Drop the chords into your melody's "
              "pattern to see them as ghost notes." }
        };

        tips = {
            "**Play with FL** loops the progression along with FL's transport. Turn it off once the MIDI is in your "
            "project, or you will hear the chords twice.",
            "**Spark** and **Presets** (progression header): a fresh idea or a proven progression in your key.",
            "**Right-click** a progression block for length, inversion, octave, duplicate and delete. Scroll on it to "
            "change its length.",
            "**Chord trigger**: white keys C to B on your MIDI keyboard play the key's chords I to VII "
            "(**7ths** for seventh chords)."
        };

        setSize (width > 0 ? width : kDefaultWidth, 100);
        setSize (getWidth(), layoutContent (getWidth()));
    }

    std::function<void()> onClose;

    // Ideal height at `width` with the largest text size that fits `maxHeight` (0 = no limit). Keeps that text size.
    int fitHeight (int width, int maxHeight = 0)
    {
        for (const float size : { 12.5f, 12.0f, 11.5f })
        {
            bodySize = size;
            const int h = layoutContent (width);

            if (maxHeight <= 0 || h <= maxHeight)
                return h;
        }

        return layoutContent (width);
    }

    void resized() override
    {
        layoutContent (getWidth());
        close.setBounds (getWidth() - kPad - 22 + 8, 12, 26, 26);
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();

        if (paintsBackground)
            paintCardBackground (g, bounds);

        // Title
        g.setFont (cb::ui::font (16.0f, 700));
        g.setColour (Col::text);
        g.drawText ("How to use Chordbloom in FL Studio",
                    juce::Rectangle<float> ((float) kPad, 15.0f, bounds.getWidth() - 2.0f * kPad - 30.0f, 22.0f),
                    juce::Justification::centredLeft, true);
        g.setFont (cb::ui::font (12.0f));
        g.setColour (Col::textDim);
        g.drawText ("Three ways to hear your chords and get them into your project.",
                    juce::Rectangle<float> ((float) kPad, 37.0f, bounds.getWidth() - 2.0f * kPad - 30.0f, 16.0f),
                    juce::Justification::centredLeft, true);

        for (const auto& s : laidOut)
        {
            // Number badge
            const auto badge = juce::Rectangle<float> ((float) kPad, s.titleY - 2.0f, 24.0f, 24.0f);
            g.setColour (s.colour.withAlpha (0.16f));
            g.fillEllipse (badge);
            g.setColour (s.colour.withAlpha (0.65f));
            g.drawEllipse (badge.reduced (0.5f), 1.0f);
            g.setFont (cb::ui::font (12.0f, 700));
            g.setColour (s.colour.brighter (0.15f));
            g.drawText (s.number, badge, juce::Justification::centred, false);

            g.setFont (cb::ui::font (13.5f, 600));
            g.setColour (Col::text);
            g.drawText (s.title, juce::Rectangle<float> (textX(), s.titleY, bounds.getWidth() - textX() - kPad, 20.0f),
                        juce::Justification::centredLeft, true);

            s.body.draw (g, juce::Rectangle<float> (textX(), s.bodyY, bounds.getWidth() - textX() - kPad, s.body.getHeight()));
        }

        // Tips inset
        if (! tipsArea.isEmpty())
        {
            g.setColour (Col::background.withAlpha (0.55f));
            g.fillRoundedRectangle (tipsArea, 8.0f);
            g.setColour (Col::outline.withAlpha (0.6f));
            g.drawRoundedRectangle (tipsArea.reduced (0.5f), 8.0f, 1.0f);

            cb::ui::drawSectionTitle (g, "Tips", tipsArea.reduced (14.0f, 0.0f).withY (tipsArea.getY() + 9.0f)
                                                         .withHeight (14.0f).toNearestInt());

            for (const auto& t : laidOutTips)
            {
                g.setColour (Col::accent.withAlpha (0.8f));
                g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre ({ t.area.getX() - 9.0f, t.area.getY() + 8.0f }));
                t.layout.draw (g, t.area);
            }
        }
    }

    static void paintCardBackground (juce::Graphics& g, juce::Rectangle<float> bounds)
    {
        const auto b = bounds.reduced (0.5f);
        g.setGradientFill (juce::ColourGradient::vertical (cardTop(), b.getY(), cardBottom(), b.getBottom()));
        g.fillRoundedRectangle (b, 10.0f);
        g.setColour (Col::outline.brighter (0.12f));
        g.drawRoundedRectangle (b, 10.0f, 1.0f);
    }

    static juce::Colour cardTop()    { return Col::panel.brighter (0.07f); }
    static juce::Colour cardBottom() { return Col::panel.darker (0.02f); }

private:
    static constexpr int kPad = 20;
    static float textX() { return (float) kPad + 36.0f; }

    struct Section
    {
        juce::String number, title;
        juce::Colour colour;
        juce::String body;
    };

    struct LaidOutSection
    {
        juce::String number, title;
        juce::Colour colour;
        juce::TextLayout body;
        float titleY = 0.0f, bodyY = 0.0f;
    };

    struct LaidOutTip
    {
        juce::TextLayout layout;
        juce::Rectangle<float> area;
    };

    int layoutContent (int width)
    {
        laidOut.clear();
        laidOutTips.clear();

        const float w = (float) width;
        const float bodyW = w - textX() - (float) kPad;
        float y = 66.0f;

        for (const auto& s : sections)
        {
            LaidOutSection l;
            l.number = s.number;
            l.title = s.title;
            l.colour = s.colour;
            l.titleY = y;
            l.bodyY = y + 22.0f;
            l.body.createLayout (richText (s.body, bodySize, Col::textDim.brighter (0.12f), Col::text), bodyW);
            y = l.bodyY + l.body.getHeight() + 14.0f;
            laidOut.push_back (std::move (l));
        }

        // Tips: two columns when there is room, one otherwise.
        const float tipsTop = y + 2.0f;
        const float innerX = (float) kPad + 28.0f, innerW = w - 2.0f * (float) kPad - 28.0f - 14.0f;
        const int columns = width >= 520 ? 2 : 1;
        const float columnGap = 34.0f;
        const float columnW = columns == 2 ? (innerW - columnGap) * 0.5f : innerW;
        float rowY = tipsTop + 30.0f;

        for (int i = 0; i < tips.size(); i += columns)
        {
            float rowH = 0.0f;

            for (int c = 0; c < columns && i + c < tips.size(); ++c)
            {
                LaidOutTip l;
                l.layout.createLayout (richText (tips[i + c], bodySize - 0.5f, Col::textDim.brighter (0.08f), Col::text),
                                       columnW);
                l.area = { innerX + (float) c * (columnW + columnGap), rowY, columnW, l.layout.getHeight() };
                rowH = juce::jmax (rowH, l.layout.getHeight());
                laidOutTips.push_back (std::move (l));
            }

            rowY += rowH + 8.0f;
        }

        tipsArea = { (float) kPad, tipsTop, w - 2.0f * (float) kPad, rowY - tipsTop + 4.0f };
        return (int) std::ceil (tipsArea.getBottom() + 18.0f);
    }

    bool paintsBackground = true;
    float bodySize = 12.5f;
    std::vector<Section> sections;
    juce::StringArray tips;
    std::vector<LaidOutSection> laidOut;
    std::vector<LaidOutTip> laidOutTips;
    juce::Rectangle<float> tipsArea;
    CloseButton close;
};

//==============================================================================
// Floats over the whole editor: dims it slightly and shows the help card in a speech bubble pointing at the "?" button.
// A click anywhere outside the card dismisses it. Never takes keyboard focus.
class HelpPopover final : public juce::Component, private juce::ComponentListener
{
public:
    HelpPopover (juce::Component& hostComponent, juce::Component& anchorComponent, std::function<void()> dismissFn)
        : host (&hostComponent), anchor (&anchorComponent), onDismiss (std::move (dismissFn)), card (0, false)
    {
        setTitle ("Help");
        card.onClose = [this] { dismiss(); };
        addAndMakeVisible (card);
        host->addComponentListener (this);
        cb::ui::neverTakeKeyboardFocus (*this);
    }

    ~HelpPopover() override
    {
        if (host != nullptr)
            host->removeComponentListener (this);
    }

    void updateLayout()
    {
        if (host == nullptr || anchor == nullptr)
            return;

        setBounds (host->getLocalBounds());
        anchorArea = host->getLocalArea (anchor, anchor->getLocalBounds()).toFloat();

        const int margin = 10;
        const int width = juce::jmin (HelpCard::kDefaultWidth, getWidth() - 2 * margin);
        const int top = juce::roundToInt (anchorArea.getBottom()) + kArrow + 2;
        const int height = juce::jmin (card.fitHeight (width, getHeight() - top - margin), getHeight() - top - margin);
        const int right = juce::jmin (getWidth() - margin, juce::roundToInt (anchorArea.getCentreX()) + 44);
        const int x = juce::jmax (margin, right - width);

        card.setBounds (x, top, width, juce::jmax (40, height));
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.28f));

        const auto body = card.getBounds().toFloat().reduced (0.5f);
        const float radius = 10.0f, half = 9.0f;
        const float tipX = juce::jlimit (body.getX() + radius + half, body.getRight() - radius - half, anchorArea.getCentreX());
        const float tipY = juce::jmax (anchorArea.getBottom() + 2.0f, body.getY() - (float) kArrow);

        // Arrow (open path along its two slanted sides) on top of the card's rounded body.
        juce::Path arrow;
        arrow.startNewSubPath (tipX - half, body.getY());
        arrow.lineTo (tipX, tipY);
        arrow.lineTo (tipX + half, body.getY());

        juce::Path shape;
        shape.addRoundedRectangle (body, radius);
        shape.addTriangle (tipX - half, body.getY() + 1.0f, tipX, tipY, tipX + half, body.getY() + 1.0f);

        juce::DropShadow (juce::Colours::black.withAlpha (0.55f), 28, { 0, 10 }).drawForPath (g, shape);

        g.setGradientFill (juce::ColourGradient::vertical (HelpCard::cardTop(), body.getY(), HelpCard::cardBottom(), body.getBottom()));
        g.fillRoundedRectangle (body, radius);
        g.setColour (Col::outline.brighter (0.12f));
        g.drawRoundedRectangle (body, radius, 1.0f);

        // Arrow: fill, cover the card outline under its base, then outline its two sides.
        g.setColour (HelpCard::cardTop());
        g.fillPath (arrow);
        g.fillRect (juce::Rectangle<float> (tipX - half + 1.0f, body.getY() - 0.5f, 2.0f * half - 2.0f, 1.5f));
        g.setColour (Col::outline.brighter (0.12f));
        g.strokePath (arrow, juce::PathStrokeType (1.0f, juce::PathStrokeType::mitered));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! card.getBounds().contains (e.getPosition()))
            dismiss();
    }

private:
    static constexpr int kArrow = 8;

    void dismiss()
    {
        if (onDismiss != nullptr)
            onDismiss();
    }

    void componentMovedOrResized (juce::Component&, bool, bool wasResized) override
    {
        if (wasResized && isVisible())
            updateLayout();
    }

    void componentBeingDeleted (juce::Component& c) override
    {
        if (&c == host.getComponent())
            host = nullptr;
    }

    juce::Component::SafePointer<juce::Component> host, anchor;
    std::function<void()> onDismiss;
    juce::Rectangle<float> anchorArea;
    HelpCard card;
};
} // namespace

//==============================================================================
struct HeaderBar::Impl
{
    Impl (HeaderBar& o, ChordbloomProcessor& p)
        : owner (o), processor (p), apvts (p.getAPVTS()),
          outPart (*apvts.getParameter (cb::ParamIDs::midiOutPart), { "Both", "Chords", "Bass" },
                   { "ch 1 + 2", "ch 1", "ch 2" }, "MIDI out sends", "MIDI out part")
    {
        namespace id = cb::ParamIDs;

        // Key root: item texts follow the key's spelling (Db major / C# minor), refreshed on scale changes; the menu
        // itself lists both spellings (KeyRootCombo::showPopup).
        for (int i = 0; i < 12; ++i)
            keyRoot.addItem (cb::pitchClassName (i, true), i + 1);

        for (int i = 0; i < cb::numScaleTypes; ++i)
        {
            if (i == (int) cb::ScaleType::Dorian)
                scale.addSectionHeading ("Modes");

            scale.addItem (cb::scaleInfo ((cb::ScaleType) i).name, i + 1);

            if (i == 0)
                scale.addSectionHeading ("Minor");
        }

        if (auto* toneParam = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (id::previewTone)))
            tone.addItemList (toneParam->choices, 1);

        for (auto* c : { static_cast<juce::ComboBox*> (&keyRoot), static_cast<juce::ComboBox*> (&scale) })
        {
            c->getProperties().set (cb::ui::kPropProminent, true);
            c->setJustificationType (juce::Justification::centredLeft);
        }

        keyRootAtt = std::make_unique<APVTS::ComboBoxAttachment> (apvts, id::keyRoot, keyRoot);
        scaleAtt   = std::make_unique<APVTS::ComboBoxAttachment> (apvts, id::scale, scale);
        toneAtt    = std::make_unique<APVTS::ComboBoxAttachment> (apvts, id::previewTone, tone);

        sound.setClickingTogglesState (true);
        soundAtt = std::make_unique<APVTS::ButtonAttachment> (apvts, id::previewOn, sound);
        midiOutAtt = std::make_unique<APVTS::ButtonAttachment> (apvts, id::midiOut, midiOut);
        syncAtt = std::make_unique<APVTS::ButtonAttachment> (apvts, id::syncToHost, sync);

        volume.setSliderStyle (juce::Slider::LinearHorizontal);
        volume.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        volumeAtt = std::make_unique<APVTS::SliderAttachment> (apvts, id::previewVolume, volume);
        volume.textFromValueFunction = [] (double v)
        {
            return v <= -47.9 ? juce::String ("-inf dB") : juce::String (v, 1) + " dB";
        };

        scale.onChange = [this] { updateKeyNames(); };

        // I26: what the user picks here (and only that) becomes the starting key / scale / tone of new instances.
        for (auto* c : { static_cast<PickCombo*> (&keyRoot), &scale, &tone })
            c->onUserPick = [this] { processor.rememberUserDefaults(); };
        midiOut.onStateChange = [this] { updatePartLook(); };

        undo.onClick = [this] { processor.undo(); owner.refresh(); };
        redo.onClick = [this] { processor.redo(); owner.refresh(); };
        help.onClick = [this] { owner.setHelpVisible (! owner.isHelpVisible()); };

        keyRoot.setTooltip ("Key: the root note. Every chord in the palette is built from this key.");
        scale.setTooltip ("Scale or mode: decides which chords are diatonic and which ones count as borrowed.");
        sound.setTooltip ("Built-in sound on/off: the instrument you hear while auditioning and playing. Turn it off when "
                          "Chordbloom's MIDI plays another instrument in FL.");
        tone.setTooltip ("Built-in sound: the instrument's tone.");
        volume.setTooltip ("Built-in sound volume (double-click to reset).");
        midiOut.setTooltip ("MIDI out: send the chords (channel 1) and bass (channel 2) to FL Studio, to play them on "
                            "another instrument. Click ? for the routing steps.");
        outPart.setTooltip ("MIDI out part: what Chordbloom sends to FL: chords and bass, chords only or bass only. "
                            "Two Chordbloom channels (one Chords, one Bass) can feed two different synths.");
        sync.setTooltip ("Play with FL: while FL Studio plays, Chordbloom loops the progression in time with it "
                         "(built-in sound and MIDI out). Turn it off once you've dragged the chords into FL, "
                         "or you will hear them twice.");
        undo.setTooltip ("Undo the last progression edit");
        redo.setTooltip ("Redo");
        help.setTooltip ("How to use Chordbloom in FL Studio");

        keyRoot.setTitle ("Key");
        scale.setTitle ("Scale");
        tone.setTitle ("Built-in sound tone");
        volume.setTitle ("Built-in sound volume");
        midiOut.setTitle ("MIDI out");
        sync.setTitle ("Play with FL");

        for (juce::Component* c : std::initializer_list<juce::Component*> { &keyRoot, &scale, &sound, &tone, &volume,
                                                                            &midiOut, &outPart, &sync, &undo, &redo,
                                                                            &help })
        {
            // Never take keyboard focus away from the host (FL Studio's spacebar / shortcuts keep working).
            c->setWantsKeyboardFocus (false);
            c->setMouseClickGrabsKeyboardFocus (false);
            owner.addAndMakeVisible (c);
        }

        int size = 0;

        if (auto* data = ChordbloomBinary::getNamedResource ("logo_png", size))
        {
            auto full = juce::ImageCache::getFromMemory (data, size);

            if (full.isValid())
            {
                logo96 = full.getWidth() > 120 ? halveImage (full) : full;
                logo48 = logo96.getWidth() > 60 ? halveImage (logo96) : logo96;
            }
        }

        updateKeyNames();
        updatePartLook();
    }

    void updateKeyNames()
    {
        const int scaleIndex = juce::jlimit (0, cb::numScaleTypes - 1, scale.getSelectedItemIndex());

        for (int i = 0; i < 12; ++i)
        {
            cb::Key key;
            key.tonic = i;
            key.scale = (cb::ScaleType) scaleIndex;
            keyRoot.changeItemText (i + 1, juce::String (key.tonicName()));
        }

        keyRoot.setSelectedId (keyRoot.getSelectedId(), juce::dontSendNotification);   // re-sync the shown text
    }

    void updatePartLook()
    {
        outPart.setAlpha (midiOut.getToggleState() ? 1.0f : 0.45f);
    }

    void paintBrand (juce::Graphics& g) const
    {
        auto area = brandArea;
        const auto logoArea = area.removeFromLeft (30).withSizeKeepingCentre (30, 30).toFloat();
        const float pixelScale = g.getInternalContext().getPhysicalPixelScaleFactor();

        if (logo48.isValid())
        {
            const auto& img = (logoArea.getWidth() * pixelScale > 48.0f && logo96.isValid()) ? logo96 : logo48;
            g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
            g.drawImage (img, logoArea, juce::RectanglePlacement::centred);
        }
        else
        {
            paintVectorFlower (g, logoArea);
        }

        if (! showWordmark)
            return;

        auto textArea = area.withTrimmedLeft (10).toFloat();
        const auto f = cb::ui::font (19.0f, 700);
        g.setFont (f);
        const float w1 = cb::ui::textWidth (f, "Chord");
        g.setColour (Col::text);
        g.drawText ("Chord", textArea, juce::Justification::centredLeft, false);
        g.setColour (Col::accent);
        g.drawText ("bloom", textArea.withTrimmedLeft (w1), juce::Justification::centredLeft, false);
    }

    static void paintVectorFlower (juce::Graphics& g, juce::Rectangle<float> area)
    {
        const auto c = area.getCentre();
        const float r = area.getWidth() * 0.5f;
        const juce::Colour petals[] = { Col::tonic, Col::subdominant, Col::dominant, Col::borrowed, Col::secondary };

        for (int i = 0; i < 5; ++i)
        {
            const float angle = juce::MathConstants<float>::twoPi * (float) i / 5.0f;
            juce::Path petal;
            petal.addEllipse (-r * 0.32f, -r * 0.98f, r * 0.64f, r * 0.82f);
            g.setColour (petals[i]);
            g.fillPath (petal, juce::AffineTransform::rotation (angle).translated (c));
        }

        g.setColour (Col::accent.brighter (0.2f));
        g.fillEllipse (juce::Rectangle<float> (r * 0.6f, r * 0.6f).withCentre (c));
    }

    HeaderBar& owner;
    ChordbloomProcessor& processor;
    APVTS& apvts;

    KeyRootCombo keyRoot;
    PickCombo scale, tone;
    IconButton sound { "Built-in sound", IconButton::Icon::speaker };
    cb::ui::LeftClickOnly<juce::Slider> volume;
    cb::ui::LeftClickOnly<juce::ToggleButton> midiOut { "MIDI out" }, sync { "Play with FL" };
    ChoiceChip outPart;
    IconButton undo { "Undo", IconButton::Icon::undo }, redo { "Redo", IconButton::Icon::redo };
    IconButton help { "Help", IconButton::Icon::help };

    std::unique_ptr<APVTS::ComboBoxAttachment> keyRootAtt, scaleAtt, toneAtt;
    std::unique_ptr<APVTS::ButtonAttachment> soundAtt, midiOutAtt, syncAtt;
    std::unique_ptr<APVTS::SliderAttachment> volumeAtt;

    std::unique_ptr<HelpPopover> helpPopover;

    juce::Image logo96, logo48;
    juce::Rectangle<int> brandArea;
    bool showWordmark = true;
    std::vector<int> separators;
};

//==============================================================================
HeaderBar::HeaderBar (ChordbloomProcessor& p) : impl (std::make_unique<Impl> (*this, p))
{
    setTitle ("Header");
    refresh();
}

HeaderBar::~HeaderBar()
{
    impl->helpPopover.reset();
}

void HeaderBar::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setGradientFill (juce::ColourGradient::vertical (juce::Colour (0xff222a33), b.getY(),
                                                       juce::Colour (0xff1d232b), b.getBottom()));
    g.fillRect (b);

    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillRect (b.withTop (b.getBottom() - 1.0f));
    g.setColour (juce::Colours::white.withAlpha (0.03f));
    g.fillRect (b.withHeight (1.0f));

    for (int x : impl->separators)
    {
        g.setColour (Col::outline.withAlpha (0.8f));
        g.fillRect (juce::Rectangle<float> ((float) x, b.getY() + 16.0f, 1.0f, b.getHeight() - 32.0f));
    }

    impl->paintBrand (g);
}

void HeaderBar::resized()
{
    auto& d = *impl;
    auto r = getLocalBounds().reduced (16, 0);
    const int h = 30;
    const int gap = 8;
    int groupGap = 14;   // each side of a separator

    // Preferred widths, squeezed below when the editor is narrow.
    const int wordmarkW = (int) std::ceil (cb::ui::textWidth (cb::ui::font (19.0f, 700), "Chordbloom"));
    int brandW = 30 + 10 + wordmarkW;
    int rootW = 70;
    int scaleW = 160;
    int toneW = 122;
    int volumeW = 104;
    const int iconW = 28;
    const int partW = d.outPart.getIdealWidth();

    d.midiOut.setSize (10, 24);
    d.midiOut.changeWidthToFitText();
    d.sync.setSize (10, 24);
    d.sync.changeWidthToFitText();

    auto required = [&]
    {
        const int left = brandW + 2 * groupGap + rootW + gap + scaleW;
        const int right = iconW + 4 + toneW + gap + volumeW
                          + 2 * groupGap + d.midiOut.getWidth() + 6 + partW + 14 + d.sync.getWidth()
                          + 2 * groupGap + iconW * 2 + 2 + 8 + iconW;
        return left + right + 12;   // + a minimum gap between the left and right groups
    };

    int excess = required() - r.getWidth();

    auto squeeze = [&excess] (int& value, int minValue)
    {
        if (excess <= 0)
            return;

        const int take = juce::jmin (excess, value - minValue);
        value -= take;
        excess -= take;
    };

    squeeze (volumeW, 72);
    squeeze (toneW, 100);
    squeeze (scaleW, 140);
    d.showWordmark = excess <= 0;

    if (! d.showWordmark)
    {
        brandW = 30;
        excess = required() - r.getWidth();
    }

    if (excess > 0)
    {
        groupGap = 10;
        excess = required() - r.getWidth();
    }

    squeeze (scaleW, 120);
    squeeze (volumeW, 56);
    squeeze (toneW, 90);
    squeeze (rootW, 62);

    auto centred = [h] (juce::Rectangle<int> area) { return area.withSizeKeepingCentre (area.getWidth(), h); };
    d.separators.clear();

    // left: brand + key
    d.brandArea = r.removeFromLeft (brandW);
    r.removeFromLeft (groupGap);
    d.separators.push_back (r.getX());
    r.removeFromLeft (groupGap);
    d.keyRoot.setBounds (centred (r.removeFromLeft (rootW)).withSizeKeepingCentre (rootW, 32));
    r.removeFromLeft (gap);
    d.scale.setBounds (centred (r.removeFromLeft (scaleW)).withSizeKeepingCentre (scaleW, 32));

    // right: help, undo / redo | Play with FL, MIDI out + part | sound group
    d.help.setBounds (centred (r.removeFromRight (iconW)).withSizeKeepingCentre (iconW, 28));
    r.removeFromRight (8);
    d.redo.setBounds (centred (r.removeFromRight (iconW)).withSizeKeepingCentre (iconW, 28));
    r.removeFromRight (2);
    d.undo.setBounds (centred (r.removeFromRight (iconW)).withSizeKeepingCentre (iconW, 28));
    r.removeFromRight (groupGap);
    d.separators.push_back (r.getRight());
    r.removeFromRight (groupGap);

    d.sync.setBounds (r.removeFromRight (d.sync.getWidth()).withSizeKeepingCentre (d.sync.getWidth(), 24));
    r.removeFromRight (14);
    d.outPart.setBounds (r.removeFromRight (partW).withSizeKeepingCentre (partW, 22));
    r.removeFromRight (6);
    d.midiOut.setBounds (r.removeFromRight (d.midiOut.getWidth()).withSizeKeepingCentre (d.midiOut.getWidth(), 24));
    r.removeFromRight (groupGap);
    d.separators.push_back (r.getRight());
    r.removeFromRight (groupGap);

    d.volume.setBounds (centred (r.removeFromRight (volumeW)));
    r.removeFromRight (gap);
    d.tone.setBounds (centred (r.removeFromRight (toneW)));
    r.removeFromRight (4);
    d.sound.setBounds (centred (r.removeFromRight (iconW)));

    if (auto* top = getTopLevelComponent(); top != nullptr && top != this)
        d.volume.setPopupDisplayEnabled (true, false, top);

    if (d.helpPopover != nullptr && d.helpPopover->isVisible())
        d.helpPopover->updateLayout();
}

void HeaderBar::refresh()
{
    auto& d = *impl;
    d.undo.setEnabled (d.processor.canUndo());
    d.redo.setEnabled (d.processor.canRedo());
    d.updateKeyNames();
}

void HeaderBar::setHelpVisible (bool shouldBeVisible)
{
    auto& d = *impl;
    auto* host = getParentComponent();

    if (shouldBeVisible && host != nullptr)
    {
        if (d.helpPopover == nullptr)
        {
            juce::Component::SafePointer<HeaderBar> safeThis (this);
            d.helpPopover = std::make_unique<HelpPopover> (*host, d.help, [safeThis]
            {
                // Called from the popover's own mouse handler: hide now, never delete it from inside its callback.
                if (safeThis != nullptr)
                    safeThis->setHelpVisible (false);
            });
        }

        if (d.helpPopover->getParentComponent() != host)
            host->addChildComponent (*d.helpPopover);

        d.helpPopover->updateLayout();
        d.helpPopover->setVisible (true);
        d.helpPopover->toFront (false);
    }
    else if (d.helpPopover != nullptr)
    {
        d.helpPopover->setVisible (false);
    }

    d.help.setToggleState (isHelpVisible(), juce::dontSendNotification);
}

bool HeaderBar::isHelpVisible() const
{
    return impl->helpPopover != nullptr && impl->helpPopover->isVisible();
}

std::unique_ptr<juce::Component> HeaderBar::createHelpContent (int width)
{
    return std::make_unique<HelpCard> (width, true);
}
