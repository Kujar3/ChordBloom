#include "ui/MelodyRoll.h"

#include "ui/Theme.h"

#include <cmath>

namespace
{
namespace Col = cb::ui::Colours;

constexpr int kMinVisibleBeats = 16;              // = ProgressionLane: the roll's beat axis is the lane's track
int roundUpToBar (int beats) { return ((beats + 3) / 4) * 4; }

constexpr float kPadX = 12.0f, kPadY = 10.0f;     // horizontal inset = the lane's track inset
constexpr float kTargetRows = 17.0f;              // about 1 1/2 octaves visible
constexpr float kMinRowH = 9.0f, kMaxRowH = 16.0f;
constexpr int kDefaultCentreNote = 72;            // C6 in FL Studio's naming (the melody register above the chords)

// Roll settings kept in the APVTS state tree (saved with the plugin state, like the lane's Spark settings).
constexpr const char* kSnapProp = "cb.melodySnap";
constexpr const char* kGridProp = "cb.melodyGrid";
constexpr const char* kTopProp = "cb.melodyTopNote";
constexpr const char* kGhostsProp = "cb.melodyGhosts";

float widthOf (const juce::Font& f, const juce::String& s) { return juce::GlyphArrangement::getStringWidth (f, s); }
juce::String toJuce (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }
juce::String middleDot() { return juce::String::charToString ((juce::juce_wchar) 0x00b7); }

void noFocus (juce::Component& c)
{
    c.setWantsKeyboardFocus (false);
    c.setMouseClickGrabsKeyboardFocus (false);
}

// Note name with FL Studio's octave numbers (MIDI 60 = C5), spelled for the key: "C5", "F#4", "Bb5".
juce::String noteName (int midiNote, const cb::Key& key)
{
    try
    {
        return toJuce (key.spell (cb::pc (midiNote))) + juce::String (midiNote / 12);
    }
    catch (...)
    {
        return juce::String (midiNote);
    }
}

cb::HarmonicFunction functionOf (const cb::Chord& chord, const cb::Key& key)
{
    try
    {
        return cb::harmonicFunction (chord, key);
    }
    catch (...)
    {
        return cb::HarmonicFunction::Chromatic;
    }
}

//==============================================================================
// Segmented choice (Snap, Grid): the same look as the controls panel's segmented selectors, not bound to a parameter.
class Segments final : public juce::Component, public juce::SettableTooltipClient
{
public:
    Segments (juce::StringArray segmentLabels, const juce::String& accessibleTitle) : labels (std::move (segmentLabels))
    {
        noFocus (*this);
        setTitle (accessibleTitle);
        setDescription ("Choose one of: " + labels.joinIntoString (", "));
    }

    std::function<void (int)> onChange;

    void setSelected (int index)
    {
        if (index != selected)
        {
            selected = index;
            repaint();

            if (auto* handler = getAccessibilityHandler())
                handler->notifyAccessibilityEvent (juce::AccessibilityEvent::valueChanged);
        }
    }

    int getSelected() const noexcept { return selected; }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        const float radius = juce::jmin (6.0f, b.getHeight() * 0.5f);
        g.setColour (Col::background.brighter (0.035f));
        g.fillRoundedRectangle (b, radius);
        g.setColour (Col::outline.withAlpha (0.9f));
        g.drawRoundedRectangle (b.reduced (0.5f), radius, 1.0f);

        const int n = labels.size();
        const float segW = (b.getWidth() - 4.0f) / (float) juce::jmax (1, n);
        g.setFont (cb::ui::font (juce::jmin (11.5f, b.getHeight() * 0.5f), 600));

        for (int i = 0; i < n; ++i)
        {
            const auto seg = juce::Rectangle<float> (b.getX() + 2.0f + segW * (float) i, b.getY() + 2.0f, segW, b.getHeight() - 4.0f);
            const bool isSelected = i == selected;
            const bool isHover = i == hover;

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
                g.fillRect (juce::Rectangle<float> (seg.getX() - 0.5f, seg.getY() + seg.getHeight() * 0.28f, 1.0f,
                                                    seg.getHeight() * 0.44f));
            }

            g.setColour (isSelected ? Col::accent : (isHover ? Col::text : Col::textDim));
            g.drawFittedText (labels[i], seg.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.75f);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override { setHover (indexAt (e.position.x)); }
    void mouseExit (const juce::MouseEvent&) override   { setHover (-1); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())   // right-click never changes the setting
            return;

        choose (indexAt (e.position.x));
    }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        struct Value final : public juce::AccessibilityTextValueInterface
        {
            explicit Value (Segments& s) : owner (s) {}
            bool isReadOnly() const override { return false; }
            juce::String getCurrentValueAsString() const override { return owner.labels[owner.selected]; }
            void setValueAsString (const juce::String& v) override { owner.choose (owner.labels.indexOf (v.trim(), true)); }
            Segments& owner;
        };

        return std::make_unique<juce::AccessibilityHandler> (
            *this, juce::AccessibilityRole::comboBox,
            juce::AccessibilityActions().addAction (juce::AccessibilityActionType::press,
                                                    [this] { choose ((selected + 1) % juce::jmax (1, labels.size())); }),
            juce::AccessibilityHandler::Interfaces { std::make_unique<Value> (*this) });
    }

private:
    void choose (int index)
    {
        if (index < 0 || index >= labels.size() || index == selected)
            return;

        setSelected (index);

        if (onChange != nullptr)
            onChange (index);
    }

    int indexAt (float x) const
    {
        const int n = labels.size();
        const float segW = ((float) getWidth() - 4.0f) / (float) juce::jmax (1, n);
        return juce::jlimit (0, juce::jmax (0, n - 1), (int) ((x - 2.0f) / segW));
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
    int selected = 0, hover = -1;
};

//==============================================================================
// Small outlined pill with an icon (Clear, Ghosts). A right-click never acts like a click.
class ToolButton final : public juce::Button
{
public:
    enum class Icon { trash, ghosts };

    ToolButton (const juce::String& name, Icon i, const juce::String& label) : juce::Button (name), icon (i), text (label)
    {
        noFocus (*this);
        setTitle (name);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    bool compact = false;   // icon only

    int idealWidth (bool iconOnly) const
    {
        return iconOnly ? 30 : (int) std::ceil (12.0f + 11.0f + 6.0f + widthOf (labelFont(), text) + 12.0f);
    }

    void mouseDown (const juce::MouseEvent& e) override { if (! e.mods.isPopupMenu()) juce::Button::mouseDown (e); }
    void mouseDrag (const juce::MouseEvent& e) override { if (! e.mods.isPopupMenu()) juce::Button::mouseDrag (e); }
    void mouseUp (const juce::MouseEvent& e) override   { if (! e.mods.isPopupMenu()) juce::Button::mouseUp (e); }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const auto r = getLocalBounds().toFloat().reduced (0.5f);
        const bool enabled = isEnabled();
        const bool on = getToggleState();
        const float alpha = enabled ? 1.0f : 0.35f;
        const float radius = r.getHeight() * 0.5f;
        over = over && enabled;

        if (on)
        {
            g.setColour (Col::accent.withAlpha ((over ? 0.17f : 0.10f) * alpha));
            g.fillRoundedRectangle (r, radius);
        }
        else if (over || down)
        {
            g.setColour (down ? Col::panelRaised : Col::panelHover);
            g.fillRoundedRectangle (r, radius);
        }

        g.setColour ((on ? Col::accent.withAlpha (over ? 0.75f : 0.45f) : Col::outline).withMultipliedAlpha (alpha));
        g.drawRoundedRectangle (r, radius, 1.0f);

        const auto ink = (on ? Col::text : (over ? Col::text : Col::textDim)).withMultipliedAlpha (alpha);
        const auto iconInk = (on ? Col::accent : ink).withMultipliedAlpha (alpha);
        const float textW = widthOf (labelFont(), text);
        const float iconW = 11.0f;
        const float contentW = compact ? iconW : iconW + 6.0f + textW;
        const float x = std::round (r.getCentreX() - contentW * 0.5f);
        const float cy = r.getCentreY();

        g.setColour (iconInk);

        if (icon == Icon::trash)
        {
            g.fillRoundedRectangle (x, cy - 4.5f, iconW - 1.0f, 1.4f, 0.7f);
            g.fillRoundedRectangle (x + 3.0f, cy - 6.0f, iconW - 7.0f, 1.4f, 0.7f);
            juce::Path body;
            body.addRoundedRectangle (x + 1.2f, cy - 2.5f, iconW - 3.4f, 7.5f, 1.2f);
            g.strokePath (body, juce::PathStrokeType (1.3f));
        }
        else
        {
            // Two offset note bars: "another melody behind this one"
            g.drawRoundedRectangle (x, cy - 5.0f, iconW - 3.0f, 4.5f, 1.5f, 1.1f);
            g.setColour (iconInk.withMultipliedAlpha (0.55f));
            g.drawRoundedRectangle (x + 3.0f, cy + 0.5f, iconW - 3.0f, 4.5f, 1.5f, 1.1f);
        }

        if (! compact)
        {
            g.setFont (labelFont());
            g.setColour (ink);
            g.drawText (text, juce::Rectangle<float> (x + iconW + 6.0f, r.getY(), textW + 2.0f, r.getHeight()),
                        juce::Justification::centredLeft, false);
        }
    }

private:
    static juce::Font labelFont() { return cb::ui::font (12.0f, 600); }

    Icon icon;
    juce::String text;
};

using MelodyOnButton = cb::ui::LeftClickOnly<juce::ToggleButton>;
} // namespace

//==============================================================================
struct MelodyRoll::Impl final : public juce::Component,
                                private juce::Timer
{
    explicit Impl (ChordbloomProcessor& p)
        : processor (p),
          snapSegments ({ "Off", "Scale", "Chord" }, "Snap"),
          gridSegments ({ "1/4", "1/8", "1/16", "1/8T" }, "Grid"),
          clearButton ("Clear melody", ToolButton::Icon::trash, "Clear"),
          ghostButton ("Ghost notes", ToolButton::Icon::ghosts, "Ghosts")
    {
        noFocus (*this);
        setOpaque (false);
        setTitle ("Melody piano roll");
        setDescription ("Draw a melody over the progression. Rows light up with the scale and the chord at each beat.");

        snapSegments.setTooltip ("Snap: keep drawn and moved notes on the harmony.\n"
                                 "Off = any note, Scale = notes of the key (plus the chord's own tones), "
                                 "Chord = only the tones of the chord under the note");
        gridSegments.setTooltip ("Grid: the step notes start, move and resize on (hold Alt for no grid)");
        playToggle.setTooltip ("Play the melody with the progression: built-in sound and MIDI out on channel 3");
        clearButton.setTooltip ("Clear the melody (undo brings it back)");
        ghostButton.setClickingTogglesState (true);

        snapSegments.onChange = [this] (int i) { setSnap ((cb::MelodySnap) i); };
        gridSegments.onChange = [this] (int i) { setGrid ((cb::RateDivision) i); };
        clearButton.onClick = [this] { processor.clearMelody(); };
        ghostButton.onClick = [this]
        {
            showGhosts = ghostButton.getToggleState();
            processor.getAPVTS().state.setProperty (kGhostsProp, showGhosts, nullptr);
            repaint();
        };

        playToggle.setTitle ("Play melody");
        noFocus (playToggle);
        playAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
            processor.getAPVTS(), cb::ParamIDs::melodyOn, playToggle);

        for (juce::Component* c : { (juce::Component*) &snapSegments, (juce::Component*) &gridSegments,
                                    (juce::Component*) &playToggle, (juce::Component*) &clearButton,
                                    (juce::Component*) &ghostButton })
            addAndMakeVisible (c);

        ghostButton.setVisible (false);
        sync();
        startTimerHz (30);
    }

    ~Impl() override
    {
        stopTimer();

        if (auditioning)
            processor.stopPreview();
    }

    //==============================================================================
    // Settings
    void loadSettings()
    {
        const auto& state = processor.getAPVTS().state;
        snap = (cb::MelodySnap) juce::jlimit (0, cb::numMelodySnapModes - 1,
                                              (int) state.getProperty (kSnapProp, (int) cb::MelodySnap::Scale));
        grid = (cb::RateDivision) juce::jlimit (0, cb::numRateDivisions - 1,
                                                (int) state.getProperty (kGridProp, (int) cb::RateDivision::Eighth));
        showGhosts = (bool) state.getProperty (kGhostsProp, true);

        if (state.hasProperty (kTopProp))
            requestedTop = (int) state.getProperty (kTopProp);

        snapSegments.setSelected ((int) snap);
        gridSegments.setSelected ((int) grid);
        ghostButton.setToggleState (showGhosts, juce::dontSendNotification);

        if (requestedTop == topNote)
            requestedTop = -1;
    }

    void setSnap (cb::MelodySnap s)
    {
        snap = s;
        snapSegments.setSelected ((int) s);
        processor.getAPVTS().state.setProperty (kSnapProp, (int) s, nullptr);
        repaint();
    }

    void setGrid (cb::RateDivision r)
    {
        grid = r;
        gridSegments.setSelected ((int) r);
        processor.getAPVTS().state.setProperty (kGridProp, (int) r, nullptr);
        repaint();
    }

    double gridStep() const { return cb::rateInBeats (grid); }

    //==============================================================================
    // Data
    void sync()
    {
        loadSettings();   // a loaded project state may bring other roll settings
        progression = processor.getProgression();
        key = processor.getKey();
        total = progression.totalBeats();

        if (gesture == Gesture::none)
            working = processor.getMelody();

        slotNames.clear();
        slotColours.clear();

        for (const auto& slot : progression.slots)
        {
            juce::String name;
            try
            {
                name = toJuce (slot.chord.name (key));
            }
            catch (...)
            {
                name = "?";
            }
            slotNames.add (name);
            slotColours.push_back (cb::ui::functionColour (functionOf (slot.chord, key)));
        }

        ghosts = processor.getGhostMelodies();
        ghostSerial = processor.getGhostSerial();
        layout();
        repaint();
    }

    int parkedCount() const
    {
        int n = 0;

        for (const auto& note : working.notes)
            if (note.startBeat >= (double) total)
                ++n;

        return total > 0 ? n : 0;
    }

    //==============================================================================
    // Geometry
    void resized() override { layout(); }

    void layout()
    {
        auto inner = getLocalBounds().toFloat().reduced (kPadX, kPadY);
        compact = getHeight() < 230;

        toolbarArea = inner.removeFromTop (compact ? 24.0f : 26.0f);
        inner.removeFromTop (compact ? 4.0f : 6.0f);
        stripArea = inner.removeFromTop (compact ? 14.0f : 16.0f);
        inner.removeFromTop (2.0f);
        gridArea = inner;

        visibleBeats = std::max (kMinVisibleBeats, roundUpToBar (total));
        pxPerBeat = gridArea.getWidth() / (float) visibleBeats;
        rowH = juce::jlimit (kMinRowH, kMaxRowH, gridArea.getHeight() / kTargetRows);

        if (requestedTop >= 0)
        {
            topNote = requestedTop;
            requestedTop = -1;
        }
        else if (topNote < 0)
        {
            // First show: centre on the melody, else on the default melody register.
            int centre = kDefaultCentreNote;

            if (! working.empty())
            {
                int lo = 127, hi = 0;
                for (const auto& n : working.notes)
                {
                    lo = std::min (lo, n.note);
                    hi = std::max (hi, n.note);
                }
                centre = (lo + hi) / 2;
            }

            topNote = centre + visibleRows() / 2;
        }

        topNote = clampTop (topNote);
        layoutToolbar();
    }

    int visibleRows() const { return std::max (1, (int) std::floor (gridArea.getHeight() / rowH + 0.5f)); }

    int clampTop (int t) const
    {
        const int lowestTop = std::min (cb::kMelodyHighestNote, cb::kMelodyLowestNote + visibleRows() - 1);
        return juce::jlimit (lowestTop, cb::kMelodyHighestNote, t);
    }

    int bottomNote() const { return std::max (cb::kMelodyLowestNote, topNote - visibleRows() + 1); }

    void scrollTo (int newTop)
    {
        newTop = clampTop (newTop);

        if (newTop == topNote)
            return;

        topNote = newTop;
        processor.getAPVTS().state.setProperty (kTopProp, topNote, nullptr);
        repaint();
    }

    float beatToX (double beat) const { return gridArea.getX() + (float) beat * pxPerBeat; }
    double xToBeat (float x) const { return (double) ((x - gridArea.getX()) / pxPerBeat); }
    float rowTop (int note) const { return gridArea.getY() + (float) (topNote - note) * rowH; }
    int yToNote (float y) const { return topNote - (int) std::floor ((y - gridArea.getY()) / rowH); }

    juce::Rectangle<float> noteRect (const cb::MelodyNote& n) const
    {
        const float x0 = beatToX (n.startBeat);
        const float x1 = beatToX (n.startBeat + n.lengthBeats);
        return { x0 + 0.5f, rowTop (n.note) + 1.0f, std::max (3.0f, x1 - x0 - 1.0f), std::max (3.0f, rowH - 2.0f) };
    }

    void layoutToolbar()
    {
        const auto row = toolbarArea.withTrimmedRight ((float) rightInset).getSmallestIntegerContainer();
        const int segH = std::min (22, row.getHeight());
        const bool ghostsVisible = ! ghosts.empty();

        struct Level { bool readout, labels, buttonText, wideSegments, title; };
        const Level levels[] = {
            { true,  true,  true,  true,  true  },
            { false, true,  true,  true,  true  },
            { false, false, true,  true,  true  },
            { false, false, false, true,  true  },
            { false, false, false, false, true  },
            { false, false, false, false, false },
        };

        const auto readout = readoutText();
        const float titleW = widthOf (titleFont(), "MELODY") + 2.0f;
        const float readoutW = widthOf (readoutFont(), readout.first) + 2.0f
                             + (readout.second.isNotEmpty() ? 8.0f + widthOf (detailFont(), readout.second) : 0.0f);
        const float snapLabelW = widthOf (labelFont(), "Snap") + 2.0f;
        const float gridLabelW = widthOf (labelFont(), "Grid") + 2.0f;
        const int playW = 66;

        Level chosen = levels[std::size (levels) - 1];
        int snapW = 0, gridW = 0, clearW = 0, ghostW = 0;

        for (const auto& level : levels)
        {
            snapW = level.wideSegments ? 3 * 48 : 3 * 40;
            gridW = level.wideSegments ? 4 * 38 : 4 * 32;
            clearW = clearButton.idealWidth (! level.buttonText);
            ghostW = ghostsVisible ? ghostButton.idealWidth (! level.buttonText) : 0;

            const float needed = (level.title ? titleW + 12.0f : 0.0f) + (level.readout ? readoutW + 16.0f : 0.0f)
                                 + (level.labels ? snapLabelW + 6.0f + gridLabelW + 6.0f : 0.0f) + (float) snapW + 14.0f
                                 + (float) gridW + 14.0f + (float) playW + 8.0f + (float) (ghostW > 0 ? ghostW + 8 : 0)
                                 + (float) clearW;
            chosen = level;

            if (needed <= (float) row.getWidth())
                break;
        }

        showTitle = chosen.title;
        showReadout = chosen.readout;
        showLabels = chosen.labels;
        clearButton.compact = ! chosen.buttonText;
        ghostButton.compact = ! chosen.buttonText;

        // Right side, right to left: Clear, Ghosts, Play
        auto r = row;
        auto placeRight = [&r] (juce::Component& c, int w, int h)
        {
            w = std::min (w, std::max (0, r.getWidth()));
            c.setBounds (r.removeFromRight (w).withSizeKeepingCentre (w, h));
        };

        placeRight (clearButton, clearW, segH + 2);
        r.removeFromRight (8);
        ghostButton.setVisible (ghostsVisible);

        if (ghostsVisible)
        {
            placeRight (ghostButton, ghostW, segH + 2);
            r.removeFromRight (8);
        }

        placeRight (playToggle, playW, segH);
        r.removeFromRight (14);

        // Left side: title, readout, Snap, Grid
        float x = (float) r.getX();
        titleArea = readoutArea = snapLabelArea = gridLabelArea = {};

        if (showTitle)
        {
            titleArea = { x, (float) r.getY(), titleW, (float) r.getHeight() };
            x += titleW + 12.0f;
        }

        if (showReadout)
        {
            readoutArea = { x, (float) r.getY(), readoutW, (float) r.getHeight() };
            x += readoutW + 16.0f;
        }

        auto placeLeft = [&] (juce::Component& c, int w)
        {
            const int left = (int) std::round (x);
            w = std::min (w, std::max (0, r.getRight() - left));
            c.setBounds (juce::Rectangle<int> (left, r.getY(), w, r.getHeight()).withSizeKeepingCentre (w, segH));
            x = (float) (left + w);
        };

        if (showLabels)
        {
            snapLabelArea = { x, (float) r.getY(), snapLabelW, (float) r.getHeight() };
            x += snapLabelW + 6.0f;
        }

        placeLeft (snapSegments, snapW);
        x += 14.0f;

        if (showLabels)
        {
            gridLabelArea = { x, (float) r.getY(), gridLabelW, (float) r.getHeight() };
            x += gridLabelW + 6.0f;
        }

        placeLeft (gridSegments, gridW);
        clearButton.setEnabled (! working.empty());
        repaint();
    }

    static juce::Font titleFont()   { return cb::ui::font (10.5f, 600).withExtraKerningFactor (0.12f); }
    static juce::Font readoutFont() { return cb::ui::font (13.0f, 600); }
    static juce::Font detailFont()  { return cb::ui::font (12.0f, 400); }
    static juce::Font labelFont()   { return cb::ui::font (11.5f, 400); }

    std::pair<juce::String, juce::String> readoutText() const
    {
        const int n = (int) working.notes.size();
        const juce::String main = n == 0 ? juce::String ("Empty") : juce::String (n) + (n == 1 ? " note" : " notes");
        const int parked = parkedCount();
        return { main, parked > 0 ? middleDot() + "  " + juce::String (parked) + " past the end" : juce::String() };
    }

    //==============================================================================
    // Painting
    void paint (juce::Graphics& g) override
    {
        paintCard (g);
        paintToolbarText (g);
        paintStrip (g);
        paintGrid (g);
    }

    void paintCard (juce::Graphics& g) const
    {
        const auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setGradientFill (juce::ColourGradient::vertical (Col::panel.brighter (0.035f), r.getY(), Col::panel, r.getBottom()));
        g.fillRoundedRectangle (r, cb::ui::kCornerRadius);
        g.setColour (Col::outline.withAlpha (0.55f));
        g.drawRoundedRectangle (r, cb::ui::kCornerRadius, 1.0f);
    }

    void paintToolbarText (juce::Graphics& g) const
    {
        if (showTitle)
        {
            g.setFont (titleFont());
            g.setColour (Col::textDim);
            g.drawText ("MELODY", titleArea, juce::Justification::centredLeft, false);
        }

        if (showReadout)
        {
            const auto readout = readoutText();
            const float mainW = widthOf (readoutFont(), readout.first) + 2.0f;
            g.setFont (readoutFont());
            g.setColour (working.empty() ? Col::textDim : Col::text);
            g.drawText (readout.first, readoutArea.withWidth (mainW), juce::Justification::centredLeft, false);

            if (readout.second.isNotEmpty())
            {
                g.setFont (detailFont());
                g.setColour (Col::textFaint);
                g.drawText (readout.second, readoutArea.withTrimmedLeft (mainW + 8.0f), juce::Justification::centredLeft, false);
            }
        }

        g.setFont (labelFont());
        g.setColour (Col::textDim);

        if (! snapLabelArea.isEmpty())
            g.drawText ("Snap", snapLabelArea, juce::Justification::centredLeft, false);

        if (! gridLabelArea.isEmpty())
            g.drawText ("Grid", gridLabelArea, juce::Justification::centredLeft, false);
    }

    // Chord names above the beats they cover, in their function colours.
    void paintStrip (juce::Graphics& g) const
    {
        const auto area = stripArea;

        if (progression.empty())
        {
            g.setFont (cb::ui::font (11.0f, 400));
            g.setColour (Col::textFaint);
            g.drawText ("No chords yet: the rows light up with each chord's tones once you add some below",
                        area.withTrimmedLeft (2.0f), juce::Justification::centredLeft, true);
            return;
        }

        const auto nameFont = cb::ui::font (std::min (11.5f, area.getHeight() * 0.78f), 600);
        double beat = 0.0;

        for (size_t i = 0; i < progression.slots.size(); ++i)
        {
            const double end = beat + progression.slots[i].lengthBeats;
            const float x0 = std::round (beatToX (beat)) + 1.0f;
            const float x1 = std::round (beatToX (end)) - 1.0f;
            beat = end;

            if (x1 - x0 < 3.0f)
                continue;

            const auto colour = slotColours[i];
            const auto cell = juce::Rectangle<float> (x0, area.getY(), x1 - x0, area.getHeight());
            g.setColour (colour.withAlpha (0.8f));
            g.fillRoundedRectangle (cell.withTop (cell.getBottom() - 2.5f), 1.25f);

            g.setFont (nameFont);
            g.setColour (colour.brighter (0.3f));
            g.drawText (slotNames[(int) i], cell.withTrimmedBottom (3.0f).withTrimmedLeft (3.0f), juce::Justification::centredLeft,
                        true);
        }

        if (playing)
        {
            const float x = beatToX (playBeat);
            juce::Path tri;
            tri.addTriangle (x - 4.5f, area.getY(), x + 4.5f, area.getY(), x, area.getY() + 5.5f);
            g.setColour (Col::accent);
            g.fillPath (tri);
        }
    }

    juce::Colour noteColour (const cb::MelodyNote& n) const
    {
        const int slot = progression.slotAtBeat (n.startBeat);
        const cb::Chord* chord = slot >= 0 ? &progression.slots[(size_t) slot].chord : nullptr;

        switch (cb::toneRole (cb::pc (n.note), key, chord))
        {
            case cb::ToneRole::ChordRoot:
            case cb::ToneRole::ChordTone: return slotColours[(size_t) slot];
            case cb::ToneRole::Scale:     return juce::Colour (0xffd9e2ea);
            case cb::ToneRole::Outside:   break;
        }

        return Col::chromatic.darker (0.3f);
    }

    bool melodyOn() const
    {
        if (auto* v = processor.getAPVTS().getRawParameterValue (cb::ParamIDs::melodyOn))
            return v->load() >= 0.5f;

        return true;
    }

    void paintGrid (juce::Graphics& g)
    {
        if (gridArea.isEmpty())
            return;

        {
            juce::Graphics::ScopedSaveState save (g);
            paintGridContents (g);
        }

        g.setColour (Col::outline.withAlpha (0.9f));
        g.drawRoundedRectangle (gridArea.reduced (0.5f), 5.0f, 1.0f);
    }

    // Everything inside the grid's rounded rectangle (the caller saves / restores the clip).
    void paintGridContents (juce::Graphics& g)
    {
        const auto area = gridArea;
        juce::Path clip;
        clip.addRoundedRectangle (area, 5.0f);
        g.reduceClipRegion (clip);

        const int hi = topNote;
        const int lo = std::max (cb::kMelodyLowestNote, topNote - (int) std::ceil (area.getHeight() / rowH));

        // Rows: scale tones lighter (the tonic a touch more), other notes darker.
        for (int n = hi; n >= lo; --n)
        {
            const int p = cb::pc (n);
            const bool inScale = key.contains (p);
            g.setColour (inScale ? Col::background.brighter (p == key.tonic ? 0.09f : 0.06f) : Col::background.darker (0.04f));
            g.fillRect (juce::Rectangle<float> (area.getX(), rowTop (n), area.getWidth(), rowH));
        }

        // Each chord's tones over its beats, in its function colour (the root more strongly).
        {
            double beat = 0.0;

            for (size_t i = 0; i < progression.slots.size(); ++i)
            {
                const auto& slot = progression.slots[i];
                const double end = beat + slot.lengthBeats;
                const float x0 = beatToX (beat), x1 = beatToX (end);
                beat = end;

                for (int n = hi; n >= lo; --n)
                {
                    const auto role = cb::toneRole (cb::pc (n), key, &slot.chord);

                    if (role == cb::ToneRole::ChordRoot || role == cb::ToneRole::ChordTone)
                    {
                        g.setColour (slotColours[i].withAlpha (role == cb::ToneRole::ChordRoot ? 0.24f : 0.13f));
                        g.fillRect (juce::Rectangle<float> (x0, rowTop (n), x1 - x0, rowH));
                    }
                }
            }
        }

        // Row separators; the line between B and C is stronger (octaves)
        for (int n = hi; n >= lo; --n)
        {
            const bool octave = cb::pc (n) == 0;
            g.setColour (Col::outline.withAlpha (octave ? 0.7f : 0.2f));
            g.fillRect (juce::Rectangle<float> (area.getX(), std::round (rowTop (n) + rowH) - (octave ? 0.5f : 0.0f), area.getWidth(),
                                                octave ? 1.0f : 0.6f));
        }

        // Vertical lines: grid steps, beats, bars
        const double step = gridStep();

        if (step * pxPerBeat >= 7.0)
        {
            g.setColour (Col::outline.withAlpha (0.16f));

            for (int k = 1; (double) k * step < (double) visibleBeats; ++k)
            {
                const double b = (double) k * step;

                if (std::abs (b - std::round (b)) > 1.0e-6)
                    g.fillRect (juce::Rectangle<float> (std::round (beatToX (b)), area.getY(), 1.0f, area.getHeight()));
            }
        }

        for (int b = 1; b < visibleBeats; ++b)
        {
            const bool bar = b % 4 == 0;
            g.setColour (bar ? Col::textFaint.withAlpha (0.42f) : Col::outline.withAlpha (0.45f));
            g.fillRect (juce::Rectangle<float> (std::round (beatToX (b)), area.getY(), 1.0f, area.getHeight()));
        }

        // Past the end of the progression: dimmed and hatched (notes there are kept but do not play)
        if (total > 0 && total < visibleBeats)
        {
            const float x = std::round (beatToX (total));
            const auto past = area.withLeft (x);
            g.setColour (juce::Colours::black.withAlpha (0.3f));
            g.fillRect (past);
            g.setColour (Col::outline.withAlpha (0.22f));

            for (float d = -past.getHeight(); d < past.getWidth(); d += 9.0f)
                g.drawLine (past.getX() + d, past.getBottom(), past.getX() + d + past.getHeight(), past.getY(), 1.0f);

            g.setColour (Col::textFaint.withAlpha (0.8f));
            g.fillRect (juce::Rectangle<float> (x, area.getY(), 1.0f, area.getHeight()));
        }

        // C labels (and the hovered row's name) at the left edge
        const auto labelFont = cb::ui::font (juce::jlimit (8.5f, 10.5f, rowH * 0.8f), 600);
        g.setFont (labelFont);

        for (int n = hi; n >= lo; --n)
        {
            if (cb::pc (n) != 0 || n == hoverRow || rowH < 8.5f)
                continue;

            g.setColour (Col::textFaint.withAlpha (0.95f));
            g.drawText ("C" + juce::String (n / 12), juce::Rectangle<float> (area.getX() + 4.0f, rowTop (n), 40.0f, rowH),
                        juce::Justification::centredLeft, false);
        }

        if (hoverRow >= lo && hoverRow <= hi && gesture == Gesture::none)
        {
            g.setColour (juce::Colours::white.withAlpha (0.035f));
            g.fillRect (juce::Rectangle<float> (area.getX(), rowTop (hoverRow), area.getWidth(), rowH));
        }

        paintGhosts (g);
        paintDrawPreview (g);
        paintNotes (g, lo, hi);

        if (hoverRow >= lo && hoverRow <= hi)
        {
            const auto name = noteName (hoverRow, key);
            const float w = widthOf (labelFont, name) + 10.0f;
            const auto pill = juce::Rectangle<float> (area.getX() + 2.0f, rowTop (hoverRow) + 0.5f, w, rowH - 1.0f);
            g.setColour (Col::panelHover.withAlpha (0.95f));
            g.fillRoundedRectangle (pill, 3.0f);
            g.setColour (Col::text);
            g.drawText (name, pill, juce::Justification::centred, false);
        }

        if (playing)
        {
            const float x = beatToX (playBeat);
            g.setColour (Col::accent.withAlpha (0.9f));
            g.fillRect (juce::Rectangle<float> (x - 0.75f, area.getY(), 1.5f, area.getHeight()));
        }

        paintEmptyHint (g);
    }

    // Calls fn (note) for every ghost note inside the visible rows / beats: each other instance's melody, repeated
    // every loop of that instance's progression (they all follow the host's position).
    template <typename Fn>
    void forEachVisibleGhost (Fn&& fn) const
    {
        if (! showGhosts)
            return;

        const int hi = topNote;
        const int lo = std::max (cb::kMelodyLowestNote, topNote - (int) std::ceil (gridArea.getHeight() / rowH));

        for (const auto& ghost : ghosts)
        {
            const double loop = ghost.loopBeats;
            const int repeats = loop > 0.0 ? (int) std::ceil ((double) visibleBeats / loop) : 1;

            for (int k = 0; k < repeats && k < 64; ++k)
            {
                for (const auto& n : ghost.melody.notes)
                {
                    if (n.note < lo || n.note > hi || (loop > 0.0 && n.startBeat >= loop))
                        continue;

                    auto shifted = n;
                    shifted.startBeat += (double) k * loop;

                    if (shifted.startBeat < (double) visibleBeats)
                        fn (shifted);
                }
            }
        }
    }

    int countVisibleGhosts() const
    {
        int n = 0;
        forEachVisibleGhost ([&n] (const cb::MelodyNote&) { ++n; });
        return n;
    }

    void paintGhosts (juce::Graphics& g) const
    {
        forEachVisibleGhost ([&] (const cb::MelodyNote& n)
        {
            const auto r = noteRect (n);
            g.setColour (Col::textDim.withAlpha (0.08f));
            g.fillRoundedRectangle (r, 2.5f);
            g.setColour (Col::textDim.withAlpha (0.55f));
            g.drawRoundedRectangle (r.reduced (0.5f), 2.5f, 1.0f);
        });
    }

    // Where a click would put a new note (hovering empty space).
    void paintDrawPreview (juce::Graphics& g) const
    {
        if (gesture != Gesture::none || ! hoverDraw.has_value())
            return;

        const auto r = noteRect (*hoverDraw);
        g.setColour (juce::Colours::white.withAlpha (0.07f));
        g.fillRoundedRectangle (r, 2.5f);
        g.setColour (Col::accent.withAlpha (0.55f));
        g.drawRoundedRectangle (r.reduced (0.5f), 2.5f, 1.0f);
    }

    void paintNotes (juce::Graphics& g, int lo, int hi) const
    {
        const bool on = melodyOn();
        const auto labelFont = cb::ui::font (juce::jlimit (8.5f, 10.0f, rowH * 0.72f), 600);

        for (int i = 0; i < (int) working.notes.size(); ++i)
        {
            const auto& n = working.notes[(size_t) i];

            if (n.note < lo || n.note > hi)
                continue;

            const auto r = noteRect (n);

            if (r.getRight() < gridArea.getX() || r.getX() > gridArea.getRight())
                continue;

            const bool parked = total <= 0 || n.startBeat >= (double) total;
            const bool active = (gesture != Gesture::none && gesture != Gesture::erase && i == gestureIndex)
                             || (gesture == Gesture::none && i == hoverIndex);
            const double end = std::min (n.startBeat + n.lengthBeats, (double) total);
            const bool sounding = playing && on && ! parked && playBeat >= n.startBeat && playBeat < end;
            const auto colour = noteColour (n);
            const float alpha = (0.55f + 0.45f * n.velocity) * (parked ? 0.45f : 1.0f);

            if (sounding)
            {
                g.setColour (Col::accent.withAlpha (0.35f));
                g.fillRoundedRectangle (r.expanded (2.0f), 4.0f);
            }

            g.setColour (juce::Colours::black.withAlpha (0.28f * alpha));
            g.fillRoundedRectangle (r.translated (0.0f, 1.0f), 2.5f);
            g.setGradientFill (juce::ColourGradient::vertical (colour.brighter (active ? 0.5f : 0.32f).withMultipliedAlpha (alpha), r.getY(),
                                                               colour.brighter (active ? 0.15f : 0.02f).withMultipliedAlpha (alpha),
                                                               r.getBottom()));
            g.fillRoundedRectangle (r, 2.5f);
            g.setColour (colour.darker (0.7f).withAlpha (0.85f * alpha));
            g.drawRoundedRectangle (r.reduced (0.5f), 2.5f, 1.0f);

            if (active || sounding)
            {
                g.setColour (juce::Colours::white.withAlpha (sounding ? 0.9f : 0.7f));
                g.drawRoundedRectangle (r, 2.5f, 1.2f);
            }

            if (i == hoverIndex && hoverEdge && gesture == Gesture::none)
            {
                g.setColour (juce::Colours::white.withAlpha (0.85f));
                g.fillRoundedRectangle (r.getRight() - 3.5f, r.getY() + 2.0f, 2.0f, r.getHeight() - 4.0f, 1.0f);
            }

            if (r.getWidth() >= 26.0f && rowH >= 11.0f)
            {
                g.setFont (labelFont);
                g.setColour ((colour.getPerceivedBrightness() > 0.55f ? Col::background : Col::text).withAlpha (0.85f * alpha + 0.1f));
                g.drawText (noteName (n.note, key), r.withTrimmedLeft (4.0f).withTrimmedRight (4.0f), juce::Justification::centredLeft,
                            true);
            }
        }
    }

    void paintEmptyHint (juce::Graphics& g) const
    {
        if (! working.empty() || gesture != Gesture::none)
            return;

        const juce::String line1 = progression.empty() ? "Add chords below, then draw a melody over them"
                                                       : "Click to draw a melody";
        const juce::String line2 = "drag to move  " + middleDot() + "  drag the right edge to resize  " + middleDot()
                                 + "  right-click to delete  " + middleDot() + "  Snap keeps notes on the harmony";
        const auto f1 = cb::ui::font (13.0f, 600), f2 = cb::ui::font (11.5f, 400);
        const float w = std::min (gridArea.getWidth() - 40.0f, std::max (widthOf (f1, line1), widthOf (f2, line2)) + 32.0f);
        const auto box = juce::Rectangle<float> (w, 48.0f).withCentre (gridArea.getCentre());

        g.setColour (Col::panel.withAlpha (0.88f));
        g.fillRoundedRectangle (box, 8.0f);
        g.setColour (Col::outline.withAlpha (0.8f));
        g.drawRoundedRectangle (box.reduced (0.5f), 8.0f, 1.0f);

        g.setFont (f1);
        g.setColour (Col::text);
        g.drawText (line1, box.withTrimmedBottom (22.0f).withTrimmedTop (4.0f), juce::Justification::centred, true);
        g.setFont (f2);
        g.setColour (Col::textDim);
        g.drawFittedText (line2, box.withTrimmedTop (24.0f).withTrimmedBottom (6.0f).reduced (8.0f, 0.0f).toNearestInt(),
                          juce::Justification::centred, 1, 0.8f);
    }

    //==============================================================================
    // Editing
    enum class Gesture { none, move, resize, erase };

    struct Hit
    {
        int index = -1;
        bool edge = false;
    };

    Hit hitTest (juce::Point<float> pos) const
    {
        for (int i = (int) working.notes.size(); --i >= 0;)   // drawn last = on top
        {
            const auto r = noteRect (working.notes[(size_t) i]);

            if (r.expanded (0.0f, 0.5f).contains (pos))
            {
                const float edgeW = juce::jlimit (3.0f, 7.0f, r.getWidth() * 0.3f);
                return { i, pos.x >= r.getRight() - edgeW };
            }
        }

        return {};
    }

    std::optional<cb::Chord> chordAt (double beat) const { return cb::chordAtBeat (progression, beat); }

    double gridBeat (double beat, bool fine, bool roundToNearest) const
    {
        return cb::snapBeatToGrid (beat, fine ? cb::kMelodyTick : gridStep(), roundToNearest);
    }

    // The note a click at `pos` would draw.
    std::optional<cb::MelodyNote> noteToDraw (juce::Point<float> pos, bool fine) const
    {
        if (! gridArea.contains (pos))
            return std::nullopt;

        const double beat = gridBeat (xToBeat (pos.x), fine, false);

        if (beat < 0.0 || beat >= (double) visibleBeats)
            return std::nullopt;

        cb::MelodyNote n;
        n.startBeat = beat;
        n.lengthBeats = std::max (lastLength, cb::kMinMelodyNoteBeats);
        n.note = cb::snapMelodyPitch (yToNote (pos.y), snap, key, chordAt (beat));
        n.velocity = cb::kDefaultMelodyVelocity;
        return n;
    }

    void audition (int note)
    {
        if (note == auditionedNote && auditioning)
            return;

        processor.startPreviewMelodyNote (note);
        auditionedNote = note;
        auditioning = true;
    }

    void stopAudition()
    {
        if (auditioning)
            processor.stopPreview();

        auditioning = false;
        auditionedNote = -1;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (gesture != Gesture::none)
        {
            // The previous gesture never got its mouse-up (e.g. a window grabbed the mouse): drop its edits.
            gesture = Gesture::none;
            gestureIndex = -1;
            stopAudition();
        }

        if (! gridArea.contains (e.position))
            return;

        working = processor.getMelody();
        downPos = e.position;
        dragged = false;
        hoverDraw.reset();

        if (e.mods.isPopupMenu())
        {
            gesture = Gesture::erase;
            erasedAny = false;
            pressedOnNote = hitTest (e.position).index >= 0;
            lastErasePos = e.position;
            eraseAt (e.position);
            return;
        }

        const auto hit = hitTest (e.position);

        if (hit.index >= 0)
        {
            gesture = hit.edge ? Gesture::resize : Gesture::move;
            gestureIndex = hit.index;
            original = working.notes[(size_t) hit.index];

            if (gesture == Gesture::move)
                audition (original.note);
        }
        else if (auto n = noteToDraw (e.position, e.mods.isAltDown()))
        {
            working.notes.push_back (*n);
            gesture = Gesture::move;   // keep dragging to place it
            gestureIndex = (int) working.notes.size() - 1;
            original = *n;
            audition (n->note);
        }

        setMouseCursor (gesture == Gesture::resize ? juce::MouseCursor::LeftRightResizeCursor
                                                   : juce::MouseCursor::DraggingHandCursor);
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (gesture == Gesture::none)
            return;

        dragged = dragged || e.getDistanceFromDragStart() > 3;
        const bool fine = e.mods.isAltDown();

        if (gesture == Gesture::erase)
        {
            // Sample the path so a fast sweep does not skip short notes.
            const auto from = lastErasePos, to = e.position;
            const int steps = std::max (1, (int) std::ceil (from.getDistanceFrom (to) / 3.0f));

            for (int s = 1; s <= steps; ++s)
                eraseAt (from + (to - from) * ((float) s / (float) steps));

            lastErasePos = to;
            return;
        }

        if (gestureIndex < 0 || gestureIndex >= (int) working.notes.size())
            return;

        auto& n = working.notes[(size_t) gestureIndex];

        if (gesture == Gesture::move)
        {
            const double delta = xToBeat (e.position.x) - xToBeat (downPos.x);
            const double latest = (double) visibleBeats - std::max (gridStep(), cb::kMinMelodyNoteBeats);
            const double start = juce::jlimit (0.0, std::max (0.0, latest), gridBeat (original.startBeat + delta, fine, true));
            const int rows = yToNote (e.position.y) - yToNote (downPos.y);
            const int pitch = cb::snapMelodyPitch (original.note + rows, snap, key, chordAt (start));

            if (start != n.startBeat || pitch != n.note)
            {
                n.startBeat = start;
                n.note = pitch;
                audition (pitch);
                updateToolbarReadout();
                repaint();
            }
        }
        else if (gesture == Gesture::resize)
        {
            const double minLength = fine ? cb::kMinMelodyNoteBeats : std::max (gridStep(), cb::kMinMelodyNoteBeats);
            const double end = gridBeat (xToBeat (e.position.x), fine, true);
            const double length = juce::jlimit (minLength, cb::kMaxMelodyNoteBeats, end - n.startBeat);

            if (length != n.lengthBeats)
            {
                n.lengthBeats = length;
                repaint();
            }
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (gesture == Gesture::none)
            return;

        const auto finished = gesture;
        gesture = Gesture::none;
        stopAudition();

        if (finished == Gesture::erase && ! erasedAny && ! pressedOnNote && ! dragged)
        {
            working = processor.getMelody();
            showMenu();
        }
        else
        {
            if (finished == Gesture::resize && gestureIndex >= 0 && gestureIndex < (int) working.notes.size())
                lastLength = working.notes[(size_t) gestureIndex].lengthBeats;

            processor.setMelody (working);   // one undo step for the whole gesture (none when nothing changed)
            working = processor.getMelody();
        }

        gestureIndex = -1;
        updateHover (e.position, e.mods.isAltDown());
        updateToolbarReadout();
        repaint();
    }

    void eraseAt (juce::Point<float> pos)
    {
        const auto hit = hitTest (pos);

        if (hit.index < 0)
            return;

        working.notes.erase (working.notes.begin() + hit.index);
        erasedAny = true;
        updateToolbarReadout();
        repaint();
    }

    void mouseMove (const juce::MouseEvent& e) override { updateHover (e.position, e.mods.isAltDown()); }

    void mouseExit (const juce::MouseEvent&) override
    {
        hoverIndex = -1;
        hoverRow = -1;
        hoverEdge = false;
        hoverDraw.reset();
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint();
    }

    void updateHover (juce::Point<float> pos, bool fine)
    {
        const bool inGrid = gridArea.contains (pos);
        const auto hit = inGrid ? hitTest (pos) : Hit {};
        const int row = inGrid ? yToNote (pos.y) : -1;
        std::optional<cb::MelodyNote> draw;

        if (inGrid && hit.index < 0)
            draw = noteToDraw (pos, fine);

        const bool drawChanged = draw.has_value() != hoverDraw.has_value() || (draw.has_value() && ! (*draw == *hoverDraw));

        if (hit.index != hoverIndex || hit.edge != hoverEdge || row != hoverRow || drawChanged)
        {
            hoverIndex = hit.index;
            hoverEdge = hit.edge;
            hoverRow = row;
            hoverDraw = draw;
            setMouseCursor (hit.index < 0 ? juce::MouseCursor::NormalCursor
                                          : (hit.edge ? juce::MouseCursor::LeftRightResizeCursor
                                                      : juce::MouseCursor::DraggingHandCursor));
            repaint (gridArea.expanded (2.0f).getSmallestIntegerContainer());
        }
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (! gridArea.contains (e.position) && ! stripArea.contains (e.position))
            return;

        // Smooth (touchpad) deltas accumulate; a classic wheel notch (0.234) scrolls about two rows.
        wheelAccumulator += wheel.deltaY * (wheel.isReversed ? -1.0f : 1.0f) * 9.0f;
        const int rows = (int) wheelAccumulator;

        if (rows != 0)
        {
            wheelAccumulator -= (float) rows;
            scrollTo (topNote + rows);
            updateHover (e.position, e.mods.isAltDown());
        }
    }

    void showMenu()
    {
        juce::PopupMenu snapMenu, gridMenu;

        for (int i = 0; i < cb::numMelodySnapModes; ++i)
            snapMenu.addItem (100 + i, cb::melodySnapName ((cb::MelodySnap) i), true, i == (int) snap);

        for (int i = 0; i < cb::numRateDivisions; ++i)
            gridMenu.addItem (200 + i, cb::rateDivisionName ((cb::RateDivision) i), true, i == (int) grid);

        const bool hasNotes = ! processor.getMelody().empty();
        juce::PopupMenu menu;
        menu.setLookAndFeel (&getLookAndFeel());
        menu.addSectionHeader ("Melody");
        menu.addSubMenu ("Snap", snapMenu);
        menu.addSubMenu ("Grid", gridMenu);
        menu.addItem (1, "Play melody", true, melodyOn());
        menu.addSeparator();
        menu.addItem (2, "Octave up", hasNotes);
        menu.addItem (3, "Octave down", hasNotes);
        menu.addItem (4, "Clear melody", hasNotes);

        juce::Component::SafePointer<Impl> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea ({ juce::Desktop::getMousePosition(), { 1, 1 } }),
                            [safe] (int result)
                            {
                                if (safe != nullptr)
                                    safe->menuChosen (result);
                            });
    }

    void menuChosen (int result)
    {
        if (result >= 100 && result < 100 + cb::numMelodySnapModes)
            setSnap ((cb::MelodySnap) (result - 100));
        else if (result >= 200 && result < 200 + cb::numRateDivisions)
            setGrid ((cb::RateDivision) (result - 200));
        else if (result == 1)
        {
            if (auto* param = processor.getAPVTS().getParameter (cb::ParamIDs::melodyOn))
            {
                param->beginChangeGesture();
                param->setValueNotifyingHost (melodyOn() ? 0.0f : 1.0f);
                param->endChangeGesture();
            }
        }
        else if (result == 2 || result == 3)
        {
            auto m = processor.getMelody();
            const int shift = result == 2 ? 12 : -12;
            bool fits = true;

            for (auto& n : m.notes)
            {
                n.note += shift;
                fits = fits && n.note >= cb::kMelodyLowestNote && n.note <= cb::kMelodyHighestNote;
            }

            if (fits)
            {
                processor.setMelody (m);
                scrollTo (topNote + shift);
            }
        }
        else if (result == 4)
        {
            processor.clearMelody();
        }
    }

    void updateToolbarReadout()
    {
        clearButton.setEnabled (! working.empty());
        layoutToolbar();
    }

    //==============================================================================
    void timerCallback() override
    {
        if (! isVisible())
            return;

        const bool playingNow = processor.getPlayingSlot() >= 0;
        const double beat = playingNow ? processor.getPlaybackBeat() : 0.0;

        if (playingNow != playing || (playingNow && std::abs (beat - playBeat) > 1.0e-4))
        {
            playing = playingNow;
            playBeat = beat;
            repaint (stripArea.getUnion (gridArea).expanded (4.0f).getSmallestIntegerContainer());
        }

        if (const auto serial = processor.getGhostSerial(); serial != ghostSerial)
        {
            ghostSerial = serial;
            const bool hadGhosts = ! ghosts.empty();
            ghosts = processor.getGhostMelodies();

            if (hadGhosts != ! ghosts.empty())
                layoutToolbar();

            updateGhostTooltip();
            repaint();
        }
    }

    void updateGhostTooltip()
    {
        juce::StringArray names;

        for (const auto& gm : ghosts)
            names.add ("Chordbloom " + juce::String (gm.instanceNumber));

        ghostButton.setTooltip ("Ghost notes: show the melodies of the other Chordbloom instances in this project ("
                                + names.joinIntoString (", ") + ") as faint outlines");
    }

    //==============================================================================
    ChordbloomProcessor& processor;

    Segments snapSegments, gridSegments;
    MelodyOnButton playToggle { "Play" };
    ToolButton clearButton, ghostButton;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> playAttachment;

    // Settings
    cb::MelodySnap snap = cb::MelodySnap::Scale;
    cb::RateDivision grid = cb::RateDivision::Eighth;
    bool showGhosts = true;
    int rightInset = 0;

    // Data
    cb::Progression progression;
    cb::Key key;
    int total = 0;
    cb::Melody working;   // the processor's melody, or the gesture's edited copy while dragging
    juce::StringArray slotNames;
    std::vector<juce::Colour> slotColours;
    std::vector<ChordbloomProcessor::GhostMelody> ghosts;
    uint64_t ghostSerial = 0;

    // Geometry
    juce::Rectangle<float> toolbarArea, stripArea, gridArea, titleArea, readoutArea, snapLabelArea, gridLabelArea;
    bool compact = false, showTitle = true, showReadout = true, showLabels = true;
    int visibleBeats = kMinVisibleBeats;
    float pxPerBeat = 1.0f, rowH = 10.0f;
    int topNote = -1, requestedTop = -1;
    float wheelAccumulator = 0.0f;

    // Playback display
    bool playing = false;
    double playBeat = 0.0;

    // Interaction
    Gesture gesture = Gesture::none;
    int gestureIndex = -1;
    cb::MelodyNote original;
    juce::Point<float> downPos, lastErasePos;
    bool dragged = false, erasedAny = false, pressedOnNote = false;
    double lastLength = 1.0;
    int hoverIndex = -1, hoverRow = -1;
    bool hoverEdge = false;
    std::optional<cb::MelodyNote> hoverDraw;
    bool auditioning = false;
    int auditionedNote = -1;
};

//==============================================================================
MelodyRoll::MelodyRoll (ChordbloomProcessor& p) : impl (std::make_unique<Impl> (p))
{
    setOpaque (false);
    noFocus (*this);
    setTitle ("Melody");
    addAndMakeVisible (*impl);
}

MelodyRoll::~MelodyRoll() = default;

void MelodyRoll::paint (juce::Graphics&) {}   // the Impl surface paints the whole roll

void MelodyRoll::resized() { impl->setBounds (getLocalBounds()); }

void MelodyRoll::refresh()
{
    impl->sync();
}

void MelodyRoll::setToolbarRightInset (int pixels)
{
    if (impl->rightInset != pixels)
    {
        impl->rightInset = std::max (0, pixels);
        impl->layoutToolbar();
    }
}

cb::MelodySnap MelodyRoll::getSnap() const noexcept { return impl->snap; }
void MelodyRoll::setSnap (cb::MelodySnap s) { impl->setSnap (s); }
cb::RateDivision MelodyRoll::getGrid() const noexcept { return impl->grid; }
void MelodyRoll::setGrid (cb::RateDivision r) { impl->setGrid (r); }

int MelodyRoll::getTopNote() const noexcept { return impl->topNote; }
int MelodyRoll::getBottomNote() const noexcept { return impl->bottomNote(); }
void MelodyRoll::setTopNote (int midiNote) { impl->scrollTo (midiNote); }

juce::Rectangle<float> MelodyRoll::getGridArea() const noexcept { return impl->gridArea; }

juce::Point<float> MelodyRoll::getCellCentre (double beat, int midiNote) const
{
    return { impl->beatToX (beat), impl->rowTop (midiNote) + impl->rowH * 0.5f };
}

juce::Rectangle<float> MelodyRoll::getNoteBounds (int noteIndex) const
{
    const auto& notes = impl->processor.getMelody().notes;

    if (noteIndex < 0 || noteIndex >= (int) notes.size())
        return {};

    return impl->noteRect (notes[(size_t) noteIndex]);
}

int MelodyRoll::getNumGhostNotesShown() const noexcept { return impl->countVisibleGhosts(); }
