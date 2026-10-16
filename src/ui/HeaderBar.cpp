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
// Small icon buttons (undo, redo, preview sound on/off)
class IconButton final : public juce::Button
{
public:
    enum class Icon { undo, redo, speaker };

    IconButton (const juce::String& name, Icon iconType) : juce::Button (name), icon (iconType) {}

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        const auto b = getLocalBounds().toFloat().reduced (0.5f);
        const bool enabled = isEnabled();
        highlighted = highlighted && enabled;
        down = down && enabled;

        if (highlighted || down)
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

        if (! enabled)
            colour = Col::textFaint.withAlpha (0.55f);

        g.setColour (colour);

        if (icon == Icon::speaker)
            paintSpeaker (g, box);
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

    Icon icon;
};
} // namespace

//==============================================================================
struct HeaderBar::Impl
{
    Impl (HeaderBar& o, ChordbloomProcessor& p) : owner (o), processor (p), apvts (p.getAPVTS())
    {
        namespace id = cb::ParamIDs;

        // Key root: item texts follow the key's spelling (Db major / C# minor), refreshed on scale changes.
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

        for (auto* c : { &keyRoot, &scale })
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

        undo.onClick = [this] { processor.undo(); owner.refresh(); };
        redo.onClick = [this] { processor.redo(); owner.refresh(); };

        keyRoot.setTooltip ("Key: the root note. Every chord in the palette is built from this key.");
        scale.setTooltip ("Scale or mode: decides which chords are diatonic and which ones count as borrowed.");
        sound.setTooltip ("Preview sound on/off: the built-in instrument you hear while auditioning and playing.");
        tone.setTooltip ("Preview tone: the built-in instrument's sound.");
        volume.setTooltip ("Preview volume (double-click to reset).");
        midiOut.setTooltip ("MIDI out: send the chords (ch 1) and bass (ch 2) to the host, e.g. to route them into another instrument in FL Studio.");
        sync.setTooltip ("Sync to host: loop the progression along with the DAW's transport while it plays.");
        undo.setTooltip ("Undo the last progression edit");
        redo.setTooltip ("Redo");

        for (juce::Component* c : std::initializer_list<juce::Component*> { &keyRoot, &scale, &sound, &tone, &volume,
                                                                            &midiOut, &sync, &undo, &redo })
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

    juce::ComboBox keyRoot, scale, tone;
    IconButton sound { "Preview sound", IconButton::Icon::speaker };
    juce::Slider volume;
    juce::ToggleButton midiOut { "MIDI out" }, sync { "Sync" };
    IconButton undo { "Undo", IconButton::Icon::undo }, redo { "Redo", IconButton::Icon::redo };

    std::unique_ptr<APVTS::ComboBoxAttachment> keyRootAtt, scaleAtt, toneAtt;
    std::unique_ptr<APVTS::ButtonAttachment> soundAtt, midiOutAtt, syncAtt;
    std::unique_ptr<APVTS::SliderAttachment> volumeAtt;

    juce::Image logo96, logo48;
    juce::Rectangle<int> brandArea;
    bool showWordmark = true;
    std::vector<int> separators;
};

//==============================================================================
HeaderBar::HeaderBar (ChordbloomProcessor& p) : impl (std::make_unique<Impl> (*this, p))
{
    refresh();
}

HeaderBar::~HeaderBar() = default;

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
    const int groupGap = 14;   // each side of a separator

    // Preferred widths, squeezed below when the editor is narrow.
    const int wordmarkW = (int) std::ceil (cb::ui::textWidth (cb::ui::font (19.0f, 700), "Chordbloom"));
    int brandW = 30 + 10 + wordmarkW;
    const int rootW = 70;
    int scaleW = 160;
    int toneW = 122;
    int volumeW = 104;
    const int iconW = 30;

    d.midiOut.setSize (10, 24);
    d.midiOut.changeWidthToFitText();
    d.sync.setSize (10, 24);
    d.sync.changeWidthToFitText();

    auto required = [&]
    {
        const int left = brandW + 2 * groupGap + rootW + gap + scaleW;
        const int right = iconW + gap + toneW + gap + volumeW + 2 * groupGap + d.midiOut.getWidth() + 12
                          + d.sync.getWidth() + 2 * groupGap + iconW * 2 + 4;
        return left + right + 16;
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

    squeeze (volumeW, 64);
    squeeze (toneW, 104);
    squeeze (scaleW, 138);
    d.showWordmark = excess <= 0;

    if (! d.showWordmark)
        brandW = 30;

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

    // right: undo / redo, sync, MIDI out, sound group
    d.redo.setBounds (centred (r.removeFromRight (iconW)));
    r.removeFromRight (4);
    d.undo.setBounds (centred (r.removeFromRight (iconW)));
    r.removeFromRight (groupGap);
    d.separators.push_back (r.getRight());
    r.removeFromRight (groupGap);

    d.sync.setBounds (r.removeFromRight (d.sync.getWidth()).withSizeKeepingCentre (d.sync.getWidth(), 24));
    r.removeFromRight (12);
    d.midiOut.setBounds (r.removeFromRight (d.midiOut.getWidth()).withSizeKeepingCentre (d.midiOut.getWidth(), 24));
    r.removeFromRight (groupGap);
    d.separators.push_back (r.getRight());
    r.removeFromRight (groupGap);

    d.volume.setBounds (centred (r.removeFromRight (volumeW)));
    r.removeFromRight (gap);
    d.tone.setBounds (centred (r.removeFromRight (toneW)));
    r.removeFromRight (gap - 2);
    d.sound.setBounds (centred (r.removeFromRight (iconW)));

    if (auto* top = getTopLevelComponent(); top != nullptr && top != this)
        d.volume.setPopupDisplayEnabled (true, false, top);
}

void HeaderBar::refresh()
{
    auto& d = *impl;
    d.undo.setEnabled (d.processor.canUndo());
    d.redo.setEnabled (d.processor.canRedo());
    d.updateKeyNames();
}
