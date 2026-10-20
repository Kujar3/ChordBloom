#include "TestFramework.h"

#include "core/Layers.h"
#include "core/Voicing.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

// Layers, follow and note limits (owner: WP3; final-spec 4.1, 4.4, 9 WP3). WP0 seeded this file with checks of the
// frozen identity / limit functions; WP3 adds the rest.

using namespace cb;

namespace
{
// Tiny deterministic generator (xorshift32); the tests never use std::rand or <random>.
struct Rng
{
    uint32_t state;

    explicit Rng (uint32_t seed) : state (seed != 0 ? seed : 1u) {}

    uint32_t next()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    int range (int lo, int hi) { return lo + (int) (next() % (uint32_t) (hi - lo + 1)); }
    double unit() { return (double) next() / 4294967296.0; }
    bool chance (int percent) { return range (0, 99) < percent; }
};

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

MelodyNote mn (int note, double start, double length, float velocity = kDefaultMelodyVelocity)
{
    MelodyNote n;
    n.note = note;
    n.startBeat = start;
    n.lengthBeats = length;
    n.velocity = velocity;
    return n;
}

Melody mel (std::initializer_list<MelodyNote> notes)
{
    Melody m;
    m.notes = notes;
    return m;
}

const Chord C { 0, ChordQuality::Major };
const Chord Am { 9, ChordQuality::Minor };
const Chord F { 5, ChordQuality::Major };
const Chord G { 7, ChordQuality::Major };
const Chord Dm { 2, ChordQuality::Minor };
const Chord Cm { 0, ChordQuality::Minor };
const Chord Cmaj7 { 0, ChordQuality::Major7 };
const Chord E7 { 4, ChordQuality::Dominant7 };
const Chord Em { 4, ChordQuality::Minor };
const Key cMajor { 0, ScaleType::Major };
const Key aMinor { 9, ScaleType::NaturalMinor };

Progression prog (std::initializer_list<std::pair<Chord, int>> chords)
{
    Progression p;

    for (const auto& [chord, beats] : chords)
    {
        ProgressionSlot s;
        s.chord = chord;
        s.lengthBeats = beats;
        p.slots.push_back (s);
    }

    return p;
}

// The notes of a list that start in [from, to) (order kept), optionally moved by `shift` beats.
Melody slice (const Melody& m, double from, double to, double shift = 0.0)
{
    Melody out;

    for (auto n : m.notes)
        if (n.startBeat >= from - 1.0e-9 && n.startBeat < to - 1.0e-9)
        {
            n.startBeat += shift;
            out.notes.push_back (n);
        }

    return out;
}

Melody joined (std::initializer_list<Melody> parts, const NoteLimits& limits)
{
    Melody all;

    for (const auto& p : parts)
        all.notes.insert (all.notes.end(), p.notes.begin(), p.notes.end());

    return sanitiseMelody (all, limits);
}

//==============================================================================
// The phase-1.5 sanitiseMelody, kept here verbatim as the reference for "the old function == the kMelodyLimits case".
Melody legacySanitise (const Melody& in)
{
    auto roundToTick = [] (double beats) { return std::round (beats * 48.0) / 48.0; };
    Melody out;
    const double latestStart = kMaxMelodyBeat - kMinMelodyNoteBeats;

    for (auto n : in.notes)
    {
        if (! std::isfinite (n.startBeat) || ! std::isfinite (n.lengthBeats))
            continue;

        n.note = std::clamp (n.note, kMelodyLowestNote, kMelodyHighestNote);
        n.velocity = std::isfinite (n.velocity) ? std::clamp (n.velocity, 0.05f, 1.0f) : kDefaultMelodyVelocity;
        n.startBeat = roundToTick (std::clamp (n.startBeat, 0.0, latestStart));

        const double room = kMaxMelodyBeat - n.startBeat;
        n.lengthBeats = roundToTick (std::clamp (n.lengthBeats, kMinMelodyNoteBeats, std::min (kMaxMelodyNoteBeats, room)));
        n.lengthBeats = std::max (n.lengthBeats, kMinMelodyNoteBeats);
        out.notes.push_back (n);
    }

    std::sort (out.notes.begin(), out.notes.end(), [] (const MelodyNote& a, const MelodyNote& b)
    {
        if (a.startBeat != b.startBeat)
            return a.startBeat < b.startBeat;
        if (a.note != b.note)
            return a.note < b.note;
        if (a.lengthBeats != b.lengthBeats)
            return a.lengthBeats < b.lengthBeats;
        return a.velocity < b.velocity;
    });

    std::vector<MelodyNote> unique;

    for (size_t i = 0; i < out.notes.size(); ++i)
        if (i + 1 == out.notes.size() || out.notes[i + 1].startBeat != out.notes[i].startBeat
            || out.notes[i + 1].note != out.notes[i].note)
            unique.push_back (out.notes[i]);

    if ((int) unique.size() > kMaxMelodyNotes)
        unique.resize ((size_t) kMaxMelodyNotes);

    out.notes = unique;
    return out;
}

// Random lists that exercise every sanitising branch: out-of-range pitches and starts, tiny / huge / negative lengths,
// off-grid times, NaN / infinity, duplicated (pitch, start), velocities outside 0..1.
Melody randomMessyMelody (Rng& rng)
{
    Melody m;
    const int count = rng.range (0, 40);

    for (int i = 0; i < count; ++i)
    {
        MelodyNote n;
        n.note = rng.range (-5, 140);
        n.startBeat = rng.chance (4) ? (rng.chance (50) ? kNaN : kInf) : -2.0 + rng.unit() * 300.0;
        n.lengthBeats = rng.chance (4) ? kNaN : -1.0 + rng.unit() * 70.0;
        n.velocity = rng.chance (4) ? (float) kNaN : (float) (-0.5 + rng.unit() * 2.0);

        if (rng.chance (15))
            n.startBeat = std::round (n.startBeat * 4.0) / 4.0;

        m.notes.push_back (n);

        if (rng.chance (10))
        {
            auto copy = n;
            copy.lengthBeats = rng.unit() * 3.0;
            m.notes.push_back (copy);
        }
    }

    return m;
}

Progression randomProgression (Rng& rng, int minSlots, int maxSlots)
{
    static const ChordQuality qualities[] = { ChordQuality::Major, ChordQuality::Minor, ChordQuality::Dominant7 };
    Progression p;
    const int count = rng.range (minSlots, maxSlots);

    for (int i = 0; i < count; ++i)
    {
        ProgressionSlot s;
        s.chord = Chord { rng.range (0, 11), qualities[rng.range (0, 2)] };
        s.lengthBeats = rng.range (1, 8);
        p.slots.push_back (s);
    }

    return p;
}

struct RandomEdit
{
    Progression after;
    std::vector<int> source;
};

// A structural edit (insert, remove, move, duplicate, length) of a progression; moved slots keep their chord.
RandomEdit randomEdit (Rng& rng, const Progression& before)
{
    RandomEdit e;
    e.after = before;
    const int n = (int) before.slots.size();
    e.source.resize ((size_t) n);

    for (int i = 0; i < n; ++i)
        e.source[(size_t) i] = i;

    switch (rng.range (0, 4))
    {
        case 0:   // insert
            if (n < kMaxSlots)
            {
                const int at = rng.range (0, n);
                ProgressionSlot s;
                s.chord = Chord { rng.range (0, 11), ChordQuality::Minor };
                s.lengthBeats = rng.range (1, 8);
                e.after.slots.insert (e.after.slots.begin() + at, s);
                e.source.insert (e.source.begin() + at, -1);
            }
            break;

        case 1:   // remove
            if (n > 1)
            {
                const int at = rng.range (0, n - 1);
                e.after.slots.erase (e.after.slots.begin() + at);
                e.source.erase (e.source.begin() + at);
            }
            break;

        case 2:   // move one slot elsewhere
            if (n > 1)
            {
                const int from = rng.range (0, n - 1), to = rng.range (0, n - 1);
                const auto slot = e.after.slots[(size_t) from];
                const int src = e.source[(size_t) from];
                e.after.slots.erase (e.after.slots.begin() + from);
                e.source.erase (e.source.begin() + from);
                e.after.slots.insert (e.after.slots.begin() + to, slot);
                e.source.insert (e.source.begin() + to, src);
            }
            break;

        case 3:   // duplicate
            if (n < kMaxSlots)
            {
                const int at = rng.range (0, n - 1);
                e.after.slots.insert (e.after.slots.begin() + at + 1, before.slots[(size_t) at]);
                e.source.insert (e.source.begin() + at + 1, at);
            }
            break;

        default:  // length change
        {
            const int at = rng.range (0, n - 1);
            e.after.slots[(size_t) at].lengthBeats = rng.range (1, 12);
            break;
        }
    }

    return e;
}

LayerFollowContext plainContext (const Progression& before, const Progression& after, const std::vector<int>& source)
{
    LayerFollowContext c;
    c.before = before;
    c.after = after;
    c.sourceSlot = source;
    c.key = cMajor;
    return c;
}

//==============================================================================
// Real auto renders for the follow tests. Bass: the classic bass modes of renderProgression (channel 2). Arp: no arp
// renderer is needed here; the up-arp of the chord renderer (channel 1, 1/16) is an arp-like auto list of the same
// kind (notesFromEvents form, arp limits).
struct AutoRender
{
    Layer layer;
    BassMode bassMode;
};

Melody autoNotesFor (const AutoRender& a, const Progression& p)
{
    RenderSettings rs;
    rs.velocity = 0.75f;
    rs.gate = 0.9f;
    rs.swing = 0.0f;
    rs.humanize = 0.0f;

    int channel = 2;

    if (a.layer == Layer::Bass)
    {
        rs.pattern = RhythmPattern::Block;
        rs.rate = RateDivision::Quarter;
        rs.bass = a.bassMode;
    }
    else
    {
        rs.pattern = RhythmPattern::ArpUp;
        rs.rate = RateDivision::Sixteenth;
        rs.bass = BassMode::Off;
        channel = 1;
    }

    const auto voicings = voiceProgression (p, VoicingSettings {});
    auto events = renderProgression (p, voicings, rs);
    events.erase (std::remove_if (events.begin(), events.end(), [channel] (const NoteEvent& e) { return e.channel != channel; }),
                  events.end());
    return notesFromEvents (events, layerLimits (a.layer));
}

LayerFollowContext realContext (const AutoRender& a, const Progression& before, const Progression& after,
                                const std::vector<int>& source)
{
    auto c = plainContext (before, after, source);
    const auto vb = voiceProgression (before, VoicingSettings {});
    const auto va = voiceProgression (after, VoicingSettings {});

    for (size_t i = 0; i < before.slots.size(); ++i)
        c.beforeBassPc.push_back (slotBassPitchClass (before.slots[i], vb[i]));

    for (size_t i = 0; i < after.slots.size(); ++i)
        c.afterBassPc.push_back (slotBassPitchClass (after.slots[i], va[i]));

    c.autoBefore = autoNotesFor (a, before);
    c.autoAfter = autoNotesFor (a, after);
    return c;
}
} // namespace

//==============================================================================
TEST_CASE ("layers: WP0 identity, channels and limits")
{
    CHECK_EQ (numLayers, 3);
    CHECK_EQ (std::string (layerKey (Layer::Arp)), std::string ("arp"));
    CHECK_EQ (std::string (layerName (Layer::Bass)), std::string ("Bass"));
    CHECK (layerFromKey ("BASS") == Layer::Bass);
    CHECK (! layerFromKey ("chords").has_value());
    CHECK_EQ (layerMidiChannel (Layer::Melody), 3);
    CHECK_EQ (layerMidiChannel (Layer::Arp), 4);
    CHECK_EQ (layerMidiChannel (Layer::Bass), 2);
    CHECK_EQ (layerLimits (Layer::Bass).lowest, 24);
    CHECK_EQ (layerLimits (Layer::Bass).highest, 72);
    CHECK_EQ (layerLimits (Layer::Arp).maxNotes, kMaxLayerNotes);
    CHECK_EQ (layerLimits (Layer::Melody).maxNotes, kMaxMelodyNotes);
}

TEST_CASE ("layers: names, keys, channels, limits and LayerState equality")
{
    CHECK_EQ (std::string (layerName (Layer::Melody)), std::string ("Melody"));
    CHECK_EQ (std::string (layerName (Layer::Arp)), std::string ("Arp"));
    CHECK_EQ (std::string (layerKey (Layer::Melody)), std::string ("melody"));
    CHECK_EQ (std::string (layerKey (Layer::Bass)), std::string ("bass"));

    for (int i = 0; i < numLayers; ++i)
        CHECK (layerFromKey (layerKey ((Layer) i)) == (Layer) i);

    CHECK (layerFromKey ("Melody") == Layer::Melody);
    CHECK (layerFromKey ("aRp") == Layer::Arp);
    CHECK (layerFromKey ("bAsS") == Layer::Bass);
    CHECK (! layerFromKey ("").has_value());
    CHECK (! layerFromKey ("bass ").has_value());
    CHECK (! layerFromKey ("drums").has_value());

    const auto melody = layerLimits (Layer::Melody);
    CHECK_EQ (melody.lowest, 36);
    CHECK_EQ (melody.highest, 96);
    CHECK_EQ (melody.maxNotes, 512);

    const auto arp = layerLimits (Layer::Arp);
    CHECK_EQ (arp.lowest, 28);
    CHECK_EQ (arp.highest, 108);
    CHECK_EQ (arp.maxNotes, 2048);

    const auto bass = layerLimits (Layer::Bass);
    CHECK_EQ (bass.lowest, 24);
    CHECK_EQ (bass.highest, 72);
    CHECK_EQ (bass.maxNotes, 2048);

    LayerState a, b;
    CHECK (a == b);
    b.custom = true;
    CHECK (a != b);
    a.custom = true;
    CHECK (a == b);
    b.notes = mel ({ mn (40, 0, 1) });
    CHECK (a != b);
}

//==============================================================================
TEST_CASE ("note limits: every overload equals the old function with kMelodyLimits (1000 random lists)")
{
    Rng rng (0x1234abcdu);

    for (int iter = 0; iter < 1000; ++iter)
    {
        const auto m = randomMessyMelody (rng);

        const auto s = sanitiseMelody (m);
        CHECK (s == sanitiseMelody (m, kMelodyLimits));
        CHECK (s == legacySanitise (m));

        const double loop = rng.chance (10) ? 0.0 : (double) rng.range (1, 64);
        CHECK (renderMelody (m, loop) == renderMelody (m, loop, 3, kMelodyLimits));
        CHECK (renderMelody (m, loop, 5) == renderMelody (m, loop, 5, kMelodyLimits));

        const int semis = rng.range (-30, 30);
        CHECK (transposeMelody (m, semis) == transposeMelody (m, semis, kMelodyLimits));

        const auto before = randomProgression (rng, 1, 6);
        const auto edit = randomEdit (rng, before);
        CHECK (melodyFollowingSlots (m, before, edit.after, edit.source)
               == melodyFollowingSlots (m, before, edit.after, edit.source, kMelodyLimits));

        auto badSource = edit.source;
        badSource.push_back (0);
        CHECK (melodyFollowingSlots (m, before, edit.after, badSource)
               == melodyFollowingSlots (m, before, edit.after, badSource, kMelodyLimits));

        const int pitch = rng.range (-10, 140);
        const auto snap = (MelodySnap) rng.range (0, numMelodySnapModes - 1);
        const Key key { rng.range (0, 11), (ScaleType) rng.range (0, numScaleTypes - 1) };
        const std::optional<Chord> chord = rng.chance (30) ? std::nullopt
                                                           : std::optional<Chord> (before.slots[0].chord);
        CHECK_EQ (snapMelodyPitch (pitch, snap, key, chord), snapMelodyPitch (pitch, snap, key, chord, kMelodyLimits));
    }
}

TEST_CASE ("note limits: idempotent, in range and capped for every layer's limits")
{
    Rng rng (0x77aa11u);

    for (int layer = 0; layer < numLayers; ++layer)
    {
        const auto limits = layerLimits ((Layer) layer);

        for (int iter = 0; iter < 200; ++iter)
        {
            const auto s = sanitiseMelody (randomMessyMelody (rng), limits);
            CHECK (sanitiseMelody (s, limits) == s);
            CHECK ((int) s.notes.size() <= limits.maxNotes);

            for (const auto& n : s.notes)
                CHECK (n.note >= limits.lowest && n.note <= limits.highest);
        }
    }

    // A degenerate range: highest below lowest uses lowest for both; values are clamped to 0..127.
    const auto odd = sanitiseMelody (mel ({ mn (10, 0, 1), mn (100, 1, 1) }), NoteLimits { 50, 40, 10 });
    REQUIRE (odd.notes.size() == 2);
    CHECK_EQ (odd.notes[0].note, 50);
    CHECK_EQ (odd.notes[1].note, 50);
    CHECK_EQ (sanitiseMelody (mel ({ mn (60, 0, 1) }), NoteLimits { 36, 96, 0 }).notes.size(), (size_t) 0);
    CHECK_EQ (sanitiseMelody (mel ({ mn (60, 0, 1) }), NoteLimits { -20, 500, 5 }).notes[0].note, 60);
}

TEST_CASE ("note limits: snapMelodyPitch stays inside the limits")
{
    const auto bass = layerLimits (Layer::Bass);
    const std::optional<Chord> chord = C;

    // a chord-tone snap of a very low note stays inside the bass range (26 -> C 24; the melody range starts at 36)
    CHECK_EQ (snapMelodyPitch (26, MelodySnap::ChordTones, cMajor, chord, bass), 24);
    CHECK_EQ (snapMelodyPitch (26, MelodySnap::ChordTones, cMajor, chord), 36);
    CHECK_EQ (snapMelodyPitch (23, MelodySnap::Off, cMajor, chord, bass), 24);
    CHECK_EQ (snapMelodyPitch (100, MelodySnap::Off, cMajor, chord, bass), 72);
    CHECK_EQ (snapMelodyPitch (71, MelodySnap::ChordTones, cMajor, chord, bass), 72);   // B -> C above (C at 72 is allowed)
    CHECK_EQ (snapMelodyPitch (73, MelodySnap::ChordTones, cMajor, chord, bass), 72);   // clamped first
    CHECK_EQ (snapMelodyPitch (62, MelodySnap::Scale, cMajor, std::nullopt, NoteLimits { 60, 62, 8 }), 62);
}

TEST_CASE ("note limits: a bass note at MIDI 28 survives sanitise, render, transpose and follow")
{
    const auto bass = layerLimits (Layer::Bass);
    const auto m = mel ({ mn (28, 0.0, 1.0), mn (24, 1.0, 1.0), mn (72, 2.0, 1.0) });

    CHECK_EQ (sanitiseMelody (m, bass).notes[0].note, 28);
    CHECK_EQ (sanitiseMelody (m, bass).notes[1].note, 24);
    CHECK_EQ (sanitiseMelody (m, bass).notes[2].note, 72);
    CHECK_EQ (sanitiseMelody (m).notes[0].note, 36);   // the melody limits would have raised it

    const auto events = renderMelody (m, 8.0, 2, bass);
    REQUIRE (events.size() == 3);
    CHECK_EQ (events[0].note, 28);
    CHECK_EQ (events[0].channel, 2);
    CHECK_EQ (events[1].note, 24);

    const auto one = mel ({ mn (28, 0.0, 1.0) });
    CHECK_EQ (transposeMelody (one, 0, bass).notes[0].note, 28);
    CHECK_EQ (transposeMelody (one, 2, bass).notes[0].note, 30);
    CHECK_EQ (transposeMelody (one, -3, bass).notes[0].note, 25);   // 28 - 3: still inside 24..72
    CHECK_EQ (transposeMelody (one, 7, bass).notes[0].note, 35);    // 28 - 5 = 23 would leave the range: one octave less
    CHECK_EQ (transposeMelody (mel ({ mn (70, 0.0, 1.0) }), 5, bass).notes[0].note, 63);   // 75 > 72

    const auto before = prog ({ { C, 4 }, { G, 4 } });
    const auto after = prog ({ { G, 4 }, { C, 4 } });
    const auto moved = melodyFollowingSlots (mel ({ mn (28, 1.0, 1.0), mn (31, 5.0, 1.0) }), before, after, { 1, 0 }, bass);
    REQUIRE (moved.notes.size() == 2);
    CHECK_EQ (moved.notes[0].note, 31);
    CHECK_EQ (moved.notes[1].note, 28);
    CHECK_EQ (moved.notes[1].startBeat, 5.0);

    // the layer follow with the bass layer keeps it too (edited slot, same chord)
    const auto ctx = plainContext (before, before, { 0, 1 });
    const auto kept = layerNotesFollowing (m, Layer::Bass, ctx);
    CHECK (kept == sanitiseMelody (m, bass));
}

TEST_CASE ("note limits: a 1024-note arp list keeps every note and 3000 notes cap at 2048")
{
    Melody arp;

    for (int i = 0; i < 1024; ++i)
        arp.notes.push_back (mn (60 + i % 12, i * 0.25, 0.25));

    const auto limits = layerLimits (Layer::Arp);
    const auto kept = sanitiseMelody (arp, limits);
    CHECK_EQ (kept.notes.size(), (size_t) 1024);
    CHECK (kept == arp);
    CHECK_EQ (sanitiseMelody (arp).notes.size(), (size_t) kMaxMelodyNotes);   // the melody cap would have cut it

    Melody big;

    for (int i = 0; i < 3000; ++i)
        big.notes.push_back (mn (40 + i / 1000, (i % 1000) * 0.25, 0.25));

    const auto capped = sanitiseMelody (big, limits);
    CHECK_EQ (capped.notes.size(), (size_t) kMaxLayerNotes);

    // the survivors are the first 2048 in canonical order
    const auto all = sanitiseMelody (big, NoteLimits { 28, 108, 100000 });
    CHECK_EQ (all.notes.size(), (size_t) 3000);
    CHECK (std::equal (capped.notes.begin(), capped.notes.end(), all.notes.begin()));

    CHECK_EQ (renderMelody (arp, 256.0, 4, limits).size(), (size_t) 1024);

    // the follow keeps all of them (those past the old loop end keep their distance to it)
    const auto same = prog ({ { C, 16 } });
    CHECK_EQ (melodyFollowingSlots (arp, same, same, { 0 }, limits).notes.size(), (size_t) 1024);
}

//==============================================================================
TEST_CASE ("notesFromEvents: rounding to 1/48, folding, minimum length, non-finite times dropped")
{
    auto ev = [] (int note, double start, double length, float velocity = 0.7f, int channel = 2)
    {
        NoteEvent e;
        e.note = note;
        e.startBeat = start;
        e.lengthBeats = length;
        e.velocity = velocity;
        e.channel = channel;
        return e;
    };

    const auto bass = layerLimits (Layer::Bass);

    // 0.26 beats = 12.48 ticks -> 12 ticks = 0.25; length 0.9 = 43.2 ticks -> 43 ticks
    auto m = notesFromEvents ({ ev (40, 0.26, 0.9) }, bass);
    REQUIRE (m.notes.size() == 1);
    CHECK_EQ (m.notes[0].startBeat, 12.0 / 48.0);
    CHECK_EQ (m.notes[0].lengthBeats, 43.0 / 48.0);
    CHECK_EQ (m.notes[0].note, 40);
    CHECK_EQ (m.notes[0].velocity, 0.7f);

    // 1/8T starts are exact ticks; swung times round to the nearest tick (1.2917 beats = 62 ticks)
    m = notesFromEvents ({ ev (40, 1.0 / 3.0, 0.2), ev (41, 1.0 + 0.25 + 0.25 / 3.0 * 0.5, 0.2) }, bass);
    REQUIRE (m.notes.size() == 2);
    CHECK_EQ (m.notes[0].startBeat, 16.0 / 48.0);
    CHECK_EQ (m.notes[1].startBeat, 62.0 / 48.0);

    // minimum length
    m = notesFromEvents ({ ev (40, 0.0, 0.001), ev (42, 1.0, 0.0) }, bass);
    REQUIRE (m.notes.size() == 2);
    CHECK_EQ (m.notes[0].lengthBeats, kMinMelodyNoteBeats);
    CHECK_EQ (m.notes[1].lengthBeats, kMinMelodyNoteBeats);

    // octave folding into the layer range (bass 24..72, arp 28..108)
    m = notesFromEvents ({ ev (20, 0.0, 1.0), ev (100, 1.0, 1.0), ev (127, 2.0, 1.0), ev (0, 3.0, 1.0) }, bass);
    REQUIRE (m.notes.size() == 4);
    CHECK_EQ (m.notes[0].note, 32);    // 20 + 12
    CHECK_EQ (m.notes[1].note, 64);    // 100 - 36
    CHECK_EQ (m.notes[2].note, 67);    // 127 - 60
    CHECK_EQ (m.notes[3].note, 24);    // 0 + 24

    m = notesFromEvents ({ ev (21, 0.0, 1.0), ev (115, 1.0, 1.0) }, layerLimits (Layer::Arp));
    REQUIRE (m.notes.size() == 2);
    CHECK_EQ (m.notes[0].note, 33);
    CHECK_EQ (m.notes[1].note, 103);

    // non-finite times are dropped
    m = notesFromEvents ({ ev (40, kNaN, 1.0), ev (41, 0.0, kNaN), ev (42, kInf, 1.0), ev (43, 0.0, kInf), ev (44, 2.0, 1.0) },
                        bass);
    REQUIRE (m.notes.size() == 1);
    CHECK_EQ (m.notes[0].note, 44);

    // canonical output: sorted, one note per (pitch, start), any channel accepted, empty list -> empty
    m = notesFromEvents ({ ev (50, 2.0, 1.0, 0.5f, 4), ev (45, 0.0, 1.0, 0.5f, 1), ev (45, 0.0, 2.0, 0.5f, 1), ev (47, 0.0, 1.0) },
                        bass);
    REQUIRE (m.notes.size() == 3);
    CHECK_EQ (m.notes[0].note, 45);
    CHECK_EQ (m.notes[0].lengthBeats, 2.0);   // the longest of the same pitch and start
    CHECK_EQ (m.notes[1].note, 47);
    CHECK_EQ (m.notes[2].note, 50);
    CHECK (m == sanitiseMelody (m, bass));
    CHECK (notesFromEvents ({}, bass).empty());

    // a negative start is clamped by the sanitising
    m = notesFromEvents ({ ev (40, -0.5, 1.0) }, bass);
    REQUIRE (m.notes.size() == 1);
    CHECK_EQ (m.notes[0].startBeat, 0.0);

    // the 2048 cap
    std::vector<NoteEvent> many;
    for (int i = 0; i < 3000; ++i)
        many.push_back (ev (40 + i / 1000, (i % 1000) * 0.25, 0.25));
    CHECK_EQ (notesFromEvents (many, bass).notes.size(), (size_t) kMaxLayerNotes);
}

TEST_CASE ("sameStructure: same slot count and lengths, chords may differ")
{
    const auto a = prog ({ { C, 4 }, { Am, 4 }, { F, 2 } });
    CHECK (sameStructure (a, a));
    CHECK (sameStructure (a, prog ({ { G, 4 }, { Dm, 4 }, { Cm, 2 } })));
    CHECK (! sameStructure (a, prog ({ { C, 4 }, { Am, 4 }, { F, 3 } })));
    CHECK (! sameStructure (a, prog ({ { C, 4 }, { Am, 4 } })));
    CHECK (! sameStructure (a, prog ({ { C, 4 }, { Am, 4 }, { F, 2 }, { G, 2 } })));
    CHECK (sameStructure (Progression {}, Progression {}));
    CHECK (! sameStructure (Progression {}, a));
}

//==============================================================================
TEST_CASE ("remapPitchToChord: roles, nearest octave, snapping to the key, folding")
{
    const auto bass = layerLimits (Layer::Bass);
    const auto arp = layerLimits (Layer::Arp);

    // C -> Am: root, 3rd, 5th (the nearest octave of A, C, E)
    CHECK_EQ (remapPitchToChord (60, C, 0, Am, 9, cMajor, arp), 57);
    CHECK_EQ (remapPitchToChord (64, C, 0, Am, 9, cMajor, arp), 60);
    CHECK_EQ (remapPitchToChord (67, C, 0, Am, 9, cMajor, arp), 64);

    // C -> Cm: the 3rd follows (E -> Eb)
    CHECK_EQ (remapPitchToChord (64, C, 0, Cm, 0, cMajor, arp), 63);
    CHECK_EQ (remapPitchToChord (60, C, 0, Cm, 0, cMajor, arp), 60);
    CHECK_EQ (remapPitchToChord (67, C, 0, Cm, 0, cMajor, arp), 67);

    // Cmaj7 -> Am: the 7th has no counterpart in a triad and goes to tone 0 (A)
    CHECK_EQ (remapPitchToChord (71, Cmaj7, 0, Am, 9, cMajor, arp), 69);
    CHECK_EQ (remapPitchToChord (67, Cmaj7, 0, Am, 9, cMajor, arp), 64);
    // and a triad -> Cmaj7 keeps the first three tones
    CHECK_EQ (remapPitchToChord (67, C, 0, Cmaj7, 0, cMajor, arp), 67);

    // C -> F/A: the bass C goes to the new bass A, the rest follows the rotated order (A C F)
    CHECK_EQ (remapPitchToChord (48, C, 0, F, 9, cMajor, bass), 45);
    CHECK_EQ (remapPitchToChord (64, C, 0, F, 9, cMajor, arp), 60);   // E = tone 1 -> C
    CHECK_EQ (remapPitchToChord (67, C, 0, F, 9, cMajor, arp), 65);   // G = tone 2 -> F

    // C/E -> Am: the bass E goes to A
    CHECK_EQ (remapPitchToChord (52, C, 4, Am, 9, cMajor, bass), 57);

    // a 9th keeps its interval above the root (B is the 9th of Am; the 9th of F is G)
    CHECK_EQ (remapPitchToChord (71, Am, 9, F, 5, cMajor, arp), 67);
    // D over C (interval 2) -> Am: B
    CHECK_EQ (remapPitchToChord (62, C, 0, Am, 9, cMajor, arp), 59);

    // a result outside the key and not a tone of the chord moves to the nearest scale tone, ties down:
    // C# (interval 1 over C) -> Am gives Bb, which is neither in C major nor in Am: A and B are equally near, A wins
    CHECK_EQ (remapPitchToChord (61, C, 0, Am, 9, cMajor, arp), 57);
    // C# over C -> G gives G#: neither in the key nor in G; F# / G are not scale... G (7) is the nearest scale tone
    CHECK_EQ (remapPitchToChord (61, C, 0, G, 7, cMajor, arp), 55);
    // a result that is a tone of the new chord stays even outside the key: E over Cm (not a Cm tone) -> E7 is G#
    CHECK_EQ (remapPitchToChord (64, Cm, 0, E7, 4, cMajor, arp), 68);
    // ... while Em has no G#, so the same note snaps to the key (G)
    CHECK_EQ (remapPitchToChord (64, Cm, 0, Em, 4, cMajor, arp), 67);
    CHECK_EQ (remapPitchToChord (64, Cm, 0, E7, 4, aMinor, arp), 68);

    // ties between octaves go down (a tritone away)
    CHECK_EQ (remapPitchToChord (60, C, 0, Chord { 6, ChordQuality::Major }, 6, cMajor, arp), 54);

    // range folding after the octave choice
    {
        const NoteLimits tight { 36, 60, 100 };
        CHECK_EQ (remapPitchToChord (36, C, 0, Am, 9, cMajor, tight), 45);   // A 33 -> 45
        CHECK_EQ (remapPitchToChord (60, C, 0, Em, 4, cMajor, tight), 52);   // E 64 -> 52
    }

    // identity: the note folded into the limits
    CHECK_EQ (remapPitchToChord (50, C, 0, C, 0, cMajor, bass), 50);
    CHECK_EQ (remapPitchToChord (20, C, 0, C, 0, cMajor, bass), 32);
    CHECK_EQ (remapPitchToChord (100, C, 4, C, 4, cMajor, bass), 64);
    CHECK_EQ (remapPitchToChord (100, C, 4, C, 4, cMajor, arp), 100);

    // inversion-aware order: C/E arp E G C -> Am gives A C E; F/A arp A C F -> C gives C E G
    {
        std::vector<int> out;
        for (int n : { 64, 67, 72 })
            out.push_back (remapPitchToChord (n, C, 4, Am, 9, cMajor, arp));
        CHECK_EQ (out, (std::vector<int> { 69, 72, 76 }));

        out.clear();
        for (int n : { 57, 60, 65 })
            out.push_back (remapPitchToChord (n, F, 9, C, 0, cMajor, arp));
        CHECK_EQ (out, (std::vector<int> { 60, 64, 67 }));

        // a RootFifth bass over C/E (E, C) -> F gives F, C
        CHECK_EQ (remapPitchToChord (40, C, 4, F, 5, cMajor, bass), 41);
        CHECK_EQ (remapPitchToChord (48, C, 4, F, 5, cMajor, bass), 48);
    }

    // every remapped pitch stays in the limits, whatever the chords
    Rng rng (0x5150u);
    for (int i = 0; i < 2000; ++i)
    {
        const Chord from { rng.range (0, 11), (ChordQuality) rng.range (0, numChordQualities - 1) };
        const Chord to { rng.range (0, 11), (ChordQuality) rng.range (0, numChordQualities - 1) };
        const auto limits = layerLimits ((Layer) rng.range (0, 2));
        const int note = rng.range (limits.lowest, limits.highest);
        const int moved = remapPitchToChord (note, from, rng.range (0, 11), to, rng.range (0, 11),
                                             Key { rng.range (0, 11), (ScaleType) rng.range (0, numScaleTypes - 1) }, limits);
        CHECK (moved >= limits.lowest && moved <= limits.highest);
    }
}

//==============================================================================
TEST_CASE ("layerNotesFollowing: Melody is exactly melodyFollowingSlots under random structural edits")
{
    Rng rng (0xfeed01u);

    for (int iter = 0; iter < 400; ++iter)
    {
        const auto before = randomProgression (rng, 1, 7);
        const auto edit = randomEdit (rng, before);

        Melody m;
        const int count = rng.range (0, 30);
        for (int i = 0; i < count; ++i)
            m.notes.push_back (mn (rng.range (40, 90), rng.range (0, (before.totalBeats() + 8) * 4) * 0.25, 0.5));

        auto ctx = plainContext (before, edit.after, edit.source);
        // the autos and bass pcs must be ignored for the melody
        ctx.autoAfter = mel ({ mn (60, 0.0, 1.0) });
        ctx.autoBefore = mel ({ mn (62, 0.0, 1.0) });
        ctx.beforeBassPc = { 3, 4, 5 };

        CHECK (layerNotesFollowing (m, Layer::Melody, ctx) == melodyFollowingSlots (m, before, edit.after, edit.source));
    }
}

TEST_CASE ("layerNotesFollowing: Arp and Bass timing equals melody timing for edited slots (random structural edits)")
{
    Rng rng (0xbeef02u);

    for (int iter = 0; iter < 400; ++iter)
    {
        const Layer layer = rng.chance (50) ? Layer::Arp : Layer::Bass;
        const auto limits = layerLimits (layer);
        const auto before = randomProgression (rng, 1, 7);
        const auto edit = randomEdit (rng, before);

        // every slot holds notes that differ from the (empty) auto notes: every slot is edited
        Melody custom;
        for (size_t s = 0; s < before.slots.size(); ++s)
        {
            const double start = before.slotStartBeat ((int) s);
            const int count = rng.range (1, 3);
            for (int i = 0; i < count; ++i)
                custom.notes.push_back (mn (rng.range (limits.lowest, 60),
                                            start + rng.range (0, before.slots[s].lengthBeats * 4 - 1) * 0.25, 0.5));
        }

        // parked notes past the end of the old loop
        for (int i = rng.range (0, 2); i > 0; --i)
            custom.notes.push_back (mn (rng.range (limits.lowest, 60), before.totalBeats() + rng.range (0, 12) * 0.25, 0.5));

        const auto ctx = plainContext (before, edit.after, edit.source);
        const auto followed = layerNotesFollowing (custom, layer, ctx);
        const auto expected = melodyFollowingSlots (custom, before, edit.after, edit.source, limits);

        CHECK (followed == expected);
    }
}

TEST_CASE ("layerNotesFollowing: insert, remove, move, duplicate and length edits of edited slots")
{
    const auto before = prog ({ { C, 4 }, { Am, 4 }, { F, 4 }, { G, 4 } });

    // one note per beat of every slot: all four slots are edited (the auto notes are empty)
    Melody custom;
    for (int b = 0; b < 16; ++b)
        custom.notes.push_back (mn (30 + (b / 4) * 2, (double) b, 0.5));

    struct Case
    {
        const char* what;
        Progression after;
        std::vector<int> source;
    };

    std::vector<Case> cases;
    cases.push_back ({ "insert", prog ({ { C, 4 }, { Am, 4 }, { Dm, 4 }, { F, 4 }, { G, 4 } }), { 0, 1, -1, 2, 3 } });
    cases.push_back ({ "remove", prog ({ { C, 4 }, { F, 4 }, { G, 4 } }), { 0, 2, 3 } });
    cases.push_back ({ "move", prog ({ { C, 4 }, { F, 4 }, { Am, 4 }, { G, 4 } }), { 0, 2, 1, 3 } });
    cases.push_back ({ "duplicate", prog ({ { C, 4 }, { Am, 4 }, { Am, 4 }, { F, 4 }, { G, 4 } }), { 0, 1, 1, 2, 3 } });
    cases.push_back ({ "shorter", prog ({ { C, 4 }, { Am, 2 }, { F, 4 }, { G, 4 } }), { 0, 1, 2, 3 } });
    cases.push_back ({ "longer", prog ({ { C, 4 }, { Am, 8 }, { F, 4 }, { G, 4 } }), { 0, 1, 2, 3 } });

    for (const auto& c : cases)
    {
        for (Layer layer : { Layer::Arp, Layer::Bass })
        {
            const auto ctx = plainContext (before, c.after, c.source);
            const auto followed = layerNotesFollowing (custom, layer, ctx);
            const auto expected = melodyFollowingSlots (custom, before, c.after, c.source, layerLimits (layer));

            if (! (followed == expected))
                cbtest::reportFailure (__FILE__, __LINE__, std::string ("timing differs from the melody follow: ") + c.what);
        }
    }

    // spot checks of the timing
    {
        const auto ctx = plainContext (before, cases[0].after, cases[0].source);   // insert a slot at beat 8
        const auto r = layerNotesFollowing (custom, Layer::Bass, ctx);
        REQUIRE (r.notes.size() == 16);
        CHECK_EQ (r.notes[8].startBeat, 12.0);   // the old beat 8 (F) now starts at 12
        CHECK_EQ (r.notes[8].note, 34);
        CHECK_EQ (slice (r, 8.0, 12.0).notes.size(), (size_t) 0);   // the new slot starts empty (autoAfter is empty)
    }
    {
        const auto ctx = plainContext (before, cases[3].after, cases[3].source);   // duplicate Am
        const auto r = layerNotesFollowing (custom, Layer::Bass, ctx);
        CHECK_EQ (r.notes.size(), (size_t) 20);
        CHECK (! slice (r, 4.0, 8.0).notes.empty());
        CHECK (slice (r, 4.0, 8.0) == slice (r, 8.0, 12.0, -4.0));
    }
    {
        const auto ctx = plainContext (before, cases[4].after, cases[4].source);   // Am shrinks to 2 beats
        const auto r = layerNotesFollowing (custom, Layer::Bass, ctx);
        CHECK_EQ (slice (r, 4.0, 6.0).notes.size(), (size_t) 2);   // the notes at offsets 2 and 3 no longer fit
        CHECK_EQ (r.notes.size(), (size_t) 14);
    }
}

TEST_CASE ("layerNotesFollowing: an in-place chord change remaps only that slot")
{
    const auto before = prog ({ { C, 4 }, { Am, 4 }, { F, 4 }, { G, 4 } });
    const auto after = prog ({ { C, 4 }, { Am, 4 }, { Dm, 4 }, { G, 4 } });   // F -> Dm in place
    const auto limits = layerLimits (Layer::Arp);

    // an arp-like line of chord tones in every slot (auto notes empty: every slot is edited)
    const int tones[4][3] = { { 60, 64, 67 }, { 57, 60, 64 }, { 65, 69, 72 }, { 67, 71, 74 } };
    Melody custom;
    for (int s = 0; s < 4; ++s)
        for (int i = 0; i < 3; ++i)
            custom.notes.push_back (mn (tones[s][i], s * 4.0 + i, 0.5));

    const auto ctx = plainContext (before, after, { 0, 1, 2, 3 });
    const auto r = layerNotesFollowing (custom, Layer::Arp, ctx);

    CHECK_EQ (r.notes.size(), (size_t) 12);
    CHECK (slice (r, 0.0, 8.0) == slice (custom, 0.0, 8.0));       // slots 0, 1 untouched
    CHECK (slice (r, 12.0, 16.0) == slice (custom, 12.0, 16.0));   // slot 3 untouched

    // F A C -> D F A (root, 3rd, 5th)
    const auto slot2 = slice (r, 8.0, 12.0);
    REQUIRE (slot2.notes.size() == 3);
    CHECK_EQ (slot2.notes[0].note, remapPitchToChord (65, F, 5, Dm, 2, cMajor, limits));
    CHECK_EQ (slot2.notes[0].note, 62);
    CHECK_EQ (slot2.notes[1].note, 65);
    CHECK_EQ (slot2.notes[2].note, 69);
    CHECK_EQ (slot2.notes[0].startBeat, 8.0);
    CHECK_EQ (slot2.notes[1].startBeat, 9.0);

    // a different bass pitch class with the same chord also remaps (the slot went to an inversion)
    auto inv = plainContext (before, before, { 0, 1, 2, 3 });
    inv.beforeBassPc = { 0, 9, 5, 7 };
    inv.afterBassPc = { 0, 9, 9, 7 };   // slot 2: F over A
    const auto r2 = layerNotesFollowing (custom, Layer::Arp, inv);
    CHECK (slice (r2, 0.0, 8.0) == slice (custom, 0.0, 8.0));
    CHECK (slice (r2, 12.0, 16.0) == slice (custom, 12.0, 16.0));
    const auto slot2b = slice (r2, 8.0, 12.0);
    REQUIRE (slot2b.notes.size() == 3);
    CHECK_EQ (slot2b.notes[0].note, 69);   // the bass F goes to the new bass A
    CHECK_EQ (slot2b.notes[1].note, 72);   // A = tone 1 of F A C -> tone 1 of A C F = C
    CHECK_EQ (slot2b.notes[2].note, 77);   // C = tone 2 -> F (5 up beats 7 down)

    // with an in-place change in two slots, both slots are remapped, the others are not
    const auto after2 = prog ({ { Am, 4 }, { Am, 4 }, { Dm, 4 }, { G, 4 } });
    const auto r3 = layerNotesFollowing (custom, Layer::Arp, plainContext (before, after2, { 0, 1, 2, 3 }));
    CHECK (slice (r3, 4.0, 8.0) == slice (custom, 4.0, 8.0));
    CHECK (slice (r3, 12.0, 16.0) == slice (custom, 12.0, 16.0));
    CHECK_EQ (pc (slice (r3, 0.0, 4.0).notes[0].note), 9);   // C -> A
    CHECK_EQ (pc (slice (r3, 8.0, 12.0).notes[0].note), 2);  // F -> D
}

TEST_CASE ("layerNotesFollowing: new slots from autoAfter, parked notes keep their distance, size mismatch is unmoved")
{
    const auto before = prog ({ { C, 4 }, { Am, 4 } });
    const auto bass = layerLimits (Layer::Bass);

    // a new slot in the middle and one appended at the end
    {
        const auto custom = mel ({ mn (30, 0.0, 1.0), mn (33, 4.0, 1.0) });
        const auto after = prog ({ { C, 4 }, { G, 4 }, { Am, 4 }, { F, 4 } });
        auto ctx = plainContext (before, after, { 0, -1, 1, -1 });
        ctx.autoAfter = mel ({ mn (31, 0.0, 1.0), mn (43, 4.0, 1.0), mn (43, 6.0, 1.0), mn (45, 8.0, 1.0), mn (41, 12.0, 1.0),
                               mn (41, 14.0, 1.0) });
        ctx.autoBefore = mel ({ mn (31, 0.0, 1.0), mn (45, 4.0, 1.0) });   // both slots edited: 30 vs 31 and 33 vs 45

        const auto r = layerNotesFollowing (custom, Layer::Bass, ctx);
        const auto expected = joined ({ slice (custom, 0.0, 4.0), slice (ctx.autoAfter, 4.0, 8.0),
                                        slice (custom, 4.0, 8.0, 4.0), slice (ctx.autoAfter, 12.0, 16.0) }, bass);
        CHECK (r == expected);
        CHECK_EQ (r.notes.size(), (size_t) 6);
    }

    // parked notes (at or after the old loop end) keep their distance to it and are never remapped
    {
        const auto custom = mel ({ mn (30, 0.0, 1.0), mn (31, 8.0, 1.0), mn (32, 9.5, 0.5), mn (60, 12.0, 1.0) });
        const auto after = prog ({ { Dm, 4 }, { Am, 4 }, { F, 4 } });   // slot 0 changes chord, one slot appended
        const auto r = layerNotesFollowing (custom, Layer::Bass, plainContext (before, after, { 0, 1, -1 }));

        // the old loop ended at 8 and the new one ends at 12: parked notes move by +4
        const auto parked = slice (r, 12.0, 100.0);
        REQUIRE (parked.notes.size() == 3);
        CHECK_EQ (parked.notes[0].startBeat, 12.0);
        CHECK_EQ (parked.notes[0].note, 31);
        CHECK_EQ (parked.notes[1].startBeat, 13.5);
        CHECK_EQ (parked.notes[1].note, 32);
        CHECK_EQ (parked.notes[2].startBeat, 16.0);
        CHECK_EQ (parked.notes[2].note, 60);

        // the edited slot 0 was remapped (C -> Dm)
        const auto first = slice (r, 0.0, 4.0);
        REQUIRE (first.notes.size() == 1);
        CHECK_EQ (first.notes[0].note, remapPitchToChord (30, C, 0, Dm, 2, cMajor, bass));
    }

    // parked notes also stay parked, unremapped, for a removal
    {
        const auto custom = mel ({ mn (61, 9.0, 1.0) });
        const auto after = prog ({ { C, 4 } });
        const auto r = layerNotesFollowing (custom, Layer::Bass, plainContext (before, after, { 0 }));
        REQUIRE (r.notes.size() == 1);
        CHECK_EQ (r.notes[0].startBeat, 5.0);   // 1 beat past the old end (8) -> 1 beat past the new end (4)
        CHECK_EQ (r.notes[0].note, 61);
    }

    // size mismatch: sanitised for the layer but unmoved
    {
        const auto after = prog ({ { C, 4 }, { Am, 4 }, { F, 4 } });
        auto ctx = plainContext (before, after, { 0, 1 });
        ctx.autoAfter = mel ({ mn (50, 8.0, 1.0) });
        const auto messy = mel ({ mn (28, 4.0, 1.0), mn (10, 0.0, 1.0), mn (200, 1.0, 1.0) });
        CHECK (layerNotesFollowing (messy, Layer::Bass, ctx) == sanitiseMelody (messy, bass));
        CHECK (layerNotesFollowing (messy, Layer::Arp, ctx) == sanitiseMelody (messy, layerLimits (Layer::Arp)));
        CHECK (layerNotesFollowing (messy, Layer::Melody, ctx) == sanitiseMelody (messy));
    }
}

TEST_CASE ("layerNotesFollowing: untouched slots follow the auto pattern (rule 0) with real renders")
{
    const auto before = prog ({ { C, 4 }, { Am, 4 }, { F, 4 }, { G, 4 } });

    for (Layer layer : { Layer::Bass, Layer::Arp })
    {
        const AutoRender render { layer, BassMode::RootFifth };
        const auto limits = layerLimits (layer);
        const auto autoBefore = autoNotesFor (render, before);
        REQUIRE (autoBefore.notes.size() > 8);

        // custom = the auto notes with the first note of slot 0 an octave up: only slot 0 is edited
        Melody custom = autoBefore;
        custom.notes[0].note += 12;
        custom = sanitiseMelody (custom, limits);

        const std::vector<bool> expectedEdited { true, false, false, false };
        CHECK (editedSlots (custom, autoBefore, before) == expectedEdited);

        // (A) an in-place chord change in another slot: slot 0 keeps its edit, every other slot takes the new auto notes
        {
            const auto after = prog ({ { C, 4 }, { Am, 4 }, { Dm, 4 }, { G, 4 } });
            const auto ctx = realContext (render, before, after, { 0, 1, 2, 3 });
            const auto r = layerNotesFollowing (custom, layer, ctx);

            CHECK (r == joined ({ slice (custom, 0.0, 4.0), slice (ctx.autoAfter, 4.0, 16.0) }, limits));
            CHECK (editedSlots (r, ctx.autoAfter, after) == expectedEdited);
            CHECK (slice (ctx.autoAfter, 8.0, 12.0) != slice (autoBefore, 8.0, 12.0));   // the change was audible
        }

        // (B) a slot that moves takes autoAfter (the untouched Am moves to the front); the edited slot moves with its notes
        {
            const auto after = prog ({ { Am, 4 }, { C, 4 }, { F, 4 }, { G, 4 } });
            const auto ctx = realContext (render, before, after, { 1, 0, 2, 3 });
            const auto r = layerNotesFollowing (custom, layer, ctx);

            CHECK (r == joined ({ slice (ctx.autoAfter, 0.0, 4.0), slice (custom, 0.0, 4.0, 4.0),
                                  slice (ctx.autoAfter, 8.0, 16.0) }, limits));
            const std::vector<bool> movedEdited { false, true, false, false };
            CHECK (editedSlots (r, ctx.autoAfter, after) == movedEdited);
        }

        // (C) the edited slot itself changes chord: its notes are remapped, the rest is auto
        {
            const auto after = prog ({ { Am, 4 }, { Am, 4 }, { F, 4 }, { G, 4 } });
            const auto ctx = realContext (render, before, after, { 0, 1, 2, 3 });
            const auto r = layerNotesFollowing (custom, layer, ctx);

            Melody remapped;
            for (auto n : slice (custom, 0.0, 4.0).notes)
            {
                n.note = remapPitchToChord (n.note, C, ctx.beforeBassPc[0], Am, ctx.afterBassPc[0], cMajor, limits);
                remapped.notes.push_back (n);
            }

            CHECK (r == joined ({ remapped, slice (ctx.autoAfter, 4.0, 16.0) }, limits));
        }

        // (D) a duplicate of the edited slot copies the edit; the untouched slots shift and take autoAfter
        {
            const auto after = prog ({ { C, 4 }, { C, 4 }, { Am, 4 }, { F, 4 }, { G, 4 } });
            const auto ctx = realContext (render, before, after, { 0, 0, 1, 2, 3 });
            const auto r = layerNotesFollowing (custom, layer, ctx);

            CHECK (r == joined ({ slice (custom, 0.0, 4.0), slice (custom, 0.0, 4.0, 4.0), slice (ctx.autoAfter, 8.0, 20.0) },
                                limits));
        }

        // nothing edited: the result is the new auto render (the processor then makes the layer auto)
        {
            const auto after = prog ({ { C, 4 }, { Am, 4 }, { Dm, 4 }, { G, 4 }, { C, 2 } });
            const auto ctx = realContext (render, before, after, { 0, 1, 2, 3, -1 });
            const auto r = layerNotesFollowing (autoBefore, layer, ctx);

            CHECK (r == sanitiseMelody (ctx.autoAfter, limits));
        }
    }
}

TEST_CASE ("layerNotesFollowing: a lengthened edited slot gets the auto notes of its new tail (rule 3b)")
{
    const auto before = prog ({ { C, 4 }, { G, 4 } });
    const auto after = prog ({ { C, 8 }, { G, 4 } });   // slot 0: 4 -> 8 beats

    for (Layer layer : { Layer::Bass, Layer::Arp })
    {
        const AutoRender render { layer, BassMode::RootFifth };
        const auto limits = layerLimits (layer);
        const auto autoBefore = autoNotesFor (render, before);
        REQUIRE (! autoBefore.notes.empty());

        Melody custom = autoBefore;
        custom.notes[0].note += 12;
        custom = sanitiseMelody (custom, limits);

        const auto ctx = realContext (render, before, after, { 0, 1 });
        const auto r = layerNotesFollowing (custom, layer, ctx);

        const auto tail = slice (ctx.autoAfter, 4.0, 8.0);
        CHECK (! tail.notes.empty());
        CHECK (slice (r, 0.0, 4.0) == slice (custom, 0.0, 4.0));
        CHECK (slice (r, 4.0, 8.0) == tail);
        CHECK (slice (r, 8.0, 12.0) == slice (ctx.autoAfter, 8.0, 12.0));   // the untouched G slot takes autoAfter
        CHECK (r == joined ({ slice (custom, 0.0, 4.0), tail, slice (ctx.autoAfter, 8.0, 12.0) }, limits));

        // an unedited slot that grows is simply the auto render
        CHECK (layerNotesFollowing (autoBefore, layer, ctx) == sanitiseMelody (ctx.autoAfter, limits));

        // a slot that shrinks gets no tail notes
        const auto shorter = prog ({ { C, 2 }, { G, 4 } });
        const auto ctx2 = realContext (render, before, shorter, { 0, 1 });
        const auto r2 = layerNotesFollowing (custom, layer, ctx2);
        CHECK (slice (r2, 0.0, 2.0) == slice (custom, 0.0, 2.0));
        CHECK (r2 == joined ({ slice (custom, 0.0, 2.0), slice (ctx2.autoAfter, 2.0, 6.0) }, limits));
    }
}

TEST_CASE ("layerNotesFollowing: custom bass notes at MIDI 28 and arp lists over 512 notes survive")
{
    const auto before = prog ({ { C, 4 }, { G, 4 } });
    const auto after = prog ({ { G, 4 }, { C, 4 } });
    const auto ctx = plainContext (before, after, { 1, 0 });

    // slot 1 (G) moves to the front, slot 0 (C) to beat 4; same chords, so nothing is remapped
    const auto bassFollowed = layerNotesFollowing (mel ({ mn (28, 0.0, 1.0), mn (24, 4.0, 1.0) }), Layer::Bass, ctx);
    REQUIRE (bassFollowed.notes.size() == 2);
    CHECK_EQ (bassFollowed.notes[0].startBeat, 0.0);
    CHECK_EQ (bassFollowed.notes[0].note, 24);
    CHECK_EQ (bassFollowed.notes[1].startBeat, 4.0);
    CHECK_EQ (bassFollowed.notes[1].note, 28);

    Melody arp;
    for (int i = 0; i < 1024; ++i)
        arp.notes.push_back (mn (60 + i % 12, i * 0.25, 0.25));
    arp = sanitiseMelody (arp, layerLimits (Layer::Arp));
    const auto same = plainContext (prog ({ { C, 16 }, { G, 16 } }), prog ({ { C, 16 }, { G, 16 } }), { 0, 1 });
    const auto kept = layerNotesFollowing (arp, Layer::Arp, same);
    CHECK_EQ (kept.notes.size(), (size_t) 1024);
    CHECK (kept == arp);

    // 1500 notes (more than the melody cap) survive an edit that keeps the structure
    Melody wide;
    for (int i = 0; i < 1500; ++i)
        wide.notes.push_back (mn (40 + i / 125, (i % 125) * 0.25, 0.25));
    wide = sanitiseMelody (wide, layerLimits (Layer::Arp));
    CHECK_EQ (wide.notes.size(), (size_t) 1500);
    CHECK (layerNotesFollowing (wide, Layer::Arp, same) == wide);
}

//==============================================================================
TEST_CASE ("editedSlots: per slot, notes outside the loop ignored")
{
    const auto bass = layerLimits (Layer::Bass);
    const auto p = prog ({ { C, 4 }, { Am, 4 }, { F, 4 } });
    const auto base = mel ({ mn (40, 0.0, 1.0), mn (45, 4.0, 1.0), mn (45, 6.0, 1.0), mn (41, 8.0, 1.0) });

    const std::vector<bool> none { false, false, false };
    CHECK (editedSlots (base, base, p) == none);
    CHECK (editedSlots (Melody {}, Melody {}, p) == none);

    // an added note, a removed note, a changed pitch / length / velocity: each flags only its slot
    auto edited = base;
    edited.notes.push_back (mn (47, 5.0, 1.0));
    edited = sanitiseMelody (edited, bass);
    CHECK (editedSlots (edited, base, p) == (std::vector<bool> { false, true, false }));

    edited = base;
    edited.notes.erase (edited.notes.begin() + 3);
    CHECK (editedSlots (edited, base, p) == (std::vector<bool> { false, false, true }));

    edited = base;
    edited.notes[0].note = 52;
    CHECK (editedSlots (edited, base, p) == (std::vector<bool> { true, false, false }));

    edited = base;
    edited.notes[1].lengthBeats = 0.5;
    CHECK (editedSlots (edited, base, p) == (std::vector<bool> { false, true, false }));

    edited = base;
    edited.notes[3].velocity = 0.3f;
    CHECK (editedSlots (edited, base, p) == (std::vector<bool> { false, false, true }));

    // notes that start outside the loop (and so in no slot) are ignored, on either side
    edited = base;
    edited.notes.push_back (mn (50, 12.0, 1.0));
    edited.notes.push_back (mn (50, 20.0, 1.0));
    CHECK (editedSlots (edited, base, p) == none);
    CHECK (editedSlots (base, edited, p) == none);

    // a note that starts in a slot but rings into the next belongs to its start slot
    edited = base;
    edited.notes[0].lengthBeats = 6.0;
    CHECK (editedSlots (edited, base, p) == (std::vector<bool> { true, false, false }));

    CHECK (editedSlots (base, base, Progression {}).empty());
}

TEST_CASE ("replaceSlotNotes: one slot takes its auto notes, the rest is unchanged, sanitised for the limits")
{
    const auto p = prog ({ { C, 4 }, { Am, 4 }, { F, 4 } });
    const auto bass = layerLimits (Layer::Bass);
    const auto custom = mel ({ mn (28, 0.0, 1.0), mn (36, 4.0, 1.0), mn (38, 5.0, 1.0), mn (29, 8.0, 1.0), mn (30, 20.0, 1.0) });
    const auto autoNotes = mel ({ mn (33, 0.0, 2.0), mn (33, 4.0, 2.0), mn (40, 6.0, 1.0), mn (41, 8.0, 2.0) });

    const auto r = replaceSlotNotes (custom, autoNotes, p, 1, bass);
    CHECK (r == sanitiseMelody (mel ({ mn (28, 0.0, 1.0), mn (33, 4.0, 2.0), mn (40, 6.0, 1.0), mn (29, 8.0, 1.0), mn (30, 20.0, 1.0) }),
                                bass));
    CHECK_EQ (r.notes[0].note, 28);   // a bass note below the melody range survives

    // the slot can end up empty (auto has no notes there)
    const auto r2 = replaceSlotNotes (custom, mel ({ mn (41, 8.0, 2.0) }), p, 0, bass);
    CHECK (slice (r2, 0.0, 4.0).notes.empty());
    CHECK_EQ (r2.notes.size(), (size_t) 4);

    // resetting every slot leaves "edited slots all false"
    const auto resetAll = replaceSlotNotes (replaceSlotNotes (replaceSlotNotes (custom, autoNotes, p, 0, bass), autoNotes, p, 1, bass),
                                            autoNotes, p, 2, bass);
    CHECK (editedSlots (slice (resetAll, 0.0, 12.0), autoNotes, p) == (std::vector<bool> { false, false, false }));

    // an invalid slot returns the custom notes sanitised
    CHECK (replaceSlotNotes (custom, autoNotes, p, -1, bass) == sanitiseMelody (custom, bass));
    CHECK (replaceSlotNotes (custom, autoNotes, p, 3, bass) == sanitiseMelody (custom, bass));
    CHECK (replaceSlotNotes (custom, autoNotes, p, 99, bass) == sanitiseMelody (custom, bass));

    // an unsanitised input comes back canonical
    const auto messy = mel ({ mn (200, 5.0, 1.0), mn (10, 0.0, 1.0) });
    CHECK (replaceSlotNotes (messy, autoNotes, p, 2, bass)
           == sanitiseMelody (mel ({ mn (10, 0.0, 1.0), mn (72, 5.0, 1.0), mn (41, 8.0, 2.0) }), bass));
}
