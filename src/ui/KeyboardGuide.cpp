#include "ui/KeyboardGuide.h"

#include "ui/Theme.h"

#include <algorithm>

namespace
{
namespace Col = cb::ui::Colours;

constexpr int kNumOctaves = 3;
constexpr int kNumKeys = kNumOctaves * 12;
constexpr int kNumWhiteKeys = kNumOctaves * 7;
constexpr int kDefaultLowNote = 48;   // MIDI 48 = "C4" in FL Studio naming (middle C, MIDI 60, is C5 there)

constexpr int whiteIndexInOctave[12] = { 0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6 };

bool isBlackKey (int note)
{
    return whiteIndexInOctave[cb::pc (note)] < 0;
}

// Black keys sit on the boundary after this white key index, nudged like a real keyboard.
float blackKeyCentre (int semitone)
{
    switch (semitone)
    {
        case 1:  return 1.0f - 0.10f;
        case 3:  return 2.0f + 0.10f;
        case 6:  return 4.0f - 0.12f;
        case 8:  return 5.0f;
        case 10: return 6.0f + 0.12f;
        default: break;
    }

    return 0.0f;
}

const juce::Colour whiteInScale  { 0xffdfe7ea };
const juce::Colour whiteOutScale { 0xffaab3bc };
const juce::Colour blackInScale  { 0xff3c4a52 };
const juce::Colour blackOutScale { 0xff1b2026 };
const juce::Colour scaleDotOnWhite { 0xff7a8793 };
const juce::Colour scaleDotOnBlack { 0xffa3b2bd };
const juce::Colour tonicDotOnWhite { 0xff2f9a7e };
const juce::Colour keyLabel        { 0xff5f6b77 };
const juce::Colour darkMark        { 0xff161b21 };
} // namespace

//==============================================================================
struct KeyboardGuide::Impl : private juce::Timer
{
    struct KeysView final : public juce::Component, public juce::SettableTooltipClient
    {
        explicit KeysView (Impl& owner) : d (owner) {}

        void paint (juce::Graphics& g) override { d.paintKeys (g); }
        void mouseMove (const juce::MouseEvent& e) override { d.setHover (d.noteAt (e.position)); }
        void mouseExit (const juce::MouseEvent&) override { d.setHover (-1); }
        void mouseDown (const juce::MouseEvent& e) override { d.press (d.noteAt (e.position)); }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (getLocalBounds().toFloat().contains (e.position))
                d.press (d.noteAt (e.position));
        }

        void mouseUp (const juce::MouseEvent&) override { d.release(); }

        Impl& d;
    };

    Impl (KeyboardGuide& o, ChordbloomProcessor& p) : owner (o), processor (p)
    {
        keys.setTooltip ("Harmonic guide. Dots mark the scale (green dot = tonic), coloured keys show the current chord "
                         "and glowing keys are sounding. Click a key to hear it.");
        owner.addAndMakeVisible (keys);
        key = processor.getKey();
        voicing = processor.getVoicingSettings();
        updateChordNotes();
        startTimerHz (30);
    }

    ~Impl() override
    {
        stopTimer();

        if (pressedNote >= 0)
            processor.stopPreview();
    }

    //==============================================================================
    void timerCallback() override
    {
        const auto now = processor.getSoundingNotes();

        if (now != sounding)
        {
            sounding = now;
            keys.repaint();
        }

        const auto vs = processor.getVoicingSettings();

        if (vs.style != voicing.style || vs.centerNote != voicing.centerNote || vs.voiceLeading != voicing.voiceLeading)
        {
            voicing = vs;
            updateChordNotes();
        }
    }

    void updateChordNotes()
    {
        chordNotes.clear();

        if (chord.has_value())
            chordNotes = ! explicitNotes.empty() ? explicitNotes : cb::voiceChord (*chord, voicing, -1, 0, {});

        scaleMask.fill (false);
        chordMask.fill (false);

        for (int p : key.pitchClasses())
            scaleMask[(size_t) cb::pc (p)] = true;

        if (chord.has_value())
            for (int p : chord->pitchClasses())
                chordMask[(size_t) cb::pc (p)] = true;

        chordColour = chord.has_value() ? cb::ui::functionColour (cb::harmonicFunction (*chord, key)) : Col::tonic;

        updateRange();
        owner.repaint();
    }

    void updateRange()
    {
        if (chordNotes.empty())
            return;

        const auto [loIt, hiIt] = std::minmax_element (chordNotes.begin(), chordNotes.end());
        const int lo = *loIt, hi = *hiIt;
        auto fits = [lo, hi] (int start) { return lo >= start && hi < start + kNumKeys; };

        if (fits (lowNote))
            return;

        if (fits (kDefaultLowNote))
        {
            lowNote = kDefaultLowNote;
            return;
        }

        int start = 12 * (int) std::floor (((double) (lo + hi) * 0.5) / 12.0) - 12;

        if (! fits (start))
            start = 12 * (lo / 12);

        lowNote = juce::jlimit (0, 84, start);
    }

    //==============================================================================
    float whiteWidth() const { return (float) keys.getWidth() / (float) kNumWhiteKeys; }

    juce::Rectangle<float> keyRect (int note) const
    {
        const float ww = whiteWidth();
        const float h = (float) keys.getHeight();
        const int rel = note - lowNote;
        const int octave = rel / 12;
        const int semitone = rel % 12;

        if (! isBlackKey (note))
            return { (float) (octave * 7 + whiteIndexInOctave[semitone]) * ww, 0.0f, ww, h };

        const float bw = ww * 0.6f;
        const float cx = ((float) (octave * 7) + blackKeyCentre (semitone)) * ww;
        return { cx - bw * 0.5f, 0.0f, bw, h * 0.62f };
    }

    int noteAt (juce::Point<float> p) const
    {
        for (int n = lowNote; n < lowNote + kNumKeys; ++n)
            if (isBlackKey (n) && keyRect (n).contains (p))
                return n;

        for (int n = lowNote; n < lowNote + kNumKeys; ++n)
            if (! isBlackKey (n) && keyRect (n).contains (p))
                return n;

        return -1;
    }

    void setHover (int note)
    {
        if (hoverNote != note)
        {
            hoverNote = note;
            keys.repaint();
        }
    }

    void press (int note)
    {
        if (note < 0 || note == pressedNote)
            return;

        if (pressedNote >= 0)
            processor.stopPreview();

        pressedNote = note;
        processor.startPreviewNote (note);
        keys.repaint();
    }

    void release()
    {
        if (pressedNote >= 0)
        {
            processor.stopPreview();
            pressedNote = -1;
            keys.repaint();
        }
    }

    //==============================================================================
    struct KeyState
    {
        bool inScale = false, tonic = false, voiced = false, chordTone = false, lit = false, hover = false;
    };

    KeyState stateOf (int note) const
    {
        KeyState s;
        const int p = cb::pc (note);
        s.inScale = scaleMask[(size_t) p];
        s.tonic = p == key.tonic;
        s.voiced = std::find (chordNotes.begin(), chordNotes.end(), note) != chordNotes.end();
        s.chordTone = chordMask[(size_t) p];
        s.lit = (note >= 0 && note < 128 && sounding[(size_t) note]) || note == pressedNote;
        s.hover = note == hoverNote;
        return s;
    }

    void paintKeys (juce::Graphics& g)
    {
        const auto bounds = keys.getLocalBounds().toFloat();

        if (bounds.isEmpty())
            return;

        const float ww = whiteWidth();
        const float radius = juce::jlimit (2.0f, 5.0f, ww * 0.2f);

        // key bed
        g.setColour (juce::Colour (0xff11151a));
        g.fillRoundedRectangle (bounds.expanded (1.0f, 0.0f).withTrimmedTop (-1.0f), radius + 1.0f);

        for (int n = lowNote; n < lowNote + kNumKeys; ++n)
            if (! isBlackKey (n))
                paintWhiteKey (g, n, radius);

        for (int n = lowNote; n < lowNote + kNumKeys; ++n)
            if (! isBlackKey (n) && stateOf (n).lit)
                paintGlow (g, keyRect (n).reduced (0.5f, 0.0f), radius);

        for (int n = lowNote; n < lowNote + kNumKeys; ++n)
            if (isBlackKey (n))
                paintBlackKey (g, n, radius * 0.7f);

        for (int n = lowNote; n < lowNote + kNumKeys; ++n)
            if (isBlackKey (n) && stateOf (n).lit)
                paintGlow (g, keyRect (n), radius * 0.7f);

        // felt strip / shadow at the top
        g.setGradientFill (juce::ColourGradient::vertical (juce::Colours::black.withAlpha (0.55f), bounds.getY(),
                                                           juce::Colours::black.withAlpha (0.0f),
                                                           bounds.getY() + juce::jmax (4.0f, bounds.getHeight() * 0.07f)));
        g.fillRect (bounds.withHeight (juce::jmax (4.0f, bounds.getHeight() * 0.07f)));
    }

    static juce::Path keyPath (juce::Rectangle<float> r, float radius)
    {
        juce::Path p;
        p.addRoundedRectangle (r.getX(), r.getY(), r.getWidth(), r.getHeight(), radius, radius, false, false, true, true);
        return p;
    }

    void paintWhiteKey (juce::Graphics& g, int note, float radius)
    {
        const auto s = stateOf (note);
        const auto r = keyRect (note).reduced (0.5f, 0.0f);
        const float h = r.getHeight();

        auto base = s.inScale ? whiteInScale : whiteOutScale;

        if (s.voiced)
            base = chordColour.interpolatedWith (juce::Colours::white, 0.1f);

        if (s.lit)
            base = Col::accent.interpolatedWith (juce::Colours::white, 0.25f);

        if (s.hover && ! s.lit)
            base = base.brighter (0.06f);

        juce::ColourGradient grad (base.darker (0.16f), 0.0f, r.getY(), base.darker (0.06f), 0.0f, r.getBottom(), false);
        grad.addColour (0.18, base);
        grad.addColour (0.85, base);
        const auto path = keyPath (r, radius);
        g.setGradientFill (grad);
        g.fillPath (path);

        // front lip
        {
            juce::Graphics::ScopedSaveState state (g);
            g.reduceClipRegion (path);
            const float lip = juce::jmax (2.0f, h * 0.035f);
            g.setColour (base.darker (0.2f));
            g.fillRect (r.withTop (r.getBottom() - lip));
        }

        const bool showLabel = r.getWidth() >= 12.0f && h >= 44.0f;
        const float labelH = showLabel ? 12.0f : 0.0f;
        const float dotD = juce::jlimit (3.0f, 6.5f, r.getWidth() * 0.3f);
        const auto dotCentre = juce::Point<float> (r.getCentreX(), r.getBottom() - labelH - 5.0f - dotD * 0.5f);

        paintDot (g, s, dotCentre, dotD, false);

        if (showLabel && cb::pc (note) == 0)
        {
            g.setFont (cb::ui::font (9.5f, 600));
            g.setColour (s.voiced || s.lit ? darkMark.withAlpha (0.7f) : keyLabel);
            // Octave numbers follow FL Studio's piano roll (MIDI 60 = C5), the host this plug-in is made for.
            g.drawText ("C" + juce::String (note / 12), r.withTop (r.getBottom() - labelH - 3.0f).withHeight (labelH),
                        juce::Justification::centred, false);
        }
    }

    void paintBlackKey (juce::Graphics& g, int note, float radius)
    {
        const auto s = stateOf (note);
        const auto r = keyRect (note);

        auto base = s.inScale ? blackInScale : blackOutScale;

        if (s.voiced)
            base = chordColour.darker (0.25f);

        if (s.lit)
            base = Col::accent.darker (0.05f);

        if (s.hover && ! s.lit)
            base = base.brighter (0.15f);

        // shadow on the white keys
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillPath (keyPath (r.translated (1.0f, 1.5f), radius));

        const auto path = keyPath (r, radius);
        g.setGradientFill (juce::ColourGradient::vertical (base.brighter (0.06f), r.getY(), base.darker (0.12f), r.getBottom()));
        g.fillPath (path);

        // front bevel
        const float bevel = juce::jmax (3.0f, r.getHeight() * 0.08f);
        const auto face = juce::Rectangle<float> (r.getX() + 1.5f, r.getBottom() - bevel - 1.0f, r.getWidth() - 3.0f, bevel);
        g.setColour (base.brighter (s.lit || s.voiced ? 0.12f : 0.22f));
        g.fillRoundedRectangle (face, juce::jmin (radius, 2.5f));

        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.strokePath (path, juce::PathStrokeType (1.0f));

        const float dotD = juce::jlimit (3.0f, 5.5f, r.getWidth() * 0.34f);
        paintDot (g, s, { r.getCentreX(), face.getY() - 3.0f - dotD * 0.5f }, dotD, true);
    }

    void paintDot (juce::Graphics& g, const KeyState& s, juce::Point<float> centre, float diameter, bool onBlack) const
    {
        juce::Colour c;
        float d = diameter;

        if (s.voiced || s.lit)
        {
            if (! s.inScale && ! s.tonic)
                return;

            c = darkMark.withAlpha (s.tonic ? 0.75f : 0.45f);
        }
        else if (s.chordTone)
        {
            c = onBlack ? chordColour : chordColour.darker (0.15f);
        }
        else if (s.tonic)
        {
            c = onBlack ? Col::accent : tonicDotOnWhite;
            d *= 1.15f;
        }
        else if (s.inScale)
        {
            c = onBlack ? scaleDotOnBlack : scaleDotOnWhite;
            d *= 0.8f;
        }
        else
        {
            return;
        }

        g.setColour (c);
        g.fillEllipse (juce::Rectangle<float> (d, d).withCentre (centre));
    }

    static void paintGlow (juce::Graphics& g, juce::Rectangle<float> r, float radius)
    {
        g.setColour (Col::accent.withAlpha (0.16f));
        g.drawRoundedRectangle (r.expanded (2.5f), radius + 2.5f, 3.0f);
        g.setColour (Col::accent.withAlpha (0.55f));
        g.drawRoundedRectangle (r.expanded (0.5f), radius + 0.5f, 1.5f);
    }

    //==============================================================================
    void paintHeader (juce::Graphics& g) const
    {
        auto area = titleArea;

        if (area.isEmpty())
            return;

        // Legend (right)
        struct Item { const char* text; juce::Colour colour; };
        const Item items[] = { { "Scale", scaleDotOnWhite.brighter (0.1f) },
                               { "Chord", chord.has_value() ? chordColour : Col::tonic },
                               { "Playing", Col::accent } };

        const auto legendFont = cb::ui::font (11.0f);
        float legendW = 0.0f;

        for (const auto& item : items)
            legendW += 7.0f + 5.0f + std::ceil (cb::ui::textWidth (legendFont, item.text)) + 4.0f + 12.0f;

        const bool showLegend = (float) area.getWidth() > legendW + 150.0f;

        if (showLegend)
        {
            auto legend = area.removeFromRight ((int) std::ceil (legendW)).toFloat();
            g.setFont (legendFont);

            for (const auto& item : items)
            {
                legend.removeFromLeft (12.0f);
                const auto dot = legend.removeFromLeft (7.0f);
                g.setColour (item.colour);
                g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (dot.getCentre()));
                legend.removeFromLeft (5.0f);
                const float w = std::ceil (cb::ui::textWidth (legendFont, item.text)) + 4.0f;
                g.setColour (Col::textDim);
                g.drawText (item.text, legend.removeFromLeft (w), juce::Justification::centredLeft, false);
            }

            area.removeFromRight (8);
        }

        // Chord (or key) summary (left)
        juce::String name, detail;
        juce::Colour nameColour = Col::text;
        std::vector<int> pcs;

        if (chord.has_value())
        {
            name = chord->name (key);
            detail = cb::romanNumeral (*chord, key);
            nameColour = chordColour;
            pcs = chord->pitchClasses();
        }
        else
        {
            name = key.name();
            const auto scalePcs = key.pitchClasses();
            pcs.assign (scalePcs.begin(), scalePcs.end());
        }

        auto a = area.toFloat();
        const auto nameFont = cb::ui::font (13.5f, 600);
        g.setFont (nameFont);
        g.setColour (nameColour);
        const float nameW = juce::jmin (a.getWidth(), cb::ui::textWidth (nameFont, name) + 1.0f);
        g.drawText (name, a.removeFromLeft (nameW), juce::Justification::centredLeft, true);

        const auto smallFont = cb::ui::font (12.0f);
        g.setFont (smallFont);

        if (detail.isNotEmpty() && a.getWidth() > 40.0f)
        {
            a.removeFromLeft (8.0f);
            const float w = juce::jmin (a.getWidth(), cb::ui::textWidth (smallFont, detail) + 1.0f);
            g.setColour (Col::textDim);
            g.drawText (detail, a.removeFromLeft (w), juce::Justification::centredLeft, true);
        }

        juce::StringArray noteNames;

        for (int p : pcs)
            noteNames.add (juce::String (key.spell (cb::pc (p))));

        const auto notes = noteNames.joinIntoString (juce::String (juce::CharPointer_UTF8 ("  \xc2\xb7  ")));
        const float notesW = cb::ui::textWidth (smallFont, notes);

        if (notes.isNotEmpty() && a.getWidth() > notesW + 14.0f)
        {
            a.removeFromLeft (14.0f);
            g.setColour (Col::textDim.withAlpha (0.8f));
            g.drawText (notes, a, juce::Justification::centredLeft, true);
        }
    }

    KeyboardGuide& owner;
    ChordbloomProcessor& processor;
    KeysView keys { *this };

    cb::Key key;
    cb::VoicingSettings voicing;
    std::optional<cb::Chord> chord;
    std::vector<int> explicitNotes;   // exact voicing from the editor (may be empty)
    std::vector<int> chordNotes;
    std::bitset<128> sounding;
    std::array<bool, 12> scaleMask {}, chordMask {};
    juce::Colour chordColour = Col::tonic;

    int lowNote = kDefaultLowNote;
    int hoverNote = -1, pressedNote = -1;
    juce::Rectangle<int> titleArea;
};

//==============================================================================
KeyboardGuide::KeyboardGuide (ChordbloomProcessor& p) : impl (std::make_unique<Impl> (*this, p)) {}
KeyboardGuide::~KeyboardGuide() = default;

void KeyboardGuide::paint (juce::Graphics& g)
{
    impl->paintHeader (g);
}

void KeyboardGuide::resized()
{
    auto& d = *impl;
    auto b = getLocalBounds().reduced (14, 10);
    d.titleArea = b.removeFromTop (18);
    b.removeFromTop (b.getHeight() > 90 ? 10 : 6);

    const float ww = (float) b.getWidth() / (float) kNumWhiteKeys;
    const int keysH = juce::jmax (0, juce::jmin (b.getHeight(), juce::roundToInt (ww * 5.8f)));
    d.keys.setBounds (b.withHeight (keysH).withY (b.getY() + (b.getHeight() - keysH) / 2));
}

void KeyboardGuide::setDisplayChord (std::optional<cb::Chord> newChord)
{
    setDisplay (newChord, {});
}

void KeyboardGuide::setDisplay (std::optional<cb::Chord> newChord, const std::vector<int>& voicedNotes)
{
    auto& d = *impl;

    std::vector<int> notes;

    if (newChord.has_value())
    {
        for (int n : voicedNotes)
            if (n >= 0 && n <= 127)
                notes.push_back (n);

        std::sort (notes.begin(), notes.end());
        notes.erase (std::unique (notes.begin(), notes.end()), notes.end());
    }

    if (newChord == d.chord && notes == d.explicitNotes)
        return;

    d.chord = newChord;
    d.explicitNotes = std::move (notes);
    d.updateChordNotes();
}

void KeyboardGuide::refresh()
{
    auto& d = *impl;
    d.key = d.processor.getKey();
    d.voicing = d.processor.getVoicingSettings();
    d.updateChordNotes();
}
