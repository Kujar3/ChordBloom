#include "ui/ExportPanel.h"

#include "ui/Theme.h"

#include <cmath>

namespace
{
namespace Col = cb::ui::Colours;
using Part = ChordbloomProcessor::ExportPart;

juce::String toJuce (const std::string& s) { return juce::String::fromUTF8 (s.data(), (int) s.size()); }

float widthOf (const juce::Font& f, const juce::String& s) { return juce::GlyphArrangement::getStringWidth (f, s); }

juce::File& lastSaveDirectory()   // message thread only; remembered while the plugin stays loaded
{
    static juce::File dir;
    return dir;
}

const char* partFileLabel (Part part)
{
    switch (part)
    {
        case Part::Chords: return "Chords";
        case Part::Bass:   return "Bass";
        case Part::Both:   break;
    }
    return "Chords + Bass";
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
} // namespace

//==============================================================================
struct ExportPanel::Impl : public juce::Component,
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
            setTooltip (enabledTip);
            setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        }

        void paint (juce::Graphics& g) override
        {
            const auto r = getLocalBounds().toFloat().reduced (1.0f);
            const float radius = r.getHeight() * 0.5f;
            const bool enabled = isEnabled();
            const bool over = enabled && isMouseOver (true);
            const bool active = enabled && (pressed || dragging);

            if (active)
            {
                g.setColour (colour.withAlpha (0.18f));
                g.drawRoundedRectangle (r.expanded (0.5f), radius + 0.5f, 2.0f);
            }

            const auto base = over || active ? Col::panelHover : Col::panelRaised;
            g.setGradientFill (juce::ColourGradient::vertical (base.brighter (0.05f), r.getY(), base.darker (0.08f), r.getBottom()));
            g.fillRoundedRectangle (r, radius);

            g.setColour (colour.withAlpha (active ? 0.26f : (over ? 0.15f : 0.05f)));
            g.fillRoundedRectangle (r, radius);

            g.setColour (active ? colour.withAlpha (0.9f) : (over ? colour.withAlpha (0.6f) : Col::outline));
            g.drawRoundedRectangle (r.reduced (0.5f), radius - 0.5f, 1.0f);

            const auto font = cb::ui::font (13.0f, 600);
            const float textW = widthOf (font, label);
            const float gripW = 8.0f;
            const float contentW = gripW + 8.0f + textW;
            const float x = std::round (r.getCentreX() - contentW * 0.5f);

            drawGrip (g, { x + 3.0f, r.getCentreY() }, enabled ? colour : Col::textFaint);

            g.setFont (font);
            g.setColour (enabled ? Col::text : Col::textFaint);
            g.drawText (label, juce::Rectangle<float> (x + gripW + 8.0f, r.getY(), textW + 2.0f, r.getHeight()),
                        juce::Justification::centredLeft, false);
        }

        void enablementChanged() override
        {
            setTooltip (isEnabled() ? enabledTip : juce::String ("Add some chords to the progression first"));
            setMouseCursor (isEnabled() ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
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
                owner.showStatus ("Drag me onto FL Studio's playlist or piano roll", Col::textDim);

            pressed = false;
            repaint();
        }

        Impl& owner;
        const Part part;
        const juce::String label;
        const juce::Colour colour;
        const juce::String enabledTip;
        bool pressed = false, dragStarted = false, dragging = false;
    };

    //==============================================================================
    class SaveButton : public juce::Button
    {
    public:
        SaveButton() : juce::Button ("Save .mid")
        {
            setTooltip ("Save the progression as a .mid file");
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
                "Drag chords + bass as one MIDI file (chords on channel 1, bass on channel 2)")
    {
        setOpaque (false);

        for (auto* c : { (juce::Component*) &chords, (juce::Component*) &bass, (juce::Component*) &both, (juce::Component*) &save })
            addAndMakeVisible (c);

        save.onClick = [this] { showSaveMenu(); };
        updateEnablement();
        startTimerHz (4);
    }

    ~Impl() override { stopTimer(); }

    //==============================================================================
    void updateEnablement()
    {
        const bool hasChords = ! processor.getProgression().empty();

        for (auto* c : { (juce::Component*) &chords, (juce::Component*) &bass, (juce::Component*) &both, (juce::Component*) &save })
            if (c->isEnabled() != hasChords)
                c->setEnabled (hasChords);

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

        if (captionArea.getWidth() < 10.0f)
            return;

        const bool showingStatus = status.isNotEmpty();
        auto r = captionArea;

        if (showingStatus)
        {
            g.setFont (cb::ui::font (11.5f, 600));
            g.setColour (statusColour);
            g.drawFittedText (status, r.withTrimmedLeft (twoRows ? 4.0f : 0.0f).toNearestInt(),
                              twoRows ? juce::Justification::centredLeft : juce::Justification::centred, twoRows ? 1 : 2, 0.85f);
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

    //==============================================================================
    void showStatus (const juce::String& text, juce::Colour colour)
    {
        status = text;
        statusColour = colour;
        statusUntil = juce::Time::getMillisecondCounter() + 3000;
        repaint();
    }

    void startExternalDrag (Chip& chip)
    {
        const auto file = processor.writeMidiToTempFile (chip.part);

        if (! file.existsAsFile())
        {
            showStatus ("Nothing to export yet", Col::dominant);
            return;
        }

        chip.dragging = true;
        chip.repaint();

        juce::Component::SafePointer<Chip> safeChip (&chip);
        const bool started = juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, &chip,
            [safeChip]
            {
                if (auto* c = safeChip.getComponent())
                {
                    c->dragging = false;
                    c->pressed = false;
                    c->repaint();
                }
            });

        if (! started)
        {
            chip.dragging = false;
            chip.repaint();
            showStatus ("Couldn't start the drag", Col::dominant);
        }
    }

    void showSaveMenu()
    {
        juce::PopupMenu menu;
        menu.setLookAndFeel (&getLookAndFeel());
        menu.addSectionHeader ("Save as .mid file");
        menu.addItem (1, "Chords + bass  (2 channels)");
        menu.addItem (2, "Chords only");
        menu.addItem (3, "Bass only");

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

        const auto name = juce::File::createLegalFileName ("Chordbloom - " + toJuce (processor.getKey().name()) + " - "
                                                           + partFileLabel (part) + ".mid");

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

        if (status.isNotEmpty() && juce::Time::getMillisecondCounter() > statusUntil)
        {
            status.clear();
            repaint();
        }
    }

    //==============================================================================
    ChordbloomProcessor& processor;
    Chip chords, bass, both;
    SaveButton save;
    std::unique_ptr<juce::FileChooser> chooser;

    juce::Rectangle<float> captionArea;
    bool twoRows = false;
    bool lastHasChords = false;

    juce::String status;
    juce::Colour statusColour;
    juce::uint32 statusUntil = 0;
};

//==============================================================================
ExportPanel::ExportPanel (ChordbloomProcessor& p) : impl (std::make_unique<Impl> (p))
{
    setOpaque (false);
    addAndMakeVisible (*impl);
}

ExportPanel::~ExportPanel() = default;

void ExportPanel::paint (juce::Graphics&) {}   // the Impl surface paints the panel

void ExportPanel::resized() { impl->setBounds (getLocalBounds()); }

void ExportPanel::refresh() { impl->updateEnablement(); }
