#include "ui/MelodyRoll.h"

#include "core/NoteEdit.h"
#include "ui/Theme.h"

#include <algorithm>
#include <array>
#include <cmath>

// The roll canvas (final-spec 7.2-7.9; owner WP5). Child 0 is the grid surface (Impl, which paints the whole card,
// the chord strip, the grid, the pills and the velocity lane), child 1 the RollToolbar (WP6) in the toolbar row.
// Every edit works on a working copy taken at mouse-down and goes through cb::NoteEdit; one processor.setLayerNotes
// per gesture or command (one undo step), never per mouse move.

namespace
{
namespace Col = cb::ui::Colours;

constexpr int kMinVisibleBeats = 16;              // = ProgressionLane: the roll's beat axis is the lane's track
int roundUpToBar (int beats) { return ((beats + 3) / 4) * 4; }

constexpr float kPadX = 12.0f, kPadY = 10.0f;     // horizontal inset = the lane's track inset
constexpr float kTargetRows = 17.0f;              // about 1 1/2 octaves visible
constexpr float kMinRowH = 9.0f, kMaxRowH = 16.0f;
constexpr float kDragThreshold = 3.0f;            // px before a Ctrl press becomes a marquee
constexpr float kStalkReach = 4.0f;               // px: a velocity-lane press this close to a stalk grabs it
constexpr juce::uint32 kHintMillis = 4500;

// Roll settings kept in the APVTS state tree (saved with the plugin state, like the lane's Spark settings).
struct LayerKeys
{
    const char* snap;
    const char* grid;
    const char* top;
};

constexpr LayerKeys kLayerKeys[cb::numLayers] = {
    { "cb.melodySnap", "cb.melodyGrid", "cb.melodyTopNote" },
    { "cb.arpSnap", "cb.arpGrid", "cb.arpTopNote" },
    { "cb.bassSnap", "cb.bassGrid", "cb.bassTopNote" },
};
constexpr const char* kLayerProp = "cb.rollLayer";
constexpr const char* kVelLaneProp = "cb.rollVelLane";
constexpr const char* kGhostsProp = "cb.melodyGhosts";

constexpr cb::Layer kLayers[cb::numLayers] = { cb::Layer::Melody, cb::Layer::Arp, cb::Layer::Bass };

int indexOf (cb::Layer layer) { return juce::jlimit (0, cb::numLayers - 1, (int) layer); }
bool isGenerated (cb::Layer layer) { return layer == cb::Layer::Arp || layer == cb::Layer::Bass; }

float widthOf (const juce::Font& f, const juce::String& s) { return juce::GlyphArrangement::getStringWidth (f, s); }
juce::String toJuce (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }
juce::String middleDot() { return juce::String::charToString ((juce::juce_wchar) 0x00b7); }
juce::String layerTitle (cb::Layer layer) { return juce::String (cb::layerName (layer)); }
juce::String layerLower (cb::Layer layer) { return layerTitle (layer).toLowerCase(); }

void noFocus (juce::Component& c)
{
    c.setWantsKeyboardFocus (false);
    c.setMouseClickGrabsKeyboardFocus (false);
}

// One process-wide clipboard (message thread), so notes copy between layers and instances (final-spec 7.7).
cb::NoteClip& clipboard()
{
    static cb::NoteClip clip;
    return clip;
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

// The four-pointed sparkle of the Spark buttons (same drawing as the lane's).
void drawSparkle (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour colour)
{
    auto star = [] (juce::Point<float> c, float rad)
    {
        juce::Path p;
        const float w = rad * 0.28f;
        p.startNewSubPath (c.x, c.y - rad);
        p.quadraticTo (c.x + w, c.y - w, c.x + rad, c.y);
        p.quadraticTo (c.x + w, c.y + w, c.x, c.y + rad);
        p.quadraticTo (c.x - w, c.y + w, c.x - rad, c.y);
        p.quadraticTo (c.x - w, c.y - w, c.x, c.y - rad);
        p.closeSubPath();
        return p;
    };

    const float s = std::min (r.getWidth(), r.getHeight());
    const auto c = r.getCentre();
    g.setColour (colour);
    g.fillPath (star ({ c.x - s * 0.08f, c.y + s * 0.06f }, s * 0.42f));
    g.fillPath (star ({ c.x + s * 0.34f, c.y - s * 0.32f }, s * 0.17f));
}

void drawEllipsisDots (juce::Graphics& g, juce::Point<float> centre, juce::Colour colour)
{
    g.setColour (colour);

    for (int k = -1; k <= 1; ++k)
        g.fillEllipse (juce::Rectangle<float> (3.0f, 3.0f).withCentre (centre.translated ((float) k * 4.5f, 0.0f)));
}

std::vector<cb::MelodyNote> notesAt (const cb::Melody& m, const std::vector<int>& indices)
{
    std::vector<cb::MelodyNote> out;

    for (int i : cb::validSelection (m, indices))
        out.push_back (m.notes[(size_t) i]);

    return out;
}

using Command = MelodyRoll::Command;
using MenuSection = MelodyRoll::MenuSection;

constexpr int kCommandIdBase = 1;      // menu item id = kCommandIdBase + (int) Command
constexpr int kSnapIdBase = 101;
constexpr int kGridIdBase = 111;
constexpr int kNumCommands = (int) Command::ToggleOtherLayers + 1;
} // namespace

//==============================================================================
struct MelodyRoll::Impl final : public juce::Component,
                                public juce::TooltipClient,
                                private juce::Timer
{
    struct View
    {
        cb::MelodySnap snap = cb::MelodySnap::Scale;
        cb::RateDivision grid = cb::RateDivision::Eighth;
        int top = -1;              // highest visible row; -1 = not shown yet (centred on first show)
        double lastLength = -1.0;  // last note clicked / drawn / resized in this layer (-1: the layer default)
        float lastVelocity = -1.0f;
    };

    enum class Gesture
    {
        none,
        swallow,      // pressed on something that consumes the press (deselect click, pill, strip, lane gap)
        move,         // a note or the selection being dragged (also a freshly drawn note)
        resize,       // a right edge being dragged
        erase,        // right button down: delete under the mouse
        pendingCtrl,  // Ctrl down: a click toggles, a drag becomes a marquee
        marquee,
        duplicate,    // Shift + drag of a note / the selection
        velocity,     // a velocity-lane stalk
        cardButton    // the empty-state "Generate a melody" button
    };

    explicit Impl (ChordbloomProcessor& p) : processor (p), rollToolbar (std::make_unique<RollToolbar> (p))
    {
        noFocus (*this);
        noFocus (*rollToolbar);
        setOpaque (false);
        setTitle ("Note roll");
        setDescription ("Draw and edit the melody, arp and bass over the progression. Rows light up with the scale and "
                        "the chord at each beat.");

        for (int i = 0; i < cb::numLayers; ++i)
        {
            const bool melody = kLayers[i] == cb::Layer::Melody;
            views[(size_t) i].snap = melody ? cb::MelodySnap::Scale : cb::MelodySnap::ChordTones;
            views[(size_t) i].grid = melody ? cb::RateDivision::Eighth : cb::RateDivision::Sixteenth;
        }

        auto& tb = *rollToolbar;
        tb.onLayerChosen = [this] (cb::Layer l) { setLayer (l); };
        tb.onTabMenu = [this] (cb::Layer l) { showMenu (MenuSection::Layer, -1.0, l, toolbarItemScreenArea (tabItem (l))); };
        tb.onSnapChosen = [this] (cb::MelodySnap s) { setSnap (s); };
        tb.onGridChosen = [this] (cb::RateDivision r) { setGrid (r); };
        tb.onVelocityLaneToggled = [this] (bool on) { setVelocityLane (on); };
        tb.onMoreClicked = [this] (juce::Rectangle<int> area) { showMenu (MenuSection::All, -1.0, std::nullopt, area); };
        tb.onHint = [this] (const juce::String& message) { showHint (message); };
        tb.onMelodyGenerated = [this]
        {
            syncAll();
            centreOnNotes();
        };

        syncAll();
        startTimerHz (30);
    }

    ~Impl() override
    {
        stopTimer();

        if (auditioning)
            processor.stopPreview();
    }

    //==============================================================================
    // Layer, settings
    int cur() const { return indexOf (layer); }
    View& view() { return views[(size_t) cur()]; }
    const View& view() const { return views[(size_t) cur()]; }
    cb::NoteLimits limits() const { return cb::layerLimits (layer); }

    void loadSettings()
    {
        const auto& state = processor.getAPVTS().state;

        for (int i = 0; i < cb::numLayers; ++i)
        {
            auto& v = views[(size_t) i];
            const auto& k = kLayerKeys[i];
            v.snap = (cb::MelodySnap) juce::jlimit (0, cb::numMelodySnapModes - 1, (int) state.getProperty (k.snap, (int) v.snap));
            v.grid = (cb::RateDivision) juce::jlimit (0, cb::numRateDivisions - 1, (int) state.getProperty (k.grid, (int) v.grid));

            if (state.hasProperty (k.top))
                v.top = (int) state.getProperty (k.top);
        }

        showGhosts = (bool) state.getProperty (kGhostsProp, true);
        velLane = (bool) state.getProperty (kVelLaneProp, false);

        const auto wanted = cb::layerFromKey (state.getProperty (kLayerProp, "melody").toString().toStdString())
                                .value_or (cb::Layer::Melody);

        if (wanted != layer)
            switchLayer (wanted, false);
    }

    void setLayer (cb::Layer l)
    {
        if (l == cb::Layer::NumLayers)
            l = cb::Layer::Melody;

        processor.getAPVTS().state.setProperty (kLayerProp, juce::String (cb::layerKey (l)), nullptr);

        if (l != layer)
            switchLayer (l, true);
    }

    void switchLayer (cb::Layer l, bool doLayout)
    {
        dropGesture();
        layer = l;
        selection.clear();
        editedSlots = processor.getEditedSlots (layer);
        editedSlots.resize (progression.slots.size(), false);
        pushViewToToolbar();

        if (doLayout)
        {
            layout();
            repaint();
        }
    }

    void pushViewToToolbar()
    {
        auto& tb = *rollToolbar;
        tb.setLayer (layer);
        tb.setSnap (view().snap);
        tb.setGrid (view().grid);
        tb.setVelocityLaneOn (velLane);
        tb.refresh();
    }

    void setSnap (cb::MelodySnap s)
    {
        view().snap = s;
        processor.getAPVTS().state.setProperty (kLayerKeys[cur()].snap, (int) s, nullptr);
        rollToolbar->setSnap (s);
        repaint();
    }

    void setGrid (cb::RateDivision r)
    {
        view().grid = r;
        processor.getAPVTS().state.setProperty (kLayerKeys[cur()].grid, (int) r, nullptr);
        rollToolbar->setGrid (r);
        repaint();
    }

    void setVelocityLane (bool on)
    {
        processor.getAPVTS().state.setProperty (kVelLaneProp, on, nullptr);

        if (velLane != on)
        {
            velLane = on;
            layout();
            repaint();
        }

        rollToolbar->setVelocityLaneOn (on);
    }

    void setOtherLayersShown (bool on)
    {
        showGhosts = on;
        processor.getAPVTS().state.setProperty (kGhostsProp, on, nullptr);
        repaint();
    }

    double gridStep() const { return cb::rateInBeats (view().grid); }

    float defaultVelocity() const
    {
        if (layer == cb::Layer::Melody)
            return cb::kDefaultMelodyVelocity;

        if (auto* v = processor.getAPVTS().getRawParameterValue (cb::ParamIDs::velocity))
            return juce::jlimit (0.05f, 1.0f, v->load());

        return cb::kDefaultMelodyVelocity;
    }

    double newNoteLength() const
    {
        const auto& v = view();

        if (v.lastLength > 0.0)
            return std::max (v.lastLength, cb::kMinMelodyNoteBeats);

        return layer == cb::Layer::Melody ? 1.0 : std::max (gridStep(), cb::kMinMelodyNoteBeats);
    }

    float newNoteVelocity() const { return view().lastVelocity > 0.0f ? view().lastVelocity : defaultVelocity(); }

    void rememberNote (const cb::MelodyNote& n)
    {
        view().lastLength = n.lengthBeats;
        view().lastVelocity = n.velocity;
    }

    //==============================================================================
    // Data
    // Pulls everything from the processor. The surface is laid out and repainted only when something it shows
    // changed: the layer serial also moves on parameter-only changes (a Swing or Gate knob drag, host automation),
    // which must not repaint the whole roll at 30 Hz.
    void syncAll()
    {
        const auto oldViews = views;
        const auto oldLayer = layer;
        const bool oldGhosts = showGhosts, oldVelLane = velLane;
        const auto oldGhostSerial = ghostSerial;
        loadSettings();   // a loaded project state may bring other roll settings

        auto newProgression = processor.getProgression();
        const auto newKey = processor.getKey();
        bool changed = ! synced || layer != oldLayer || showGhosts != oldGhosts || velLane != oldVelLane
                    || ! (newProgression == progression) || ! (newKey == key);

        for (size_t i = 0; i < views.size(); ++i)
            changed = changed || views[i].snap != oldViews[i].snap || views[i].grid != oldViews[i].grid
                   || views[i].top != oldViews[i].top;

        if (! synced || ! (newProgression == progression) || ! (newKey == key))
        {
            progression = std::move (newProgression);
            key = newKey;
            total = progression.totalBeats();
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
        }

        changed = syncLayers (true) || changed;
        ghostSerial = processor.getGhostSerial();

        if (! synced || ghostSerial != oldGhostSerial)
        {
            ghosts = processor.getGhostMelodies();
            changed = true;
        }

        synced = true;
        rollToolbar->setLayer (layer);
        rollToolbar->setSnap (view().snap);
        rollToolbar->setGrid (view().grid);
        rollToolbar->setVelocityLaneOn (velLane);
        rollToolbar->refresh();

        if (! changed)
            return;

        layout();
        repaint();
    }

    // Pulls every layer's notes and flags. clearOnChange: the selection goes when the current layer's notes changed
    // from outside a gesture (never in the middle of one: the gesture's working copy wins at mouse-up).
    // Returns true when anything the surface shows from the layers changed.
    bool syncLayers (bool clearOnChange)
    {
        serial = processor.getLayerSerial();
        bool changed = false;

        for (int i = 0; i < cb::numLayers; ++i)
        {
            auto notes = processor.getLayerNotes (kLayers[i]);
            const bool on = processor.isLayerOn (kLayers[i]);
            const bool custom = processor.isLayerCustom (kLayers[i]);

            if (notes != layerNotes[(size_t) i])
            {
                changed = true;

                if (i == cur() && clearOnChange && ! inGridGesture())
                    selection.clear();

                layerNotes[(size_t) i] = std::move (notes);
            }

            changed = changed || on != layerOn[(size_t) i] || custom != layerCustom[(size_t) i];
            layerOn[(size_t) i] = on;
            layerCustom[(size_t) i] = custom;
        }

        auto edited = processor.getEditedSlots (layer);
        edited.resize (progression.slots.size(), false);
        changed = changed || edited != editedSlots;
        editedSlots = std::move (edited);

        if (! inGridGesture())
            selection = cb::validSelection (layerNotes[(size_t) cur()], selection);

        return changed;
    }

    const cb::Melody& notes() const { return layerNotes[(size_t) cur()]; }

    // What the grid shows: the gesture's working copy while one runs, else the layer's notes.
    const cb::Melody& shown() const { return inGridGesture() ? working : notes(); }
    const std::vector<int>& shownSelection() const { return inGridGesture() ? workingSel : selection; }

    bool isSelected (int index) const
    {
        const auto& sel = shownSelection();
        return std::binary_search (sel.begin(), sel.end(), index);
    }

    int parkedCount() const
    {
        if (total <= 0)
            return 0;

        int n = 0;

        for (const auto& note : shown().notes)
            if (note.startBeat >= (double) total)
                ++n;

        return n;
    }

    // Commits an edited copy of the current layer (one undo step when it differs from `before`) and re-identifies the
    // selection (editedSelection indexes `edited`) in what the processor now holds.
    void commitEdit (const cb::Melody& edited, const std::vector<int>& editedSelection, const cb::Melody& before)
    {
        const auto wanted = notesAt (edited, editedSelection);
        // Nothing to commit, but the layer's notes changed from outside meanwhile (undo, an automation re-render):
        // the selection goes (2.2), as for a marquee, instead of landing on whatever the new notes hold there.
        const bool changedOutside = edited == before && processor.getLayerNotes (layer) != before;

        if (edited != before)
            processor.setLayerNotes (layer, edited);

        syncLayers (false);
        selection = changedOutside ? std::vector<int>() : cb::findNotes (notes(), wanted);
        rollToolbar->refresh();
        repaint();
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
        laneArea = {};

        if (velLane && inner.getHeight() > 60.0f)
        {
            const float laneH = juce::jlimit (24.0f, 40.0f, std::round (0.18f * inner.getHeight()));
            laneArea = inner.removeFromBottom (laneH);
            inner.removeFromBottom (compact ? 3.0f : 4.0f);
        }

        gridArea = inner;
        visibleBeats = std::max (kMinVisibleBeats, roundUpToBar (total));
        pxPerBeat = gridArea.getWidth() / (float) visibleBeats;
        rowH = juce::jlimit (kMinRowH, kMaxRowH, gridArea.getHeight() / kTargetRows);

        auto& v = view();

        if (v.top < 0 && gridArea.getHeight() >= 2.0f * kMinRowH)
        {
            // First show of this layer (once the grid has a size): centre on its notes, else on the layer's register.
            int centre = layer == cb::Layer::Melody ? 72 : (layer == cb::Layer::Bass ? 40 : arpCentre());

            if (! notes().empty())
            {
                int lo = 127, hi = 0;
                for (const auto& n : notes().notes)
                {
                    lo = std::min (lo, n.note);
                    hi = std::max (hi, n.note);
                }
                centre = (lo + hi) / 2;
            }

            v.top = centre + visibleRows() / 2;
        }

        if (v.top >= 0)
            v.top = clampTop (v.top);

        rollToolbar->setBounds (juce::roundToInt (toolbarArea.getX()), juce::roundToInt (toolbarArea.getY()),
                                std::max (0, juce::roundToInt (toolbarArea.getWidth()) - rightInset),
                                juce::roundToInt (toolbarArea.getHeight()));
    }

    int arpCentre() const
    {
        try
        {
            return juce::jlimit (40, 96, processor.getArpSettings().centreNote);
        }
        catch (...)
        {
            return 65;
        }
    }

    int topNote() const { return view().top; }
    int visibleRows() const { return std::max (1, (int) std::floor (gridArea.getHeight() / rowH + 0.5f)); }

    int clampTop (int t) const
    {
        const auto lim = limits();
        const int lowestTop = std::min (lim.highest, lim.lowest + visibleRows() - 1);
        return juce::jlimit (lowestTop, lim.highest, t);
    }

    int bottomNote() const { return std::max (limits().lowest, topNote() - visibleRows() + 1); }
    int lowestPaintedRow() const { return std::max (limits().lowest, topNote() - (int) std::ceil (gridArea.getHeight() / rowH)); }

    void scrollTo (int newTop)
    {
        newTop = clampTop (newTop);

        if (newTop == topNote())
            return;

        view().top = newTop;
        processor.getAPVTS().state.setProperty (kLayerKeys[cur()].top, newTop, nullptr);
        repaint();
    }

    void centreOnNotes()
    {
        if (notes().empty())
            return;

        int lo = 127, hi = 0;
        for (const auto& n : notes().notes)
        {
            lo = std::min (lo, n.note);
            hi = std::max (hi, n.note);
        }

        if (lo >= bottomNote() && hi <= topNote())
            return;

        scrollTo ((lo + hi) / 2 + visibleRows() / 2);
    }

    float beatToX (double beat) const { return gridArea.getX() + (float) beat * pxPerBeat; }
    double xToBeat (float x) const { return (double) ((x - gridArea.getX()) / pxPerBeat); }
    float rowTop (int note) const { return gridArea.getY() + (float) (topNote() - note) * rowH; }
    int yToNote (float y) const { return topNote() - (int) std::floor ((y - gridArea.getY()) / rowH); }

    juce::Rectangle<float> noteRect (const cb::MelodyNote& n) const
    {
        const float x0 = beatToX (n.startBeat);
        const float x1 = beatToX (n.startBeat + n.lengthBeats);
        return { x0 + 0.5f, rowTop (n.note) + 1.0f, std::max (3.0f, x1 - x0 - 1.0f), std::max (3.0f, rowH - 2.0f) };
    }

    juce::Rectangle<float> laneInner() const { return laneArea.reduced (0.0f, 4.0f); }
    // A note's stalk in the velocity lane: at its start, kept 4 px inside the lane so a beat-0 dot draws whole.
    float stalkX (const cb::MelodyNote& n) const
    {
        return juce::jlimit (laneArea.getX() + 4.0f, std::max (laneArea.getX() + 4.0f, laneArea.getRight() - 4.0f),
                             std::round (beatToX (n.startBeat)) + 1.0f);
    }
    float velocityToY (float v) const { const auto r = laneInner(); return r.getBottom() - v * r.getHeight(); }
    float yToVelocity (float y) const
    {
        const auto r = laneInner();
        return juce::jlimit (0.05f, 1.0f, (r.getBottom() - y) / std::max (1.0f, r.getHeight()));
    }

    static RollToolbarItem tabItem (cb::Layer l)
    {
        switch (l)
        {
            case cb::Layer::Arp:  return RollToolbarItem::TabArp;
            case cb::Layer::Bass: return RollToolbarItem::TabBass;
            case cb::Layer::Melody:
            case cb::Layer::NumLayers:
            default:              return RollToolbarItem::TabMelody;
        }
    }

    juce::Rectangle<int> toolbarItemScreenArea (RollToolbarItem item) const
    {
        const auto r = rollToolbar->getItemBounds (item);

        if (r.isEmpty() || ! rollToolbar->isShowing())
            return {};

        return rollToolbar->localAreaToGlobal (r);
    }

    //==============================================================================
    // Pills (grid top-right), the empty-state card
    static juce::Font pillFont() { return cb::ui::font (11.5f, 600); }

    struct Pills
    {
        juce::Rectangle<float> hint, selection, off;
    };

    bool hintVisible() const { return hintText.isNotEmpty() && juce::Time::getMillisecondCounter() < hintUntil; }
    juce::String selectionPillText() const { return juce::String ((int) shownSelection().size()) + " selected  " + middleDot() + " "; }
    juce::String offPillText() const { return layerTitle (layer) + " is off  " + middleDot() + "  "; }

    Pills pills() const
    {
        Pills p;

        if (gridArea.getWidth() < 60.0f || gridArea.getHeight() < 30.0f)
            return p;

        const float h = compact ? 18.0f : 20.0f;
        const float y = gridArea.getY() + 5.0f;
        const float left = gridArea.getX() + 6.0f;
        float right = gridArea.getRight() - 6.0f;
        const auto f = pillFont();

        auto take = [&] (float w) -> juce::Rectangle<float>
        {
            if (right - w < left)
                return {};

            const auto r = juce::Rectangle<float> (right - w, y, w, h);
            right -= w + 6.0f;
            return r;
        };

        if (! layerOn[(size_t) cur()])
            p.off = take (std::ceil (10.0f + 7.0f + 6.0f + widthOf (f, offPillText()) + widthOf (f, "Turn on") + 11.0f));

        if (! shownSelection().empty())
            p.selection = take (std::ceil (11.0f + widthOf (f, selectionPillText()) + 14.0f + 10.0f));

        if (hintVisible())
            p.hint = take (std::ceil (22.0f + widthOf (cb::ui::font (11.5f, 400), hintText)));

        return p;
    }

    enum class CardKind { none, generate, text };

    struct Card
    {
        CardKind kind = CardKind::none;
        juce::Rectangle<float> box, button, textArea;
        juce::String line1, line2;
        bool enabled = true;
    };

    static juce::Font cardButtonFont() { return cb::ui::font (12.5f, 600); }

    Card card() const
    {
        Card c;

        if (! shown().empty() || gridArea.getHeight() < 40.0f || gridArea.getWidth() < 120.0f
            || inGridGesture())
            return c;

        const auto f2 = cb::ui::font (11.5f, 400);

        if (layer == cb::Layer::Melody)
        {
            c.kind = CardKind::generate;
            c.enabled = ! progression.empty();
            c.line1 = "Generate a melody";
            c.line2 = c.enabled ? "or click to draw  " + middleDot() + "  Ctrl+drag selects  " + middleDot()
                                      + "  right-click deletes  " + middleDot() + "  Alt = no grid"
                                : juce::String ("Add chords below first, or click to draw");
        }
        else
        {
            c.kind = CardKind::text;

            if (layerCustom[(size_t) cur()])
                c.line1 = "Draw your notes, or Reset to follow the chords";
            else if (progression.empty())
                c.line1 = "Add chords below: the " + layerLower (layer) + " follows them";
            else
                c.line1 = layer == cb::Layer::Bass ? juce::String ("The bass is Off: pick a pattern, or draw your own notes")
                                                   : juce::String ("No arp notes: pick a pattern, or draw your own");

            c.line2 = "click to draw  " + middleDot() + "  right-click deletes  " + middleDot() + "  Alt = no grid";
        }

        const bool withText = gridArea.getHeight() >= 84.0f;
        const auto f1 = c.kind == CardKind::generate ? cardButtonFont() : cb::ui::font (13.0f, 600);
        const float buttonW = c.kind == CardKind::generate ? std::ceil (13.0f + 7.0f + widthOf (f1, c.line1) + 30.0f)
                                                            : widthOf (f1, c.line1);
        const float textW = withText ? widthOf (f2, c.line2) : 0.0f;
        const float boxW = std::min (gridArea.getWidth() - 24.0f, std::max (buttonW, textW) + 36.0f);
        const float boxH = c.kind == CardKind::generate ? (withText ? 74.0f : 44.0f) : (withText ? 52.0f : 32.0f);
        c.box = juce::Rectangle<float> (boxW, std::min (boxH, gridArea.getHeight() - 8.0f)).withCentre (gridArea.getCentre());

        if (c.kind == CardKind::generate)
        {
            c.button = juce::Rectangle<float> (std::min (buttonW, c.box.getWidth() - 16.0f), 26.0f)
                           .withCentre ({ c.box.getCentreX(), c.box.getY() + (withText ? 12.0f : 9.0f) + 13.0f });
            c.textArea = withText ? c.box.withTop (c.button.getBottom() + 6.0f).withTrimmedBottom (8.0f).reduced (10.0f, 0.0f)
                                  : juce::Rectangle<float>();
        }
        else
        {
            c.textArea = c.box.reduced (12.0f, 6.0f);
        }

        if (! withText)
            c.line2.clear();

        return c;
    }

    juce::Rectangle<float> cardButtonArea() const
    {
        const auto c = card();
        return c.kind == CardKind::generate ? c.button : juce::Rectangle<float>();
    }

    void showHint (const juce::String& message)
    {
        hintText = message;
        hintUntil = juce::Time::getMillisecondCounter() + kHintMillis;
        repaint();
    }

    //==============================================================================
    // Painting
    void paint (juce::Graphics& g) override
    {
        paintCard (g);
        paintStrip (g);
        paintGrid (g);
        paintVelocityLane (g);
    }

    void paintCard (juce::Graphics& g) const
    {
        const auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setGradientFill (juce::ColourGradient::vertical (Col::panel.brighter (0.035f), r.getY(), Col::panel, r.getBottom()));
        g.fillRoundedRectangle (r, cb::ui::kCornerRadius);
        g.setColour (Col::outline.withAlpha (0.55f));
        g.drawRoundedRectangle (r, cb::ui::kCornerRadius, 1.0f);
    }

    // Chord names above the beats they cover, in their function colours; an accent dot after every edited slot.
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
            const bool edited = i < editedSlots.size() && editedSlots[i];
            g.setColour (colour.withAlpha (0.8f));
            g.fillRoundedRectangle (cell.withTop (cell.getBottom() - 2.5f), 1.25f);

            const auto textArea = cell.withTrimmedBottom (3.0f).withTrimmedLeft (3.0f);
            const float nameW = widthOf (nameFont, slotNames[(int) i]);
            const bool dotFits = edited && nameW + 12.0f <= textArea.getWidth();
            g.setFont (nameFont);
            g.setColour (colour.brighter (0.3f));
            g.drawText (slotNames[(int) i], dotFits ? textArea.withWidth (nameW + 2.0f) : textArea,
                        juce::Justification::centredLeft, true);

            if (edited)
            {
                const float dx = dotFits ? textArea.getX() + nameW + 6.0f : cell.getRight() - 5.0f;
                g.setColour (Col::accent);
                g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ dx, textArea.getCentreY() }));
            }
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

        const int hi = topNote();
        const int lo = lowestPaintedRow();

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

        // Long progressions: beat lines only while a beat is at least 7 px wide, below that bar lines only, and below
        // 2.5 px per beat every 4th bar (no dense comb fighting with the notes).
        const int lineEvery = pxPerBeat >= 7.0f ? 1 : (pxPerBeat >= 2.5f ? 4 : 16);

        for (int b = lineEvery; b < visibleBeats; b += lineEvery)
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
        paintOtherLayers (g);
        paintDrawPreview (g);
        paintNotes (g, lo, hi);
        paintParkedLabel (g);

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

        if (gesture == Gesture::marquee && ! marqueeRect.isEmpty())
        {
            g.setColour (Col::accent.withAlpha (0.10f));
            g.fillRect (marqueeRect);
            g.setColour (Col::accent.withAlpha (0.8f));
            g.drawRect (marqueeRect, 1.0f);
        }

        paintEmptyCard (g);
        paintPills (g);
    }

    // Calls fn (note) for every ghost note inside the visible rows / beats: each other instance's melody, repeated
    // every loop of that instance's progression (they all follow the host's position).
    template <typename Fn>
    void forEachVisibleGhost (Fn&& fn) const
    {
        if (! showGhosts)
            return;

        const int hi = topNote();
        const int lo = lowestPaintedRow();

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

    // This instance's other layers (outlines under the current layer), only while that layer is on.
    template <typename Fn>
    void forEachVisibleOtherLayerNote (Fn&& fn) const
    {
        if (! showGhosts)
            return;

        const int hi = topNote();
        const int lo = lowestPaintedRow();

        for (int i = 0; i < cb::numLayers; ++i)
        {
            if (i == cur() || ! layerOn[(size_t) i])
                continue;

            for (const auto& n : layerNotes[(size_t) i].notes)
                if (n.note >= lo && n.note <= hi && n.startBeat < (double) visibleBeats)
                    fn (kLayers[i], n);
        }
    }

    int countVisibleGhosts() const
    {
        int n = 0;
        forEachVisibleGhost ([&n] (const cb::MelodyNote&) { ++n; });
        return n;
    }

    int countVisibleOtherLayerNotes() const
    {
        int n = 0;
        forEachVisibleOtherLayerNote ([&n] (cb::Layer, const cb::MelodyNote&) { ++n; });
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

    // Narrow outlines (a dense layer over a long loop) that touch or overlap on the same row are merged into one run,
    // painted once: stacked 45% outlines would otherwise add up to solid bars louder than the current layer.
    void paintOtherLayers (juce::Graphics& g) const
    {
        constexpr float kNarrow = 6.0f;
        std::array<juce::Rectangle<float>, 128> runs;
        cb::Layer runLayer = cb::Layer::Melody;

        auto paintOne = [&g] (cb::Layer l, juce::Rectangle<float> r)
        {
            const auto colour = cb::ui::layerColour (l);
            g.setColour (colour.withAlpha (0.06f));
            g.fillRect (r);
            g.setColour (colour.withAlpha (0.45f));
            g.drawRect (r, 1.0f);
        };

        auto flushRuns = [&]
        {
            for (auto& run : runs)
            {
                if (! run.isEmpty())
                    paintOne (runLayer, run);

                run = {};
            }
        };

        forEachVisibleOtherLayerNote ([&] (cb::Layer l, const cb::MelodyNote& n)
        {
            if (l != runLayer)
            {
                flushRuns();
                runLayer = l;
            }

            const auto r = noteRect (n);

            if (r.getWidth() >= kNarrow)
            {
                paintOne (l, r);
                return;
            }

            auto& run = runs[(size_t) juce::jlimit (0, 127, n.note)];

            if (! run.isEmpty() && r.getX() <= run.getRight() + 1.0f && r.getRight() >= run.getX() - 1.0f)
            {
                run = run.getUnion (r);
                return;
            }

            if (! run.isEmpty())
                paintOne (l, run);

            run = r;
        });

        flushRuns();
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
        const auto& m = shown();
        const bool on = layerOn[(size_t) cur()];
        const bool isAuto = isGenerated (layer) && ! layerCustom[(size_t) cur()];
        const float layerAlpha = (isAuto && ! inEditingGesture() ? 0.7f : 1.0f) * (on ? 1.0f : 0.55f);
        const auto labelFont = cb::ui::font (juce::jlimit (8.5f, 10.0f, rowH * 0.72f), 600);
        const float gridLeft = gridArea.getX(), gridRight = gridArea.getRight();

        // Two passes: unselected notes, then the selected ones on top.
        for (int pass = 0; pass < 2; ++pass)
        {
            for (int i = 0; i < (int) m.notes.size(); ++i)
            {
                const auto& n = m.notes[(size_t) i];

                if (n.note < lo || n.note > hi)
                    continue;

                const bool selected = isSelected (i);

                if (selected != (pass == 1))
                    continue;

                const auto r = noteRect (n);

                if (r.getRight() < gridLeft || r.getX() > gridRight)
                    continue;

                const bool parked = total <= 0 || n.startBeat >= (double) total;
                const bool active = (inEditingGesture() && i == activeIndex) || (gesture == Gesture::none && i == hoverIndex);
                const bool sounding = isSounding (n, playBeat);
                const auto colour = noteColour (n);
                const float alpha = (0.55f + 0.45f * n.velocity) * (parked ? 0.45f : 1.0f) * layerAlpha;

                if (sounding)
                {
                    g.setColour (Col::accent.withAlpha (0.35f));
                    g.fillRoundedRectangle (r.expanded (2.0f), 4.0f);
                }

                if (r.getWidth() >= 6.0f)
                {
                    g.setColour (juce::Colours::black.withAlpha (0.28f * alpha));
                    g.fillRoundedRectangle (r.translated (0.0f, 1.0f), 2.5f);
                    g.setGradientFill (juce::ColourGradient::vertical (colour.brighter (active ? 0.5f : 0.32f).withMultipliedAlpha (alpha),
                                                                       r.getY(),
                                                                       colour.brighter (active ? 0.15f : 0.02f).withMultipliedAlpha (alpha),
                                                                       r.getBottom()));
                    g.fillRoundedRectangle (r, 2.5f);
                    g.setColour (colour.darker (0.7f).withAlpha (0.85f * alpha));
                    g.drawRoundedRectangle (r.reduced (0.5f), 2.5f, 1.0f);
                }
                else
                {
                    // Dense layers (a 1/16 arp over a long loop): flat fill, no shadow or gradient.
                    g.setColour (colour.brighter (active ? 0.4f : 0.2f).withMultipliedAlpha (alpha));
                    g.fillRect (r);
                }

                if (selected)
                {
                    // A dark gap ring, then the accent ring outside it: reads on every function colour (the tonic
                    // mint is close to the accent), not only by hue.
                    g.setColour (Col::background.withAlpha (0.95f));
                    g.drawRoundedRectangle (r.expanded (0.75f), 3.2f, 1.5f);
                    g.setColour (Col::accent);
                    g.drawRoundedRectangle (r.expanded (2.25f), 4.5f, 1.5f);
                    g.setColour (juce::Colours::white.withAlpha (0.75f));
                    g.fillRect (juce::Rectangle<float> (r.getX() + 2.0f, r.getY() + 1.0f, std::max (0.0f, r.getWidth() - 4.0f), 1.0f));
                }
                else if (active || sounding)
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
                    g.setColour ((colour.getPerceivedBrightness() > 0.55f ? Col::background : Col::text)
                                     .withAlpha (0.85f * alpha + 0.1f));
                    g.drawText (noteName (n.note, key), r.withTrimmedLeft (4.0f).withTrimmedRight (4.0f),
                                juce::Justification::centredLeft, true);
                }
            }
        }
    }

    // "N notes past the end" at the top-left of the hatched area.
    void paintParkedLabel (juce::Graphics& g) const
    {
        const int parked = parkedCount();

        if (parked <= 0 || total >= visibleBeats)
            return;

        const auto f = cb::ui::font (10.5f, 600);
        const juce::String text = juce::String (parked) + (parked == 1 ? " note past the end" : " notes past the end");
        const float x = std::round (beatToX (total)) + 6.0f;
        const float w = widthOf (f, text) + 12.0f;

        if (x + w > gridArea.getRight() - 4.0f)
            return;

        auto r = juce::Rectangle<float> (x, gridArea.getY() + 5.0f, w, 16.0f);
        const auto p = pills();

        if (r.intersects (p.hint) || r.intersects (p.selection) || r.intersects (p.off))
            r.setY (gridArea.getBottom() - 21.0f);

        g.setColour (Col::panel.withAlpha (0.85f));
        g.fillRoundedRectangle (r, 8.0f);
        g.setFont (f);
        g.setColour (Col::textDim);
        g.drawText (text, r, juce::Justification::centred, false);
    }

    void paintEmptyCard (juce::Graphics& g) const
    {
        const auto c = card();

        if (c.kind == CardKind::none)
            return;

        g.setColour (Col::panel.withAlpha (0.9f));
        g.fillRoundedRectangle (c.box, 8.0f);
        g.setColour (Col::outline.withAlpha (0.8f));
        g.drawRoundedRectangle (c.box.reduced (0.5f), 8.0f, 1.0f);

        const auto f2 = cb::ui::font (11.5f, 400);

        if (c.kind == CardKind::generate)
        {
            const auto b = c.button;
            const float alpha = c.enabled ? 1.0f : 0.4f;
            const bool over = c.enabled && cardButtonHover;
            const bool down = over && gesture == Gesture::cardButton;
            const float radius = b.getHeight() * 0.5f;

            g.setGradientFill (juce::ColourGradient::vertical (Col::panelRaised.withMultipliedAlpha (alpha), b.getY(),
                                                               Col::panelRaised.darker (0.08f).withMultipliedAlpha (alpha), b.getBottom()));
            g.fillRoundedRectangle (b, radius);
            g.setColour (Col::accent.withAlpha ((down ? 0.24f : (over ? 0.17f : 0.10f)) * alpha));
            g.fillRoundedRectangle (b, radius);
            g.setColour (Col::accent.withAlpha ((over ? 0.75f : 0.45f) * alpha));
            g.drawRoundedRectangle (b.reduced (0.5f), radius - 0.5f, 1.0f);

            const auto f1 = cardButtonFont();
            const float labelW = widthOf (f1, c.line1);
            const float contentW = 13.0f + 7.0f + labelW;
            const float x = std::round (b.getCentreX() - contentW * 0.5f);
            drawSparkle (g, juce::Rectangle<float> (x, b.getCentreY() - 6.5f, 13.0f, 13.0f), Col::accent.withMultipliedAlpha (alpha));
            g.setFont (f1);
            g.setColour (Col::text.withMultipliedAlpha (alpha));
            g.drawText (c.line1, juce::Rectangle<float> (x + 20.0f, b.getY(), labelW + 2.0f, b.getHeight()),
                        juce::Justification::centredLeft, false);

            if (c.line2.isNotEmpty())
            {
                g.setFont (f2);
                g.setColour (Col::textDim);
                g.drawFittedText (c.line2, c.textArea.toNearestInt(), juce::Justification::centred, 1, 0.8f);
            }
        }
        else
        {
            auto t = c.textArea;
            g.setFont (cb::ui::font (13.0f, 600));
            g.setColour (Col::text);

            if (c.line2.isNotEmpty())
            {
                g.drawFittedText (c.line1, t.removeFromTop (t.getHeight() * 0.52f).toNearestInt(), juce::Justification::centredBottom, 1, 0.8f);
                g.setFont (f2);
                g.setColour (Col::textDim);
                g.drawFittedText (c.line2, t.withTrimmedTop (3.0f).toNearestInt(), juce::Justification::centredTop, 1, 0.8f);
            }
            else
            {
                g.drawFittedText (c.line1, t.toNearestInt(), juce::Justification::centred, 1, 0.8f);
            }
        }
    }

    void paintPillBase (juce::Graphics& g, juce::Rectangle<float> r, bool over) const
    {
        const float radius = std::min (10.0f, r.getHeight() * 0.5f);
        g.setColour (juce::Colours::black.withAlpha (0.22f));
        g.fillRoundedRectangle (r.translated (0.0f, 1.0f), radius);
        g.setColour ((over ? Col::panelHover : Col::panel).withAlpha (0.92f));
        g.fillRoundedRectangle (r, radius);
        g.setColour (over ? Col::accent.withAlpha (0.6f) : Col::outline);
        g.drawRoundedRectangle (r.reduced (0.5f), radius - 0.5f, 1.0f);
    }

    void paintPills (juce::Graphics& g) const
    {
        const auto p = pills();
        const auto f = pillFont();

        if (! p.off.isEmpty())
        {
            const auto r = p.off;
            paintPillBase (g, r, hoverPill == 2);
            const auto colour = cb::ui::layerColour (layer);
            g.setColour (colour.withAlpha (0.9f));
            g.drawEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ r.getX() + 13.5f, r.getCentreY() }), 1.2f);
            const auto text = offPillText();
            const float w = widthOf (f, text);
            g.setFont (f);
            g.setColour (Col::textDim);
            g.drawText (text, juce::Rectangle<float> (r.getX() + 23.0f, r.getY(), w + 2.0f, r.getHeight()),
                        juce::Justification::centredLeft, false);
            g.setColour (Col::accent);
            g.drawText ("Turn on", juce::Rectangle<float> (r.getX() + 23.0f + w, r.getY(), r.getRight() - (r.getX() + 23.0f + w), r.getHeight()),
                        juce::Justification::centredLeft, false);
        }

        if (! p.selection.isEmpty())
        {
            const auto r = p.selection;
            paintPillBase (g, r, hoverPill == 1);
            const auto text = selectionPillText();
            const float w = widthOf (f, text);
            g.setFont (f);
            g.setColour (Col::text);
            g.drawText (text, juce::Rectangle<float> (r.getX() + 11.0f, r.getY(), w + 2.0f, r.getHeight()),
                        juce::Justification::centredLeft, false);
            drawEllipsisDots (g, { r.getX() + 11.0f + w + 7.0f, r.getCentreY() + 0.5f }, Col::accent);
        }

        if (! p.hint.isEmpty())
        {
            const auto r = p.hint;
            paintPillBase (g, r, false);
            g.setFont (cb::ui::font (11.5f, 400));
            g.setColour (Col::text);
            g.drawText (hintText, r, juce::Justification::centred, false);
        }
    }

    void paintVelocityLane (juce::Graphics& g) const
    {
        if (laneArea.isEmpty())
            return;

        const auto area = laneArea;
        g.setColour (Col::background.darker (0.06f));
        g.fillRoundedRectangle (area, 4.0f);
        g.setColour (Col::outline.withAlpha (0.8f));
        g.drawRoundedRectangle (area.reduced (0.5f), 4.0f, 1.0f);

        const auto inner = laneInner();
        g.setColour (Col::outline.withAlpha (0.55f));
        g.fillRect (juce::Rectangle<float> (area.getX() + 1.0f, std::round (velocityToY (0.5f)), area.getWidth() - 2.0f, 1.0f));

        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (area.toNearestInt());

        const auto& m = shown();
        const bool on = layerOn[(size_t) cur()];
        const bool isAuto = isGenerated (layer) && ! layerCustom[(size_t) cur()];
        const float layerAlpha = (isAuto ? 0.7f : 1.0f) * (on ? 1.0f : 0.55f);

        // stalk == false: the dot only (redrawn over the label).
        auto paintStalk = [&] (const cb::MelodyNote& n, bool selected, bool stalk)
        {
            const float x = stalkX (n);
            const float y = velocityToY (n.velocity);

            if (selected)
            {
                // Wider stalk and a white-ringed dot: tells a selected stalk from a tonic chord tone's (mint).
                if (stalk)
                {
                    g.setColour (Col::accent.withMultipliedAlpha (0.9f));
                    g.fillRect (juce::Rectangle<float> (x - 1.5f, y, 3.0f, inner.getBottom() - y));
                }

                g.setColour (Col::accent);
                g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ x, y }));
                g.setColour (juce::Colours::white.withAlpha (0.9f));
                g.drawEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ x, y }), 1.5f);
                return;
            }

            const auto colour = noteColour (n).withMultipliedAlpha (layerAlpha);

            if (stalk)
            {
                g.setColour (colour.withMultipliedAlpha (0.85f));
                g.fillRect (juce::Rectangle<float> (x - 1.0f, y, 2.0f, inner.getBottom() - y));
            }

            g.setColour (colour);
            g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ x, y }));
        };

        for (int pass = 0; pass < 2; ++pass)
            for (int i = 0; i < (int) m.notes.size(); ++i)
                if (const bool selected = isSelected (i); selected == (pass == 1) && m.notes[(size_t) i].startBeat < (double) visibleBeats)
                    paintStalk (m.notes[(size_t) i], selected, true);

        // The label last, on a small backing, so dense layers never cover it; it starts right of the beat-0 stalk,
        // and a few dots under it (sparse notes) are drawn again on top, so they are never hidden either.
        {
            const auto labelFont = cb::ui::font (9.5f, 600).withExtraKerningFactor (0.08f);
            const auto label = juce::Rectangle<float> (area.getX() + 8.0f, area.getY() + 2.0f, widthOf (labelFont, "VEL") + 6.0f, 11.0f);
            g.setColour (Col::background.darker (0.06f).withAlpha (0.85f));
            g.fillRoundedRectangle (label, 3.0f);
            g.setFont (labelFont);
            g.setColour (Col::textFaint);
            g.drawText ("VEL", label.withTrimmedLeft (2.0f), juce::Justification::centredLeft, false);

            std::vector<int> under;

            for (int i = 0; i < (int) m.notes.size() && under.size() <= 2; ++i)
                if (label.expanded (4.0f).contains (stalkX (m.notes[(size_t) i]), velocityToY (m.notes[(size_t) i].velocity)))
                    under.push_back (i);

            if (under.size() <= 2)
                for (int i : under)
                    paintStalk (m.notes[(size_t) i], isSelected (i), false);
        }

        if (gesture == Gesture::velocity && activeIndex >= 0 && activeIndex < (int) m.notes.size())
        {
            const auto& n = m.notes[(size_t) activeIndex];
            const float x = stalkX (n);
            const auto text = juce::String (juce::jlimit (1, 127, juce::roundToInt (n.velocity * 127.0f)));
            const auto f = cb::ui::font (10.5f, 600);
            const float w = widthOf (f, text) + 10.0f;
            const float bx = x + 6.0f + w <= area.getRight() - 2.0f ? x + 6.0f : x - 6.0f - w;
            const float by = juce::jlimit (area.getY() + 1.0f, area.getBottom() - 15.0f, velocityToY (n.velocity) - 7.0f);
            const auto bubble = juce::Rectangle<float> (bx, by, w, 14.0f);
            g.setColour (Col::panelHover);
            g.fillRoundedRectangle (bubble, 7.0f);
            g.setColour (Col::accent.withAlpha (0.7f));
            g.drawRoundedRectangle (bubble.reduced (0.5f), 6.5f, 1.0f);
            g.setFont (f);
            g.setColour (Col::text);
            g.drawText (text, bubble, juce::Justification::centred, false);
        }
    }

    //==============================================================================
    // Editing

    // A gesture that started on the grid / lane and works on `working` (shown instead of the layer's notes).
    bool inGridGesture() const
    {
        return gesture == Gesture::move || gesture == Gesture::resize || gesture == Gesture::erase || gesture == Gesture::pendingCtrl
            || gesture == Gesture::marquee || gesture == Gesture::duplicate || gesture == Gesture::velocity;
    }

    bool inEditingGesture() const
    {
        return gesture == Gesture::move || gesture == Gesture::resize || gesture == Gesture::erase || gesture == Gesture::duplicate
            || gesture == Gesture::velocity;
    }

    struct Hit
    {
        int index = -1;
        bool edge = false;
    };

    Hit hitTest (const cb::Melody& m, juce::Point<float> pos) const
    {
        if (! gridArea.contains (pos))
            return {};

        const int hi = topNote(), lo = lowestPaintedRow();

        for (int i = (int) m.notes.size(); --i >= 0;)   // drawn last = on top
        {
            const auto& n = m.notes[(size_t) i];

            if (n.note < lo || n.note > hi)
                continue;

            const auto r = noteRect (n);

            if (r.expanded (0.0f, 0.5f).contains (pos))
            {
                // Narrow notes (dense layers) are moved, not resized: their whole body would be the edge.
                const float edgeW = juce::jlimit (3.0f, 7.0f, r.getWidth() * 0.3f);
                return { i, r.getWidth() >= 8.0f && pos.x >= r.getRight() - edgeW };
            }
        }

        return {};
    }

    std::optional<cb::Chord> chordAt (double beat) const { return cb::chordAtBeat (progression, beat); }

    double gridBeat (double beat, bool fine, bool roundToNearest) const
    {
        return cb::snapBeatToGrid (beat, fine ? cb::kMelodyTick : gridStep(), roundToNearest);
    }

    double roundDelta (double delta, bool fine) const
    {
        const double step = fine ? cb::kMelodyTick : gridStep();
        return step > 0.0 ? std::round (delta / step) * step : delta;
    }

    // Latest start a move / duplicate may give a note: one grid step before the end of the visible beats.
    double editMaxBeat() const
    {
        return (double) visibleBeats - std::max (gridStep(), cb::kMinMelodyNoteBeats) + cb::kMinMelodyNoteBeats;
    }

    cb::NoteEditContext editContext() const
    {
        cb::NoteEditContext ctx;
        ctx.progression = &progression;
        ctx.key = key;
        ctx.snap = view().snap;
        ctx.limits = limits();
        ctx.maxBeat = editMaxBeat();
        return ctx;
    }

    // The group clamp of cb::moveNotes, done here too so the anchor's destination is known (for the audition).
    std::pair<double, int> clampGroupDelta (const cb::Melody& m, const std::vector<int>& notesToMove, double deltaBeats, int rows) const
    {
        const auto sel = cb::validSelection (m, notesToMove);

        if (sel.empty())
            return { 0.0, 0 };

        double minStart = 1.0e9, maxStart = -1.0e9;
        int minPitch = 1000, maxPitch = -1000;

        for (int i : sel)
        {
            const auto& n = m.notes[(size_t) i];
            minStart = std::min (minStart, n.startBeat);
            maxStart = std::max (maxStart, n.startBeat);
            minPitch = std::min (minPitch, n.note);
            maxPitch = std::max (maxPitch, n.note);
        }

        const auto lim = limits();
        const double maxBeat = std::min (editMaxBeat(), cb::kMaxMelodyBeat);
        deltaBeats = juce::jlimit (std::min (-minStart, 0.0), std::max (maxBeat - cb::kMinMelodyNoteBeats - maxStart, 0.0), deltaBeats);
        rows = juce::jlimit (std::min (lim.lowest - minPitch, 0), std::max (lim.highest - maxPitch, 0), rows);
        return { deltaBeats, rows };
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
        n.lengthBeats = newNoteLength();
        n.note = cb::snapMelodyPitch (yToNote (pos.y), view().snap, key, chordAt (beat), limits());
        n.velocity = newNoteVelocity();
        return n;
    }

    void audition (int note)
    {
        if (note == auditionedNote && auditioning)
            return;

        processor.startPreviewLayerNote (layer, note);
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

    // The previous gesture never got its mouse-up (e.g. a window grabbed the mouse): its edits are dropped.
    void dropGesture()
    {
        if (gesture == Gesture::none)
            return;

        gesture = Gesture::none;
        activeIndex = -1;
        marqueeRect = {};
        stopAudition();
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint();
    }

    void startGridGesture (Gesture g)
    {
        gesture = g;
        snapshot = notes();
        base = snapshot;
        working = snapshot;
        workingSel = selection;
    }

    // Adding `adding` notes would pass the layer's note cap (sanitising would then silently evict other notes):
    // refused with a hint.
    bool wouldExceedCap (int adding) const { return (int) notes().notes.size() + adding > limits().maxNotes; }

    bool refuseOverCap (int adding)
    {
        if (! wouldExceedCap (adding))
            return false;

        showHint (layerTitle (layer) + " is full (" + juce::String (limits().maxNotes) + " notes)");
        return true;
    }

    // 1 = the selection pill, 2 = the off pill. The transient hint has no action: clicks pass through it to the grid.
    int pillAt (juce::Point<float> pos) const
    {
        const auto p = pills();

        if (p.selection.contains (pos)) return 1;
        if (p.off.contains (pos))       return 2;
        return 0;
    }

    int stalkAt (juce::Point<float> pos) const
    {
        if (! laneArea.contains (pos))
            return -1;

        const auto& m = notes();
        int best = -1;
        float bestDy = 1.0e9f;

        for (int i = 0; i < (int) m.notes.size(); ++i)
        {
            const auto& n = m.notes[(size_t) i];
            const float x = stalkX (n);

            if (std::abs (x - pos.x) <= kStalkReach)
            {
                const float dy = std::abs (velocityToY (n.velocity) - pos.y);

                if (dy < bestDy)
                {
                    bestDy = dy;
                    best = i;
                }
            }
        }

        return best;
    }

    juce::Rectangle<int> screenAreaOf (juce::Rectangle<float> r) const { return localAreaToGlobal (r.getSmallestIntegerContainer()); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dropGesture();
        const auto pos = e.position;
        downPos = pos;
        dragged = false;
        hoverDraw.reset();
        syncLayers (true);   // the notes may have changed since the last poll (undo, automation, another view)

        if (e.mods.isPopupMenu())
        {
            // Right-click never acts as a left click: pills ignore it, the strip and the lane open their menus.
            if (pillAt (pos) != 0)
            {
                gesture = Gesture::swallow;
                return;
            }

            if (stripArea.contains (pos))
            {
                gesture = Gesture::swallow;

                if (progression.slotAtBeat (xToBeat (pos.x)) >= 0)
                    showMenu (MenuSection::Chord, xToBeat (pos.x), std::nullopt, {});
                return;
            }

            if (laneArea.contains (pos))
            {
                gesture = Gesture::swallow;
                showMenu (MenuSection::Velocity, -1.0, std::nullopt, {});
                return;
            }

            if (! gridArea.contains (pos))
                return;

            startGridGesture (Gesture::erase);
            erasedAny = false;
            pressedOnNote = hitTest (working, pos).index >= 0;
            lastErasePos = pos;
            eraseWanted = notesAt (working, workingSel);
            eraseAt (pos);
            return;
        }

        if (const int pill = pillAt (pos); pill != 0)
        {
            gesture = Gesture::swallow;
            const auto p = pills();

            if (pill == 1)
                showMenu (MenuSection::Selection, -1.0, std::nullopt, screenAreaOf (p.selection));
            else if (pill == 2)
                processor.setLayerOn (layer, true);

            syncLayers (false);
            rollToolbar->refresh();
            repaint();
            return;
        }

        if (cardButtonArea().contains (pos))
        {
            gesture = Gesture::cardButton;
            repaint();
            return;
        }

        if (laneArea.contains (pos))
        {
            const int stalk = stalkAt (pos);

            if (stalk < 0)
            {
                gesture = Gesture::swallow;
                return;
            }

            startGridGesture (Gesture::velocity);
            activeIndex = stalk;
            velocityOriginal = base.notes[(size_t) stalk].velocity;
            velocityGroup = std::binary_search (selection.begin(), selection.end(), stalk);
            setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
            return;
        }

        if (! gridArea.contains (pos))
        {
            gesture = Gesture::swallow;
            return;
        }

        const bool fine = e.mods.isAltDown();
        const auto hit = hitTest (notes(), pos);

        if (e.mods.isCommandDown())
        {
            startGridGesture (Gesture::pendingCtrl);
            pendingHit = hit.index;
            marqueeAdd = e.mods.isShiftDown();
            marqueeBase = selection;
            return;
        }

        if (hit.index >= 0)
        {
            const bool wasSelected = std::binary_search (selection.begin(), selection.end(), hit.index);
            rememberNote (notes().notes[(size_t) hit.index]);

            if (e.mods.isShiftDown())
            {
                const auto toCopy = wasSelected ? selection : std::vector<int> { hit.index };

                if (refuseOverCap ((int) toCopy.size()))
                {
                    gesture = Gesture::swallow;
                    return;
                }

                startGridGesture (Gesture::duplicate);
                group = toCopy;
                duplicateSelects = wasSelected;
                anchorNote = base.notes[(size_t) hit.index];
                activeIndex = -1;
                setMouseCursor (juce::MouseCursor::CopyingCursor);
                repaint();
                return;
            }

            startGridGesture (hit.edge ? Gesture::resize : Gesture::move);

            if (wasSelected)
            {
                group = selection;
                groupSelected = true;
            }
            else
            {
                // Grabbing an unselected note: the selection goes and the note does not stay selected.
                selection.clear();
                workingSel.clear();
                group = { hit.index };
                groupSelected = false;
            }

            anchorNote = base.notes[(size_t) hit.index];
            activeIndex = hit.index;

            if (gesture == Gesture::move)
                audition (anchorNote.note);
        }
        else if (! selection.empty())
        {
            // A click on empty space with a selection only deselects.
            selection.clear();
            gesture = Gesture::swallow;
            repaint();
            return;
        }
        else if (auto n = noteToDraw (pos, fine))
        {
            if (refuseOverCap (1))
            {
                gesture = Gesture::swallow;
                return;
            }

            startGridGesture (Gesture::move);
            auto drawn = base;
            drawn.notes.push_back (*n);
            base = cb::sanitiseMelody (drawn, limits());
            working = base;
            workingSel.clear();
            const auto found = cb::findNotes (base, { *n });
            group = found;
            groupSelected = false;
            anchorNote = *n;
            activeIndex = found.empty() ? -1 : found.front();
            audition (n->note);
        }
        else
        {
            gesture = Gesture::swallow;
            return;
        }

        setMouseCursor (gesture == Gesture::resize ? juce::MouseCursor::LeftRightResizeCursor
                                                   : juce::MouseCursor::DraggingHandCursor);
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const auto pos = e.position;
        lastMouse = pos;
        dragged = dragged || pos.getDistanceFrom (downPos) > kDragThreshold;
        const bool fine = e.mods.isAltDown();

        switch (gesture)
        {
            case Gesture::erase:
            {
                // Sample the path so a fast sweep does not skip short notes.
                const auto from = lastErasePos;
                const int steps = std::max (1, (int) std::ceil (from.getDistanceFrom (pos) / 3.0f));

                for (int s = 1; s <= steps; ++s)
                    eraseAt (from + (pos - from) * ((float) s / (float) steps));

                lastErasePos = pos;
                return;
            }

            case Gesture::move:      dragMove (pos, fine); return;
            case Gesture::resize:    dragResize (pos, fine); return;
            case Gesture::duplicate: dragDuplicate (pos, fine); return;
            case Gesture::velocity:
                if (pos.getDistanceFrom (downPos) >= 1.0f)
                    applyVelocityDrag (pos);
                return;

            case Gesture::pendingCtrl:
                if (! dragged)
                    return;

                gesture = Gesture::marquee;
                setMouseCursor (juce::MouseCursor::CrosshairCursor);
                updateMarquee (pos);
                return;

            case Gesture::marquee:   updateMarquee (pos); return;

            case Gesture::cardButton:
            {
                const bool over = cardButtonArea().contains (pos);

                if (over != cardButtonHover)
                {
                    cardButtonHover = over;
                    repaint();
                }
                return;
            }

            case Gesture::none:
            case Gesture::swallow:
            default:
                return;
        }
    }

    void dragMove (juce::Point<float> pos, bool fine)
    {
        const auto [delta, rows] = clampGroupDelta (base, group, roundDelta (xToBeat (pos.x) - xToBeat (downPos.x), fine),
                                                    yToNote (pos.y) - yToNote (downPos.y));
        const auto r = cb::moveNotes (base, group, delta, rows, editContext());

        if (r.notes == working)
            return;

        working = r.notes;
        workingSel = groupSelected ? r.selection : std::vector<int>();

        // The anchor's destination (for the highlight and the audition): the group clamp is already applied, so
        // moveNotes moves it by exactly (delta, rows) and re-snaps it at its new start.
        auto anchor = anchorNote;

        if (delta != 0.0 || rows != 0)
        {
            anchor.startBeat = cb::snapBeatToGrid (anchorNote.startBeat + delta, cb::kMelodyTick, true);
            anchor.note += rows;

            if (view().snap != cb::MelodySnap::Off)
                anchor.note = cb::snapMelodyPitch (anchor.note, view().snap, key, chordAt (anchor.startBeat), limits());
        }

        const auto found = cb::findNotes (working, { anchor });
        activeIndex = found.empty() ? -1 : found.front();

        if (activeIndex >= 0)
            audition (working.notes[(size_t) activeIndex].note);

        repaint();
    }

    void dragResize (juce::Point<float> pos, bool fine)
    {
        const double anchorEnd = anchorNote.startBeat + anchorNote.lengthBeats;
        const double pressEnd = gridBeat (xToBeat (downPos.x), fine, true);
        const double mouseEnd = gridBeat (xToBeat (pos.x), fine, true);

        // Until the mouse reaches another grid line than the one nearest to the press (jitter, a vertical wobble,
        // or dragged back), nothing changes: no note gets lengthened to the minimum, so the click commits nothing.
        if (std::abs (pos.x - downPos.x) < 1.0f || std::abs (mouseEnd - pressEnd) < 1.0e-9)
        {
            if (working != base)
            {
                working = base;
                workingSel = groupSelected ? selection : std::vector<int>();
                const auto found = cb::findNotes (working, { anchorNote });
                activeIndex = found.empty() ? -1 : found.front();
                repaint();
            }
            return;
        }

        // At least one grid step (Alt: one tick), but notes that are already shorter than that (a 1/16 arp at gate
        // 0.9 on the 1/16 grid) are never lengthened by the clamp: they are resized in a second pass whose minimum
        // is the shortest of them. Resizing keeps every (pitch, start), so the notes are found again by those.
        const double delta = mouseEnd - anchorEnd;
        const double gridMin = fine ? cb::kMinMelodyNoteBeats : std::max (gridStep(), cb::kMinMelodyNoteBeats);
        std::vector<cb::MelodyNote> longOnes, shortOnes;
        double shortest = gridMin;

        for (int i : cb::validSelection (base, group))
        {
            const auto& n = base.notes[(size_t) i];

            if (n.lengthBeats < gridMin - 1.0e-9)
            {
                shortOnes.push_back (n);
                shortest = std::min (shortest, n.lengthBeats);
            }
            else
            {
                longOnes.push_back (n);
            }
        }

        auto r = cb::resizeNotes (base, cb::findNotes (base, longOnes), delta, gridMin, limits());

        if (! shortOnes.empty())
            r = cb::resizeNotes (r.notes, cb::findNotes (r.notes, shortOnes), delta,
                                 std::max (cb::kMinMelodyNoteBeats, shortest), limits());

        r.selection = cb::findNotes (r.notes, notesAt (base, group));

        if (r.notes == working)
            return;

        working = r.notes;
        workingSel = groupSelected ? r.selection : std::vector<int>();
        const auto found = cb::findNotes (working, { anchorNote });
        activeIndex = found.empty() ? -1 : found.front();
        repaint();
    }

    void dragDuplicate (juce::Point<float> pos, bool fine)
    {
        const auto [delta, rows] = clampGroupDelta (base, group, roundDelta (xToBeat (pos.x) - xToBeat (downPos.x), fine),
                                                    yToNote (pos.y) - yToNote (downPos.y));
        // Back where it started: nothing changes and the selection stays as it was at mouse-down.
        cb::NoteEditResult r { base, selection };

        if (delta != 0.0 || rows != 0)
        {
            // The copies are shifted on their own and only merged at the end: a straight vertical drag (delta 0)
            // must not let a copy collapse into its original before it is moved.
            cb::Melody copies;

            for (int i : cb::validSelection (base, group))
            {
                auto c = base.notes[(size_t) i];
                c.startBeat += delta;
                copies.notes.push_back (c);
            }

            copies = cb::sanitiseMelody (copies, limits());

            if (rows != 0)
                copies = cb::moveNotes (copies, cb::allNoteIndices (copies), 0.0, rows, editContext()).notes;

            auto merged = base;
            merged.notes.insert (merged.notes.end(), copies.notes.begin(), copies.notes.end());
            r.notes = cb::sanitiseMelody (merged, limits());
            r.selection = cb::findNotes (r.notes, copies.notes);
        }

        if (r.notes == working && r.selection == workingSel)
            return;

        working = r.notes;
        workingSel = r.selection;
        activeIndex = -1;
        repaint();
    }

    void applyVelocityDrag (juce::Point<float> pos)
    {
        if (activeIndex < 0 || activeIndex >= (int) base.notes.size())
            return;

        const float v = yToVelocity (pos.y);
        const auto r = velocityGroup ? cb::offsetNoteVelocity (base, selection, v - velocityOriginal, limits())
                                     : cb::setNoteVelocity (base, { activeIndex }, v, limits());
        working = r.notes;
        workingSel = selection;   // velocity never moves a note: indices stay valid
        repaint();
    }

    void updateMarquee (juce::Point<float> pos)
    {
        marqueeRect = juce::Rectangle<float> (downPos, pos).getIntersection (gridArea);
        auto found = cb::notesInArea (working, xToBeat (marqueeRect.getX()), xToBeat (marqueeRect.getRight()),
                                      yToNote (marqueeRect.getBottom() - 0.01f), yToNote (marqueeRect.getY()));

        if (marqueeAdd)
        {
            found.insert (found.end(), marqueeBase.begin(), marqueeBase.end());
            found = cb::validSelection (working, found);
        }

        workingSel = found;
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (gesture == Gesture::none)
            return;

        const auto finished = gesture;
        gesture = Gesture::none;
        stopAudition();
        marqueeRect = {};

        switch (finished)
        {
            case Gesture::erase:
                if (! erasedAny && ! pressedOnNote && ! dragged)
                {
                    syncLayers (false);
                    showMenu (MenuSection::All, xToBeat (downPos.x), std::nullopt, {});
                }
                else
                {
                    commitEdit (working, cb::findNotes (working, eraseWanted), snapshot);
                }
                break;

            case Gesture::move:
                commitEdit (working, workingSel, snapshot);
                break;

            case Gesture::resize:
            {
                const auto found = cb::findNotes (working, { anchorNote });

                if (! found.empty())
                    view().lastLength = working.notes[(size_t) found.front()].lengthBeats;

                commitEdit (working, workingSel, snapshot);
                break;
            }

            case Gesture::duplicate:
                // A cancelled duplicate (dragged back) keeps the selection exactly as it was.
                commitEdit (working, working == base ? selection : (duplicateSelects ? workingSel : std::vector<int>()), snapshot);
                break;

            case Gesture::velocity:
                if (activeIndex >= 0 && activeIndex < (int) working.notes.size())
                    view().lastVelocity = working.notes[(size_t) activeIndex].velocity;

                commitEdit (working, workingSel, snapshot);
                break;

            case Gesture::pendingCtrl:
            case Gesture::marquee:
            {
                auto sel = workingSel;

                if (finished == Gesture::pendingCtrl && pendingHit >= 0)
                {
                    const auto it = std::lower_bound (sel.begin(), sel.end(), pendingHit);

                    if (it != sel.end() && *it == pendingHit)
                        sel.erase (it);
                    else
                        sel.insert (it, pendingHit);
                }

                // No notes change. If something outside changed them meanwhile, the selection goes (2.2).
                const bool changedOutside = processor.getLayerNotes (layer) != snapshot;
                syncLayers (false);
                selection = changedOutside ? std::vector<int>() : cb::validSelection (notes(), sel);
                break;
            }

            case Gesture::cardButton:
                cardButtonHover = cardButtonArea().contains (e.position);

                if (cardButtonHover && ! progression.empty())
                {
                    rollToolbar->generateNewMelody();
                    syncAll();
                    centreOnNotes();
                }
                break;

            case Gesture::none:
            case Gesture::swallow:
            default:
                break;
        }

        activeIndex = -1;
        pendingHit = -1;
        updateHover (e.position, e.mods);
        repaint();
    }

    void eraseAt (juce::Point<float> pos)
    {
        const auto hit = hitTest (working, pos);

        if (hit.index < 0)
            return;

        working = cb::deleteNotes (working, { hit.index }, limits()).notes;
        workingSel = cb::findNotes (working, eraseWanted);
        erasedAny = true;
        repaint();
    }

    void mouseMove (const juce::MouseEvent& e) override { updateHover (e.position, e.mods); }

    void mouseExit (const juce::MouseEvent&) override
    {
        hoverIndex = -1;
        hoverRow = -1;
        hoverEdge = false;
        hoverPill = 0;
        cardButtonHover = false;
        hoverDraw.reset();
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint();
    }

    void updateHover (juce::Point<float> pos, juce::ModifierKeys mods)
    {
        lastMouse = pos;
        const int pill = pillAt (pos);
        const bool overButton = pill == 0 && cardButtonArea().contains (pos);
        const bool inGrid = pill == 0 && ! overButton && gridArea.contains (pos);
        const auto hit = inGrid ? hitTest (notes(), pos) : Hit {};
        const int row = inGrid ? yToNote (pos.y) : -1;
        std::optional<cb::MelodyNote> draw;

        if (inGrid && hit.index < 0 && selection.empty() && ! mods.isCommandDown())
            draw = noteToDraw (pos, mods.isAltDown());

        const bool drawChanged = draw.has_value() != hoverDraw.has_value() || (draw.has_value() && ! (*draw == *hoverDraw));

        if (hit.index != hoverIndex || hit.edge != hoverEdge || row != hoverRow || drawChanged || pill != hoverPill
            || overButton != cardButtonHover)
        {
            hoverIndex = hit.index;
            hoverEdge = hit.edge;
            hoverRow = row;
            hoverDraw = draw;
            hoverPill = pill;
            cardButtonHover = overButton;
            repaint (gridArea.expanded (2.0f).getSmallestIntegerContainer());
        }

        juce::MouseCursor newCursor = juce::MouseCursor::NormalCursor;

        if ((pill == 1 || pill == 2) || overButton)
            newCursor = juce::MouseCursor::PointingHandCursor;
        else if (laneArea.contains (pos) && stalkAt (pos) >= 0)
            newCursor = juce::MouseCursor::UpDownResizeCursor;
        else if (hit.index >= 0)
            newCursor = mods.isShiftDown() ? juce::MouseCursor::CopyingCursor
                                        : (hit.edge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::DraggingHandCursor);

        setMouseCursor (newCursor);
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
            scrollTo (topNote() + rows);
            updateHover (e.position, e.mods);
        }
    }

    juce::String getTooltip() override
    {
        const auto pos = lastMouse;
        const int pill = pillAt (pos);

        if (pill == 1)
            return juce::String ((int) selection.size())
                 + " notes selected. Click for Copy, Duplicate, Velocity, Transpose and more; click empty space to deselect.";

        if (pill == 2)
            return "The " + layerLower (layer) + " is muted: it is shown but does not play or send MIDI (channel "
                 + juce::String (cb::layerMidiChannel (layer)) + "). Click to turn it on.";

        if (cardButtonArea().contains (pos))
        {
            if (progression.empty())
                return "Add chords first";

            const auto style = rollToolbar->getGenerateSettings().style;
            return juce::String ("Generate a new melody over the chords (") + cb::melodyStyleName (style)
                 + "). Every click gives a new one; Undo brings the last one back.";
        }

        if (stripArea.contains (pos))
        {
            const int slot = progression.slotAtBeat (xToBeat (pos.x));

            if (slot >= 0 && slot < (int) editedSlots.size() && editedSlots[(size_t) slot])
                return "Edited: these notes stay as you made them. Right-click: Reset this chord";

            return {};
        }

        if (laneArea.contains (pos))
            return "Velocity: drag a stalk up or down (selected notes change together). Right-click for presets.";

        return {};
    }

    //==============================================================================
    // Commands and menus (final-spec 7.8)
    juce::String layerStateText (cb::Layer l) const
    {
        if (l == cb::Layer::Melody)
            return "your lead line";

        if (! processor.isLayerCustom (l))
            return "auto, follows the chords";

        const auto edited = processor.getEditedSlots (l);
        const auto n = (int) std::count (edited.begin(), edited.end(), true);
        return "edited " + juce::String (n) + "/" + juce::String ((int) edited.size());
    }

    bool slotEdited (cb::Layer l, double atBeat) const
    {
        const int slot = atBeat >= 0.0 ? progression.slotAtBeat (atBeat) : -1;

        if (slot < 0 || ! isGenerated (l) || ! processor.isLayerCustom (l))
            return false;

        const auto edited = processor.getEditedSlots (l);
        return slot < (int) edited.size() && edited[(size_t) slot];
    }

    // Performs (or, dryRun, only checks) a command. Note commands act on the current layer's selection, or on all its
    // notes when nothing is selected; layer commands act on `target`.
    bool perform (cb::Layer target, Command command, double atBeat, bool dryRun)
    {
        if (! dryRun)
        {
            dropGesture();
            syncLayers (true);
        }

        switch (command)
        {
            case Command::ResetLayer:
                if (! isGenerated (target) || ! processor.isLayerCustom (target))
                    return false;
                if (! dryRun)
                    processor.resetLayer (target);
                return finishLayerCommand (dryRun);

            case Command::ResetChord:
                if (! slotEdited (target, atBeat))
                    return false;
                if (! dryRun)
                    processor.resetLayer (target, progression.slotAtBeat (atBeat));
                return finishLayerCommand (dryRun);

            case Command::ClearMelody:
                if (processor.getMelody().empty())
                    return false;
                if (! dryRun)
                    processor.clearMelody();
                return finishLayerCommand (dryRun);

            case Command::ToggleLayerOn:
                if (! dryRun)
                    processor.setLayerOn (target, ! processor.isLayerOn (target));
                return finishLayerCommand (dryRun);

            case Command::ToggleVelocityLane:
                if (! dryRun)
                    setVelocityLane (! velLane);
                return true;

            case Command::ToggleOtherLayers:
                if (! dryRun)
                    setOtherLayersShown (! showGhosts);
                return true;

            case Command::SelectAll:
            case Command::SelectNone:
            case Command::Copy:
            case Command::Cut:
            case Command::Paste:
            case Command::PasteHere:
            case Command::Duplicate:
            case Command::Delete:
            case Command::Quantize:
            case Command::Legato:
            case Command::VelocitySoft:
            case Command::VelocityMedium:
            case Command::VelocityDefault:
            case Command::VelocityHard:
            case Command::VelocityUp:
            case Command::VelocityDown:
            case Command::StepUp:
            case Command::StepDown:
            case Command::SemitoneUp:
            case Command::SemitoneDown:
            case Command::OctaveUp:
            case Command::OctaveDown:
            default:
                break;
        }

        return performNoteCommand (command, atBeat, dryRun);
    }

    bool finishLayerCommand (bool dryRun)
    {
        if (! dryRun)
        {
            syncLayers (true);
            rollToolbar->refresh();
            repaint();
        }

        return true;
    }

    bool performNoteCommand (Command command, double atBeat, bool dryRun)
    {
        const auto& m = notes();
        const auto sel = cb::validSelection (m, selection);
        const bool hadSelection = ! sel.empty();
        const auto targets = hadSelection ? sel : cb::allNoteIndices (m);
        const auto lim = limits();
        const double grid = gridStep();

        cb::NoteEditResult r { m, sel };
        bool keepSelection = true;   // re-select what the operation returns only when something was selected

        switch (command)
        {
            case Command::SelectAll:
                if (m.empty() || sel.size() == m.notes.size())
                    return false;
                if (! dryRun)
                {
                    selection = cb::allNoteIndices (m);
                    repaint();
                }
                return true;

            case Command::SelectNone:
                if (sel.empty())
                    return false;
                if (! dryRun)
                {
                    selection.clear();
                    repaint();
                }
                return true;

            case Command::Copy:
                if (targets.empty())
                    return false;
                if (! dryRun)
                    clipboard() = cb::copyNotes (m, targets, grid);
                return true;

            case Command::Cut:
                if (targets.empty())
                    return false;
                if (! dryRun)
                    clipboard() = cb::copyNotes (m, targets, grid);
                r = cb::deleteNotes (m, targets, lim);
                break;

            case Command::Paste:
            case Command::PasteHere:
            {
                if (clipboard().empty() || (command == Command::PasteHere && atBeat < 0.0)
                    || wouldExceedCap ((int) clipboard().notes.notes.size()))
                    return false;

                const double at = command == Command::Paste ? clipboard().sourceBeat
                                                            : std::max (0.0, cb::snapBeatToGrid (atBeat, grid, false));
                r = cb::pasteNotes (m, clipboard(), at, lim);
                keepSelection = false;   // the pasted notes become the selection
                break;
            }

            case Command::Duplicate:
                if (targets.empty() || wouldExceedCap ((int) targets.size()))
                    return false;
                r = cb::duplicateNotes (m, targets, cb::defaultDuplicateOffset (m, targets, grid), lim);
                keepSelection = false;   // the copies become the selection
                break;

            case Command::Delete:
                if (targets.empty())
                    return false;
                r = cb::deleteNotes (m, targets, lim);
                break;

            case Command::Quantize:      r = cb::quantizeNotes (m, targets, grid, lim); break;
            case Command::Legato:        r = cb::legatoNotes (m, targets, lim); break;
            case Command::VelocitySoft:    r = cb::setNoteVelocity (m, targets, 0.40f, lim); break;
            case Command::VelocityMedium:  r = cb::setNoteVelocity (m, targets, 0.65f, lim); break;
            case Command::VelocityDefault: r = cb::setNoteVelocity (m, targets, 0.80f, lim); break;
            case Command::VelocityHard:    r = cb::setNoteVelocity (m, targets, 1.00f, lim); break;
            case Command::VelocityUp:      r = cb::offsetNoteVelocity (m, targets, 0.10f, lim); break;
            case Command::VelocityDown:    r = cb::offsetNoteVelocity (m, targets, -0.10f, lim); break;
            case Command::StepUp:        r = cb::transposeNotesInScale (m, targets, 1, key, lim); break;
            case Command::StepDown:      r = cb::transposeNotesInScale (m, targets, -1, key, lim); break;
            case Command::SemitoneUp:    r = cb::transposeNotes (m, targets, 1, lim); break;
            case Command::SemitoneDown:  r = cb::transposeNotes (m, targets, -1, lim); break;
            case Command::OctaveUp:      r = cb::transposeNotes (m, targets, 12, lim); break;
            case Command::OctaveDown:    r = cb::transposeNotes (m, targets, -12, lim); break;

            case Command::ResetLayer:
            case Command::ResetChord:
            case Command::ClearMelody:
            case Command::ToggleLayerOn:
            case Command::ToggleVelocityLane:
            case Command::ToggleOtherLayers:
            default:
                return false;
        }

        if (r.notes == m)
            return false;

        if (dryRun)
            return true;

        if (command == Command::VelocitySoft || command == Command::VelocityMedium || command == Command::VelocityDefault
            || command == Command::VelocityHard)
        {
            const auto sel2 = cb::validSelection (r.notes, r.selection);
            view().lastVelocity = sel2.empty() ? r.notes.notes.front().velocity : r.notes.notes[(size_t) sel2.front()].velocity;
        }

        const auto newSelection = keepSelection ? (hadSelection ? r.selection : std::vector<int>()) : r.selection;
        commitEdit (r.notes, newSelection, m);

        if (command == Command::OctaveUp || command == Command::OctaveDown)
            scrollTo (topNote() + (command == Command::OctaveUp ? 12 : -12));

        return true;
    }

    void addCommand (juce::PopupMenu& menu, Command c, const juce::String& text, cb::Layer target, double atBeat, bool ticked = false)
    {
        menu.addItem (kCommandIdBase + (int) c, text, perform (target, c, atBeat, true), ticked);
    }

    void addLayerSection (juce::PopupMenu& menu, cb::Layer target, double atBeat)
    {
        menu.addSectionHeader (layerTitle (target) + "  (" + layerStateText (target) + ")");
        addCommand (menu, Command::ToggleLayerOn, "Play " + layerLower (target), target, atBeat, processor.isLayerOn (target));

        if (isGenerated (target))
        {
            addCommand (menu, Command::ResetLayer, "Reset " + layerLower (target) + " to follow the chords", target, atBeat);

            if (atBeat >= 0.0 && target == layer)
                addCommand (menu, Command::ResetChord, "Reset this chord", target, atBeat);
        }
        else
        {
            addCommand (menu, Command::ClearMelody, "Clear melody", target, atBeat);
        }
    }

    juce::String selectionHeader (const juce::String& prefix) const
    {
        const auto n = cb::validSelection (notes(), selection).size();
        return n == 0 ? prefix + "All notes" : prefix + juce::String ((int) n) + (n == 1 ? " note" : " notes");
    }

    void addVelocityItems (juce::PopupMenu& menu, double atBeat)
    {
        const auto& m = notes();
        const auto sel = cb::validSelection (m, selection);
        const auto targets = sel.empty() ? cb::allNoteIndices (m) : sel;
        auto allAt = [&] (float v)
        {
            if (targets.empty())
                return false;
            for (int i : targets)
                if (std::abs (m.notes[(size_t) i].velocity - v) > 1.0e-4f)
                    return false;
            return true;
        };

        addCommand (menu, Command::VelocitySoft, "Soft 40%", layer, atBeat, allAt (0.40f));
        addCommand (menu, Command::VelocityMedium, "Medium 65%", layer, atBeat, allAt (0.65f));
        addCommand (menu, Command::VelocityDefault, "Default 80%", layer, atBeat, allAt (0.80f));
        addCommand (menu, Command::VelocityHard, "Hard 100%", layer, atBeat, allAt (1.00f));
        addCommand (menu, Command::VelocityUp, "Louder +10%", layer, atBeat);
        addCommand (menu, Command::VelocityDown, "Softer -10%", layer, atBeat);
    }

    void addSelectionSection (juce::PopupMenu& menu, double atBeat)
    {
        menu.addSectionHeader (selectionHeader ("Selection " + middleDot() + " "));
        addCommand (menu, Command::SelectAll, "Select all", layer, atBeat);
        addCommand (menu, Command::SelectNone, "Select none", layer, atBeat);
        menu.addSeparator();
        addCommand (menu, Command::Copy, "Copy", layer, atBeat);
        addCommand (menu, Command::Cut, "Cut", layer, atBeat);
        addCommand (menu, Command::Paste, "Paste", layer, atBeat);

        if (atBeat >= 0.0)
            addCommand (menu, Command::PasteHere, "Paste here", layer, atBeat);

        addCommand (menu, Command::Duplicate, "Duplicate", layer, atBeat);
        addCommand (menu, Command::Delete, "Delete", layer, atBeat);
        menu.addSeparator();

        juce::PopupMenu velocity;
        velocity.setLookAndFeel (&getLookAndFeel());
        addVelocityItems (velocity, atBeat);
        menu.addSubMenu ("Velocity", velocity, ! notes().empty());

        juce::PopupMenu transpose;
        transpose.setLookAndFeel (&getLookAndFeel());
        addCommand (transpose, Command::StepUp, "Up a scale step", layer, atBeat);
        addCommand (transpose, Command::StepDown, "Down a scale step", layer, atBeat);
        addCommand (transpose, Command::SemitoneUp, "Up a semitone", layer, atBeat);
        addCommand (transpose, Command::SemitoneDown, "Down a semitone", layer, atBeat);
        addCommand (transpose, Command::OctaveUp, "Up an octave", layer, atBeat);
        addCommand (transpose, Command::OctaveDown, "Down an octave", layer, atBeat);
        menu.addSubMenu ("Transpose", transpose, ! notes().empty());

        addCommand (menu, Command::Quantize, "Quantize starts to grid", layer, atBeat);
        addCommand (menu, Command::Legato, "Legato (fill the gaps)", layer, atBeat);
    }

    void addViewSection (juce::PopupMenu& menu)
    {
        menu.addSectionHeader ("View");
        juce::PopupMenu snapMenu, gridMenu;
        snapMenu.setLookAndFeel (&getLookAndFeel());
        gridMenu.setLookAndFeel (&getLookAndFeel());

        for (int i = 0; i < cb::numMelodySnapModes; ++i)
            snapMenu.addItem (kSnapIdBase + i, cb::melodySnapName ((cb::MelodySnap) i), true, i == (int) view().snap);

        for (int i = 0; i < cb::numRateDivisions; ++i)
            gridMenu.addItem (kGridIdBase + i, cb::rateDivisionName ((cb::RateDivision) i), true, i == (int) view().grid);

        menu.addSubMenu ("Snap", snapMenu);
        menu.addSubMenu ("Grid", gridMenu);
        addCommand (menu, Command::ToggleVelocityLane, "Velocity lane", layer, -1.0, velLane);
        addCommand (menu, Command::ToggleOtherLayers, "Show other layers and instances", layer, -1.0, showGhosts);
    }

    juce::PopupMenu createMenu (MenuSection section, double atBeat, std::optional<cb::Layer> forLayer)
    {
        const auto target = forLayer.value_or (layer);
        juce::PopupMenu menu;
        menu.setLookAndFeel (&getLookAndFeel());

        switch (section)
        {
            case MenuSection::All:
                addLayerSection (menu, target, atBeat);
                addSelectionSection (menu, atBeat);
                addViewSection (menu);
                break;

            case MenuSection::Selection:
                addSelectionSection (menu, -1.0);
                break;

            case MenuSection::Layer:
                addLayerSection (menu, target, -1.0);
                break;

            case MenuSection::Velocity:
                menu.addSectionHeader (selectionHeader ("Velocity " + middleDot() + " "));
                addVelocityItems (menu, -1.0);
                menu.addSeparator();
                addCommand (menu, Command::ToggleVelocityLane, "Velocity lane", layer, -1.0, velLane);
                break;

            case MenuSection::Chord:
                addCommand (menu, Command::ResetChord, "Reset this chord", layer, atBeat);
                break;

            default:
                break;
        }

        return menu;
    }

    // What a menu showed when it opened: its item is resolved later (async), when the layer's notes may have changed
    // (an automation re-render clears the selection), so a note command must not fall back to every note.
    struct MenuScope
    {
        cb::Layer layer = cb::Layer::Melody;
        bool hadSelection = false;
        std::vector<cb::MelodyNote> selected;
    };

    static bool actsOnSelection (Command c)
    {
        switch (c)
        {
            case Command::SelectNone: case Command::Copy: case Command::Cut: case Command::Duplicate: case Command::Delete:
            case Command::Quantize: case Command::Legato:
            case Command::VelocitySoft: case Command::VelocityMedium: case Command::VelocityDefault: case Command::VelocityHard:
            case Command::VelocityUp: case Command::VelocityDown:
            case Command::StepUp: case Command::StepDown: case Command::SemitoneUp: case Command::SemitoneDown:
            case Command::OctaveUp: case Command::OctaveDown:
                return true;

            case Command::SelectAll: case Command::Paste: case Command::PasteHere:
            case Command::ResetLayer: case Command::ResetChord: case Command::ClearMelody: case Command::ToggleLayerOn:
            case Command::ToggleVelocityLane: case Command::ToggleOtherLayers:
            default:
                return false;
        }
    }

    void menuChosen (int result, cb::Layer target, double atBeat, const MenuScope& scope)
    {
        if (result >= kSnapIdBase && result < kSnapIdBase + cb::numMelodySnapModes)
        {
            setSnap ((cb::MelodySnap) (result - kSnapIdBase));
            return;
        }

        if (result >= kGridIdBase && result < kGridIdBase + cb::numRateDivisions)
        {
            setGrid ((cb::RateDivision) (result - kGridIdBase));
            return;
        }

        if (result < kCommandIdBase || result >= kCommandIdBase + kNumCommands)
            return;

        const auto command = (Command) (result - kCommandIdBase);

        if (actsOnSelection (command))
        {
            dropGesture();
            syncLayers (true);

            if (layer != scope.layer)
                return;

            if (scope.hadSelection)
            {
                // Act on exactly the notes the menu was opened for, found again by (pitch, start); refused when
                // any of them is gone.
                const auto found = cb::findNotes (notes(), scope.selected);

                if (found.size() != scope.selected.size())
                {
                    selection.clear();
                    showHint ("The notes changed: select them again");
                    repaint();
                    return;
                }

                selection = found;
            }
            else
            {
                selection.clear();   // the menu offered "All notes"
            }
        }

        perform (target, command, atBeat, false);
    }

    void showMenu (MenuSection section, double atBeat, std::optional<cb::Layer> forLayer, juce::Rectangle<int> screenArea)
    {
        syncLayers (true);
        const auto target = forLayer.value_or (layer);
        auto menu = createMenu (section, atBeat, target);
        lastMenu = section;

        MenuScope scope;
        scope.layer = layer;
        scope.selected = notesAt (notes(), selection);
        scope.hadSelection = ! scope.selected.empty();

        if (screenArea.isEmpty())
            screenArea = { juce::Desktop::getMousePosition(), juce::Point<int> (1, 1) + juce::Desktop::getMousePosition() };

        juce::Component::SafePointer<Impl> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (screenArea),
                            [safe, target, atBeat, scope] (int result)
                            {
                                if (safe != nullptr && result != 0)
                                    safe->menuChosen (result, target, atBeat, scope);
                            });
    }

    //==============================================================================
    // Playback display
    bool isSounding (const cb::MelodyNote& n, double atBeat) const
    {
        if (! playing || ! layerOn[(size_t) cur()] || total <= 0 || n.startBeat >= (double) total)
            return false;

        return atBeat >= n.startBeat && atBeat < std::min (n.startBeat + n.lengthBeats, (double) total);
    }

    void repaintPlayheadColumn (double beat)
    {
        const float x = beatToX (beat);
        repaint (juce::Rectangle<float> (x - 6.0f, stripArea.getY() - 1.0f, 12.0f, gridArea.getBottom() - stripArea.getY() + 2.0f)
                     .getSmallestIntegerContainer());
    }

    void repaintSoundingChanges (double from, double to)
    {
        const int hi = topNote(), lo = lowestPaintedRow();

        for (const auto& n : shown().notes)
        {
            if (n.note < lo || n.note > hi || isSounding (n, from) == isSounding (n, to))
                continue;

            repaint (noteRect (n).expanded (3.0f).getIntersection (gridArea).getSmallestIntegerContainer());
        }
    }

    void timerCallback() override
    {
        if (! isVisible())
            return;

        const bool playingNow = processor.getPlayingSlot() >= 0;
        const double beat = playingNow ? processor.getPlaybackBeat() : 0.0;

        if (playingNow != playing)
        {
            playing = playingNow;
            playBeat = beat;
            repaint (stripArea.getUnion (gridArea).expanded (4.0f).getSmallestIntegerContainer());
        }
        else if (playingNow && std::abs (beat - playBeat) > 1.0e-4)
        {
            // Only the old and new playhead columns and the notes that started or stopped sounding (the velocity
            // lane shows neither).
            const double old = playBeat;
            playBeat = beat;
            repaintPlayheadColumn (old);
            repaintPlayheadColumn (beat);
            repaintSoundingChanges (old, beat);
        }

        if (const auto s = processor.getGhostSerial(); s != ghostSerial)
        {
            ghostSerial = s;
            ghosts = processor.getGhostMelodies();
            repaint();
        }

        // Parameter-only changes (automation, LED clicks, the Controls panel) send no change message: poll the serial.
        if (processor.getLayerSerial() != serial && ! inGridGesture())
            syncAll();

        if (hintText.isNotEmpty() && ! hintVisible())
        {
            hintText.clear();
            repaint();
        }
    }

    //==============================================================================
    ChordbloomProcessor& processor;
    std::unique_ptr<RollToolbar> rollToolbar;   // child 1 of MelodyRoll

    // Settings
    std::array<View, (size_t) cb::numLayers> views;
    cb::Layer layer = cb::Layer::Melody;
    bool showGhosts = true, velLane = false;
    int rightInset = 0;

    // Data
    cb::Progression progression;
    cb::Key key;
    int total = 0;
    std::array<cb::Melody, (size_t) cb::numLayers> layerNotes;
    std::array<bool, (size_t) cb::numLayers> layerOn {}, layerCustom {};
    std::vector<bool> editedSlots;
    uint64_t serial = 0;
    bool synced = false;          // syncAll ran once (the cached data below is valid)
    juce::StringArray slotNames;
    std::vector<juce::Colour> slotColours;
    std::vector<ChordbloomProcessor::GhostMelody> ghosts;
    uint64_t ghostSerial = 0;
    std::vector<int> selection;   // ascending indices into layerNotes[layer]
    std::optional<MenuSection> lastMenu;
    juce::String hintText;
    juce::uint32 hintUntil = 0;

    // Geometry
    juce::Rectangle<float> toolbarArea, stripArea, gridArea, laneArea;
    bool compact = false;
    int visibleBeats = kMinVisibleBeats;
    float pxPerBeat = 1.0f, rowH = 10.0f;
    float wheelAccumulator = 0.0f;

    // Playback display
    bool playing = false;
    double playBeat = 0.0;

    // Interaction
    Gesture gesture = Gesture::none;
    cb::Melody snapshot;              // the layer's notes at mouse-down (a gesture commits only when it differs)
    cb::Melody base;                  // what the gesture edits from (snapshot, plus the drawn note when drawing)
    cb::Melody working;               // the gesture's result so far (shown while it runs)
    std::vector<int> workingSel;      // the selection shown while the gesture runs (indices into working)
    std::vector<int> group;           // the notes the gesture moves / resizes / duplicates (indices into base)
    bool groupSelected = false, duplicateSelects = false, velocityGroup = false;
    cb::MelodyNote anchorNote;        // the grabbed note as it was in base
    int activeIndex = -1;             // the grabbed note in working (highlight), or the velocity stalk
    float velocityOriginal = 0.8f;
    int pendingHit = -1;
    bool marqueeAdd = false;
    std::vector<int> marqueeBase;
    juce::Rectangle<float> marqueeRect;
    std::vector<cb::MelodyNote> eraseWanted;   // the selected notes an erase gesture keeps selected
    juce::Point<float> downPos, lastErasePos, lastMouse;
    bool dragged = false, erasedAny = false, pressedOnNote = false;
    int hoverIndex = -1, hoverRow = -1, hoverPill = 0;
    bool hoverEdge = false, cardButtonHover = false;
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
    addAndMakeVisible (*impl);                // child 0: the grid surface
    addAndMakeVisible (*impl->rollToolbar);   // child 1: the toolbar row (on top of the surface's toolbar area)
}

MelodyRoll::~MelodyRoll() = default;

void MelodyRoll::paint (juce::Graphics&) {}   // the Impl surface paints the whole roll

void MelodyRoll::resized() { impl->setBounds (getLocalBounds()); }

void MelodyRoll::refresh()
{
    impl->syncAll();
}

void MelodyRoll::setToolbarRightInset (int pixels)
{
    pixels = std::max (0, pixels);

    if (impl->rightInset != pixels)
    {
        impl->rightInset = pixels;
        impl->layout();
        impl->repaint();
    }
}

cb::MelodySnap MelodyRoll::getSnap() const noexcept { return impl->view().snap; }
void MelodyRoll::setSnap (cb::MelodySnap s) { impl->setSnap (s); }
cb::RateDivision MelodyRoll::getGrid() const noexcept { return impl->view().grid; }
void MelodyRoll::setGrid (cb::RateDivision r) { impl->setGrid (r); }

int MelodyRoll::getTopNote() const noexcept { return impl->topNote(); }
int MelodyRoll::getBottomNote() const noexcept { return impl->bottomNote(); }
void MelodyRoll::setTopNote (int midiNote) { impl->scrollTo (midiNote); }

juce::Rectangle<float> MelodyRoll::getGridArea() const noexcept { return impl->gridArea; }

juce::Point<float> MelodyRoll::getCellCentre (double beat, int midiNote) const
{
    return { impl->beatToX (beat), impl->rowTop (midiNote) + impl->rowH * 0.5f };
}

juce::Rectangle<float> MelodyRoll::getNoteBounds (int noteIndex) const
{
    const auto m = impl->processor.getLayerNotes (impl->layer);

    if (noteIndex < 0 || noteIndex >= (int) m.notes.size())
        return {};

    return impl->noteRect (m.notes[(size_t) noteIndex]);
}

int MelodyRoll::getNumGhostNotesShown() const noexcept { return impl->countVisibleGhosts(); }

//==============================================================================
cb::Layer MelodyRoll::getLayer() const noexcept { return impl->layer; }
void MelodyRoll::setLayer (cb::Layer layer) { impl->setLayer (layer); }

std::vector<int> MelodyRoll::getSelection() const { return impl->selection; }

void MelodyRoll::setSelection (const std::vector<int>& selection)
{
    impl->dropGesture();
    impl->syncLayers (true);
    impl->selection = cb::validSelection (impl->notes(), selection);
    impl->repaint();
}

bool MelodyRoll::performCommand (Command command, double atBeat)
{
    return impl->perform (impl->layer, command, atBeat, false);
}

bool MelodyRoll::areOtherLayersShown() const noexcept { return impl->showGhosts; }
void MelodyRoll::setOtherLayersShown (bool shouldShow) { impl->setOtherLayersShown (shouldShow); }

juce::PopupMenu MelodyRoll::createMenu (MenuSection section, double atBeat, std::optional<cb::Layer> layer)
{
    impl->syncLayers (true);
    return impl->createMenu (section, atBeat, layer);
}

std::optional<MelodyRoll::MenuSection> MelodyRoll::getLastMenuShown() const noexcept { return impl->lastMenu; }
juce::Rectangle<float> MelodyRoll::getChordStripArea() const noexcept { return impl->stripArea; }
int MelodyRoll::getNumOtherLayerNotesShown() const noexcept { return impl->countVisibleOtherLayerNotes(); }
bool MelodyRoll::isVelocityLaneVisible() const noexcept { return impl->velLane; }
void MelodyRoll::setVelocityLaneVisible (bool shouldShow) { impl->setVelocityLane (shouldShow); }
juce::Rectangle<float> MelodyRoll::getVelocityLaneArea() const noexcept { return impl->laneArea; }
juce::Rectangle<float> MelodyRoll::getEmptyStateButtonArea() const noexcept { return impl->cardButtonArea(); }
juce::Rectangle<float> MelodyRoll::getSelectionPillArea() const noexcept { return impl->pills().selection; }
juce::Rectangle<float> MelodyRoll::getLayerOffPillArea() const noexcept { return impl->pills().off; }
RollToolbar& MelodyRoll::getToolbar() noexcept { return *impl->rollToolbar; }
