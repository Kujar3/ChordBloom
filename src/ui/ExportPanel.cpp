#include "ui/ExportPanel.h"

#include "ui/Theme.h"

#include <cmath>

namespace
{
namespace Col = cb::ui::Colours;
using Part = ChordbloomProcessor::ExportPart;

float widthOf (const juce::Font& f, const juce::String& s) { return juce::GlyphArrangement::getStringWidth (f, s); }

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

// Six-dot grip: the universal "you can drag this" sign.
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
    arrow.lineTo (cx, r.getBottom() - 4.0f);
    arrow.startNewSubPath (cx - 3.5f, r.getBottom() - 7.5f);
    arrow.lineTo (cx, r.getBottom() - 4.0f);
    arrow.lineTo (cx + 3.5f, r.getBottom() - 7.5f);
    arrow.startNewSubPath (r.getX(), r.getBottom() - 3.0f);
    arrow.lineTo (r.getX(), r.getBottom());
    arrow.lineTo (r.getRight(), r.getBottom());
    arrow.lineTo (r.getRight(), r.getBottom() - 3.0f);
    g.strokePath (arrow, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
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
} // namespace

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
        Chip (Impl& o, Part p, const juce::String& text, juce::Colour c, const juce::String& tip)
            : owner (o), part (p), label (text), colour (c), enabledTip (tip)
        {
            noFocus (*this);
            setTooltip (enabledTip);
            setMouseCursor (juce::MouseCursor::DraggingHandCursor);
            setTitle (label + " MIDI clip");
            setDescription (enabledTip);
        }

        void paint (juce::Graphics& g) override
        {
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

            const auto font = cb::ui::font (13.0f, 600);
            const auto badgeFont = cb::ui::font (9.0f, 700).withExtraKerningFactor (0.08f);
            const juce::String badge ("OFF");
            const float textW = widthOf (font, label);
            const float badgeW = muted ? widthOf (badgeFont, badge) + 8.0f : 0.0f;
            const float gripW = 8.0f;
            const float contentW = gripW + 8.0f + textW + (muted ? 6.0f + badgeW : 0.0f);
            const float x = std::round (r.getCentreX() - contentW * 0.5f);
            const bool lively = enabled && ! muted;

            drawGrip (g, { x + 3.0f, r.getCentreY() }, lively ? colour : Col::textFaint);

            g.setFont (font);
            g.setColour (lively ? Col::text : (enabled ? Col::textDim : Col::textFaint));
            g.drawText (label, juce::Rectangle<float> (x + gripW + 8.0f, r.getY(), textW + 2.0f, r.getHeight()),
                        juce::Justification::centredLeft, false);

            if (muted)
            {
                const auto b = juce::Rectangle<float> (x + gripW + 8.0f + textW + 6.0f, r.getCentreY() - 7.0f, badgeW, 14.0f);
                g.setColour (Col::textFaint.withAlpha (enabled ? 0.9f : 0.5f));
                g.drawRoundedRectangle (b.reduced (0.5f), 4.0f, 1.0f);
                g.setFont (badgeFont);
                g.drawText (badge, b, juce::Justification::centred, false);
            }
        }

        void setMuted (bool shouldBeMuted, const juce::String& mutedTip)
        {
            if (muted == shouldBeMuted)
                return;

            muted = shouldBeMuted;
            mutedTooltip = mutedTip;
            updateTooltip();
            repaint();
        }

        void updateTooltip()
        {
            setTooltip (! isEnabled() ? juce::String ("Add some chords to the progression first")
                                      : (muted ? mutedTooltip : (noteTip.isNotEmpty() ? noteTip : enabledTip)));
            setMouseCursor (isEnabled() && ! muted ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
        }

        void enablementChanged() override
        {
            updateTooltip();
            repaint();
        }

        void mouseEnter (const juce::MouseEvent&) override { repaint(); }
        void mouseExit (const juce::MouseEvent&) override  { repaint(); }

        void mouseDown (const juce::MouseEvent&) override
        {
            if (! isEnabled())
                return;

            pressed = true;
            dragStarted = false;
            repaint();
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (! isEnabled() || dragStarted || e.getDistanceFromDragStart() < 5)
                return;

            dragStarted = true;
            owner.startExternalDrag (*this);
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (isEnabled() && ! dragStarted && getLocalBounds().contains (e.getPosition()))
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
        const juce::String enabledTip;
        juce::String mutedTooltip, noteTip;
        bool pressed = false, dragStarted = false, dragging = false, muted = false;
    };

    //==============================================================================
    class SaveButton : public juce::Button
    {
    public:
        SaveButton() : juce::Button ("Save .mid")
        {
            noFocus (*this);   // Enter in the host must never re-open the save menu
            setTooltip ("Save the progression as a .mid file");
            setTitle ("Save the progression as a MIDI file");
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
        }

        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            const auto r = getLocalBounds().toFloat().reduced (1.0f);
            const float radius = r.getHeight() * 0.5f;
            const bool enabled = isEnabled();
            const float alpha = enabled ? 1.0f : 0.4f;

            const auto base = enabled && (over || down) ? Col::panelHover : Col::panelRaised;
            g.setGradientFill (juce::ColourGradient::vertical (base.brighter (0.05f).withMultipliedAlpha (alpha), r.getY(),
                                                               base.darker (0.08f).withMultipliedAlpha (alpha), r.getBottom()));
            g.fillRoundedRectangle (r, radius);
            g.setColour (Col::accent.withAlpha ((over && enabled ? 0.85f : 0.45f) * alpha));
            g.drawRoundedRectangle (r.reduced (0.5f), radius - 0.5f, 1.0f);

            const auto font = cb::ui::font (12.5f, 600);
            const juce::String caption ("Save .mid");
            const float textW = widthOf (font, caption);
            const float iconW = 10.0f;
            const float x = std::round (r.getCentreX() - (iconW + 7.0f + textW) * 0.5f);

            drawSaveIcon (g, { x, r.getCentreY() - 6.0f, iconW, 12.0f }, Col::accent.withMultipliedAlpha (alpha));

            g.setFont (font);
            g.setColour ((enabled ? Col::text : Col::textFaint));
            g.drawText (caption, juce::Rectangle<float> (x + iconW + 7.0f, r.getY(), textW + 2.0f, r.getHeight()),
                        juce::Justification::centredLeft, false);
        }
    };

    //==============================================================================
    explicit Impl (ChordbloomProcessor& p)
        : processor (p),
          chords (*this, Part::Chords, "Chords", Col::tonic,
                  "Drag the chords as a MIDI clip into FL Studio (playlist, channel rack or piano roll)"),
          bass (*this, Part::Bass, "Bass", Col::secondary, "Drag the bass line as a MIDI clip into your DAW"),
          both (*this, Part::Both, "Both", Col::accent,
                "Drag chords + bass as one MIDI file with two tracks (Chordbloom Chords, Chordbloom Bass).\n"
                "Drop it on FL Studio's playlist to get chords and bass on separate channels.")
    {
        setOpaque (false);
        noFocus (*this);
        setTitle ("Drag to DAW");
        setDescription ("Drag the progression into your DAW as MIDI, or save it as a .mid file");

        for (auto* c : { (juce::Component*) &chords, (juce::Component*) &bass, (juce::Component*) &both, (juce::Component*) &save })
            addAndMakeVisible (c);

        save.onClick = [this] { showSaveMenu(); };
        updateEnablement();
        startTimerHz (4);
    }

    ~Impl() override { stopTimer(); }

    //==============================================================================
    bool paramOn (const char* id, bool fallback) const
    {
        if (auto* v = processor.getAPVTS().getRawParameterValue (id))
            return v->load() >= 0.5f;

        return fallback;
    }

    bool bassIsOff() const
    {
        if (auto* v = processor.getAPVTS().getRawParameterValue (cb::ParamIDs::bassMode))
            return (int) std::lround (v->load()) == (int) cb::BassMode::Off;

        return false;
    }

    void updateEnablement()
    {
        const bool hasChords = ! processor.getProgression().empty();

        for (auto* c : { (juce::Component*) &chords, (juce::Component*) &bass, (juce::Component*) &both, (juce::Component*) &save })
            if (c->isEnabled() != hasChords)
                c->setEnabled (hasChords);

        const bool off = bassIsOff();

        if (off != lastBassOff)
        {
            lastBassOff = off;
            bass.setMuted (off, "Bass is Off: pick a bass mode in the Controls to export a bass line");
            both.noteTip = off ? juce::String ("Bass is Off, so this clip has the chords only (channel 1)") : juce::String();
            both.updateTooltip();
        }

        if (hasChords != lastHasChords)
        {
            lastHasChords = hasChords;
            repaint (captionArea.getSmallestIntegerContainer());
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10, 6);
        twoRows = r.getHeight() >= 40;   // caption on its own row above the chips whenever there is room

        if (twoRows)
        {
            const bool roomy = r.getHeight() >= 62;
            captionArea = r.removeFromTop (roomy ? 18 : 13).toFloat();
            r.removeFromTop (roomy ? 6 : 3);
        }

        const int chipH = std::min (r.getHeight(), 34);
        auto row = r.withSizeKeepingCentre (r.getWidth(), chipH);

        save.setBounds (row.removeFromRight (juce::jlimit (84, 104, row.getWidth() / 4)));
        row.removeFromRight (8);

        if (! twoRows)
        {
            const int captionW = row.getWidth() >= 300 ? 86 : 0;
            captionArea = row.removeFromLeft (captionW).toFloat().withY ((float) getLocalBounds().getY())
                              .withHeight ((float) getHeight());
            row.removeFromLeft (captionW > 0 ? 4 : 0);
        }

        const int gap = 6;
        const int chipW = (row.getWidth() - 2 * gap) / 3;
        chords.setBounds (row.removeFromLeft (chipW));
        row.removeFromLeft (gap);
        bass.setBounds (row.removeFromLeft (chipW));
        row.removeFromLeft (gap);
        both.setBounds (row);
    }

    void paint (juce::Graphics& g) override
    {
        paintCard (g, getLocalBounds().toFloat());
        actionArea = {};

        if (captionArea.getWidth() < 10.0f)
            return;

        const bool showingStatus = status.isNotEmpty();
        auto r = captionArea;

        if (showingStatus)
        {
            paintStatus (g, r.withTrimmedLeft (twoRows ? 4.0f : 0.0f));
            return;
        }

        const auto titleFont = cb::ui::font (10.5f, 600).withExtraKerningFactor (0.12f);
        const juce::String title ("DRAG TO DAW");
        g.setFont (titleFont);
        g.setColour (lastHasChords ? Col::textDim : Col::textFaint);

        if (twoRows)
        {
            r = r.withTrimmedLeft (4.0f);
            g.drawText (title, r, juce::Justification::centredLeft, false);

            const auto detailFont = cb::ui::font (11.0f, 400);
            const juce::String detail = lastHasChords ? "drop a chip on the playlist, channel rack or piano roll"
                                                      : "add chords to the progression first";
            const auto detailArea = r.withTrimmedLeft (widthOf (titleFont, title) + 10.0f);

            if (widthOf (detailFont, detail) <= detailArea.getWidth())
            {
                g.setFont (detailFont);
                g.setColour (Col::textFaint);
                g.drawText (detail, detailArea, juce::Justification::centredLeft, false);
            }

            return;
        }

        // Single row: title with a small arrow pointing at the chips
        const float titleW = widthOf (titleFont, title);
        const float x = r.getX() + 2.0f;
        g.drawText (title, juce::Rectangle<float> (x, r.getY(), titleW + 2.0f, r.getHeight()), juce::Justification::centredLeft, false);

        const float ax = x + titleW + 6.0f;
        const float cy = r.getCentreY();

        if (ax + 9.0f < r.getRight())
        {
            juce::Path arrow;
            arrow.startNewSubPath (ax, cy);
            arrow.lineTo (ax + 8.0f, cy);
            arrow.startNewSubPath (ax + 5.0f, cy - 3.0f);
            arrow.lineTo (ax + 8.0f, cy);
            arrow.lineTo (ax + 5.0f, cy + 3.0f);
            g.strokePath (arrow, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }

    // Status line; an actionable one ends with a clickable link (icon + label).
    void paintStatus (juce::Graphics& g, juce::Rectangle<float> r)
    {
        const auto font = cb::ui::font (11.5f, 600);

        if (statusAction == nullptr || ! twoRows)
        {
            g.setFont (font);
            g.setColour (statusColour);
            g.drawFittedText (status, r.toNearestInt(), twoRows ? juce::Justification::centredLeft : juce::Justification::centred,
                              twoRows ? 1 : 2, 0.85f);
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

        auto link = juce::Rectangle<float> (r.getX() + textW + 12.0f, r.getY() - 2.0f, linkW + 8.0f, r.getHeight() + 4.0f);
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
        showStatus ("Bass is Off: pick a bass mode in the Controls first", Col::subdominant, 3500);
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
                    [safe] { if (safe != nullptr) safe->stopPlayingWithHost(); }, "Mute", "Still playing along with FL");
    }

    void stopPlayingWithHost()
    {
        if (auto* param = processor.getAPVTS().getParameter (cb::ParamIDs::syncToHost))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (0.0f);
            param->endChangeGesture();
        }

        showStatus ("Muted: Chordbloom stays quiet while FL plays (Play with FL is off)", Col::accent, 4500);
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
        const bool off = bassIsOff();

        if (chip.part == Part::Bass && off)
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

        if (chip.part == Part::Both && off)
            showStatus ("Bass is Off, so this clip has the chords only", Col::subdominant, 3500);

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

    void showSaveMenu()
    {
        const bool off = bassIsOff();
        juce::PopupMenu menu;
        menu.setLookAndFeel (&getLookAndFeel());
        menu.addSectionHeader ("Save as .mid file");
        menu.addItem (1, off ? "Chords + bass  (Bass is Off)" : "Chords + bass  (2 tracks)", ! off);
        menu.addItem (2, "Chords only");
        menu.addItem (3, off ? "Bass only  (Bass is Off)" : "Bass only", ! off);

        juce::Component::SafePointer<Impl> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&save), [safe] (int result)
        {
            if (safe == nullptr || result == 0)
                return;

            safe->launchSaveChooser (result == 2 ? Part::Chords : (result == 3 ? Part::Bass : Part::Both));
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
                              ok ? Col::accent : Col::dominant);
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
    Chip chords, bass, both;
    SaveButton save;
    std::unique_ptr<juce::FileChooser> chooser;

    juce::Rectangle<float> captionArea, actionArea;
    bool twoRows = false;
    bool lastHasChords = false;
    bool lastBassOff = false;

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
