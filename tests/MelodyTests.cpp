#include "TestFramework.h"

#include "core/Melody.h"

#include <cmath>
#include <cstdint>
#include <limits>

// Tests for the melody model, harmonic-guide helpers and melody rendering (src/core/Melody.h).

using namespace cb;

namespace
{
constexpr double kTol = 1.0e-9;

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

bool near (double a, double b) { return std::abs (a - b) < kTol; }

bool onTick (double beats) { return near (beats * 48.0, std::round (beats * 48.0)); }

std::vector<double> starts (const Melody& m)
{
    std::vector<double> v;
    for (const auto& n : m.notes)
        v.push_back (n.startBeat);
    return v;
}

// Deterministic pseudo-random numbers for the property tests (no std::rand).
struct Lcg
{
    uint32_t state;
    uint32_t next() { state = state * 1664525u + 1013904223u; return state >> 8; }
    double unit() { return (double) next() / (double) (1u << 24); }
    int range (int lo, int hi) { return lo + (int) (next() % (uint32_t) (hi - lo + 1)); }
};

const Key cMajor { 0, ScaleType::Major };
const Key aMinor { 9, ScaleType::NaturalMinor };
const Key fSharpMajor { 6, ScaleType::Major };
} // namespace

//==============================================================================
TEST_CASE ("melody: sanitise clamps pitch, velocity, start and length")
{
    const auto m = sanitiseMelody (mel ({ mn (20, -3.0, 0.001, 2.0f), mn (120, 300.0, 100.0, -1.0f),
                                          mn (60, 1.01, 0.49, std::numeric_limits<float>::quiet_NaN()) }));
    REQUIRE (m.notes.size() == 3);

    CHECK_EQ (m.notes[0].note, kMelodyLowestNote);
    CHECK (near (m.notes[0].startBeat, 0.0));
    CHECK (near (m.notes[0].lengthBeats, kMinMelodyNoteBeats));
    CHECK_EQ (m.notes[0].velocity, 1.0f);

    CHECK_EQ (m.notes[1].note, 60);
    CHECK (near (m.notes[1].startBeat, 1.0));      // 1.01 rounds to the nearest 1/48 beat
    CHECK (near (m.notes[1].lengthBeats, 0.5));    // 0.49 -> 24/48
    CHECK_EQ (m.notes[1].velocity, kDefaultMelodyVelocity);

    CHECK_EQ (m.notes[2].note, kMelodyHighestNote);
    CHECK (near (m.notes[2].startBeat, kMaxMelodyBeat - kMinMelodyNoteBeats));
    CHECK (near (m.notes[2].startBeat + m.notes[2].lengthBeats, kMaxMelodyBeat));   // never past the longest loop
    CHECK_EQ (m.notes[2].velocity, 0.05f);
}

TEST_CASE ("melody: sanitise drops non-finite notes, sorts and keeps the longest of equal starts")
{
    const double inf = std::numeric_limits<double>::infinity();
    const auto m = sanitiseMelody (mel ({ mn (64, 2.0, 1.0), mn (60, inf, 1.0), mn (62, 1.0, std::nan ("")),
                                          mn (60, 2.0, 0.5), mn (64, 2.0, 3.0), mn (60, 0.0, 1.0) }));
    REQUIRE (m.notes.size() == 3);
    CHECK_EQ (starts (m), (std::vector<double> { 0.0, 2.0, 2.0 }));
    CHECK_EQ (m.notes[1].note, 60);
    CHECK_EQ (m.notes[2].note, 64);
    CHECK (near (m.notes[2].lengthBeats, 3.0));   // the longer of the two E notes at beat 2
}

TEST_CASE ("melody: sanitise truncates to kMaxMelodyNotes and is idempotent")
{
    Melody big;
    for (int i = 0; i < kMaxMelodyNotes + 40; ++i)
        big.notes.push_back (mn (48 + i % 24, (double) (i / 24) * 0.5, 0.25));

    const auto m = sanitiseMelody (big);
    CHECK_EQ ((int) m.notes.size(), kMaxMelodyNotes);
    CHECK (sanitiseMelody (m) == m);

    Lcg rng { 12345u };

    for (int round = 0; round < 200; ++round)
    {
        Melody r;
        const int count = rng.range (0, 40);

        for (int i = 0; i < count; ++i)
            r.notes.push_back (mn (rng.range (20, 110), rng.unit() * 300.0 - 20.0, rng.unit() * 70.0 - 2.0,
                                   (float) (rng.unit() * 1.4 - 0.2)));

        const auto s = sanitiseMelody (r);
        CHECK (sanitiseMelody (s) == s);

        for (size_t i = 0; i < s.notes.size(); ++i)
        {
            const auto& n = s.notes[i];
            CHECK (n.note >= kMelodyLowestNote && n.note <= kMelodyHighestNote);
            CHECK (n.startBeat >= 0.0 && n.startBeat + n.lengthBeats <= kMaxMelodyBeat + kTol);
            CHECK (n.lengthBeats >= kMinMelodyNoteBeats - kTol && n.lengthBeats <= kMaxMelodyNoteBeats + kTol);
            CHECK (onTick (n.startBeat) && onTick (n.lengthBeats));
            CHECK (n.velocity >= 0.05f && n.velocity <= 1.0f);

            if (i > 0)
            {
                const auto& p = s.notes[i - 1];
                CHECK (p.startBeat < n.startBeat || (p.startBeat == n.startBeat && p.note < n.note));
            }
        }
    }
}

TEST_CASE ("melody: note hit testing")
{
    const auto m = sanitiseMelody (mel ({ mn (60, 0.0, 2.0), mn (60, 1.0, 2.0), mn (64, 4.0, 1.0) }));
    CHECK_EQ (melodyNoteAt (m, 0.5, 60), 0);
    CHECK_EQ (melodyNoteAt (m, 1.5, 60), 1);    // overlapping notes: the later start is on top
    CHECK_EQ (melodyNoteAt (m, 2.99, 60), 1);
    CHECK_EQ (melodyNoteAt (m, 3.0, 60), -1);   // end is exclusive
    CHECK_EQ (melodyNoteAt (m, 4.0, 64), 2);
    CHECK_EQ (melodyNoteAt (m, 4.5, 65), -1);
    CHECK_EQ (melodyNoteAt (m, -0.1, 60), -1);
}

//==============================================================================
TEST_CASE ("melody guide: tone roles against key and chord")
{
    const Chord g7 { 7, ChordQuality::Dominant7 };
    CHECK_EQ (toneRole (7, cMajor, &g7), ToneRole::ChordRoot);
    CHECK_EQ (toneRole (11, cMajor, &g7), ToneRole::ChordTone);
    CHECK_EQ (toneRole (2, cMajor, &g7), ToneRole::ChordTone);
    CHECK_EQ (toneRole (5, cMajor, &g7), ToneRole::ChordTone);    // the 7th
    CHECK_EQ (toneRole (9, cMajor, &g7), ToneRole::Scale);
    CHECK_EQ (toneRole (1, cMajor, &g7), ToneRole::Outside);
    CHECK_EQ (toneRole (19, cMajor, &g7), ToneRole::ChordRoot);   // pitch classes wrap

    const Chord e7 { 4, ChordQuality::Dominant7 };
    CHECK_EQ (toneRole (8, aMinor, &e7), ToneRole::ChordTone);    // G# is a chord tone though outside A minor
    CHECK_EQ (toneRole (8, aMinor, nullptr), ToneRole::Outside);
    CHECK_EQ (toneRole (9, aMinor, nullptr), ToneRole::Scale);

    const Chord c9 { 0, ChordQuality::Dominant9 };
    CHECK_EQ (toneRole (2, cMajor, &c9), ToneRole::ChordTone);    // extensions count
    CHECK_EQ (toneRole (10, cMajor, &c9), ToneRole::ChordTone);
}

TEST_CASE ("melody guide: chord at a beat")
{
    const auto p = prog ({ { { 0, ChordQuality::Major }, 4 }, { { 7, ChordQuality::Major }, 2 } });
    CHECK (chordAtBeat (p, 0.0) == std::optional<Chord> (Chord { 0, ChordQuality::Major }));
    CHECK (chordAtBeat (p, 3.99) == std::optional<Chord> (Chord { 0, ChordQuality::Major }));
    CHECK (chordAtBeat (p, 4.0) == std::optional<Chord> (Chord { 7, ChordQuality::Major }));
    CHECK (! chordAtBeat (p, 6.0).has_value());
    CHECK (! chordAtBeat (p, -0.5).has_value());
    CHECK (! chordAtBeat ({}, 0.0).has_value());
}

//==============================================================================
TEST_CASE ("melody snap: off only clamps to the melody range")
{
    CHECK_EQ (snapMelodyPitch (61, MelodySnap::Off, cMajor, std::nullopt), 61);
    CHECK_EQ (snapMelodyPitch (20, MelodySnap::Off, cMajor, std::nullopt), kMelodyLowestNote);
    CHECK_EQ (snapMelodyPitch (127, MelodySnap::Off, cMajor, std::nullopt), kMelodyHighestNote);
    CHECK_EQ (std::string (melodySnapName (MelodySnap::Off)), std::string ("Off"));
    CHECK_EQ (std::string (melodySnapName (MelodySnap::Scale)), std::string ("Scale"));
    CHECK_EQ (std::string (melodySnapName (MelodySnap::ChordTones)), std::string ("Chord"));
}

TEST_CASE ("melody snap: scale tones, ties go down, chord tones count as scale")
{
    CHECK_EQ (snapMelodyPitch (60, MelodySnap::Scale, cMajor, std::nullopt), 60);
    CHECK_EQ (snapMelodyPitch (61, MelodySnap::Scale, cMajor, std::nullopt), 60);   // C# -> C (tie: lower)
    CHECK_EQ (snapMelodyPitch (66, MelodySnap::Scale, cMajor, std::nullopt), 65);   // F# -> F
    CHECK_EQ (snapMelodyPitch (70, MelodySnap::Scale, cMajor, std::nullopt), 69);   // Bb -> A

    const std::optional<Chord> e7 = Chord { 4, ChordQuality::Dominant7 };
    CHECK_EQ (snapMelodyPitch (68, MelodySnap::Scale, aMinor, e7), 68);            // G# allowed over E7
    CHECK_EQ (snapMelodyPitch (68, MelodySnap::Scale, aMinor, std::nullopt), 67);  // ... but not without it

    // The range edges: no candidate outside [36, 96]
    CHECK_EQ (snapMelodyPitch (36, MelodySnap::Scale, fSharpMajor, std::nullopt), 37);   // C -> C# (B = 35 is out)
    CHECK_EQ (snapMelodyPitch (96, MelodySnap::Scale, fSharpMajor, std::nullopt), 95);   // C -> B (C# = 97 is out)
}

TEST_CASE ("melody snap: chord tones, scale where no chord sounds")
{
    const std::optional<Chord> c = Chord { 0, ChordQuality::Major };
    CHECK_EQ (snapMelodyPitch (62, MelodySnap::ChordTones, cMajor, c), 60);   // D: C and E equally near -> C
    CHECK_EQ (snapMelodyPitch (63, MelodySnap::ChordTones, cMajor, c), 64);
    CHECK_EQ (snapMelodyPitch (66, MelodySnap::ChordTones, cMajor, c), 67);
    CHECK_EQ (snapMelodyPitch (71, MelodySnap::ChordTones, cMajor, c), 72);
    CHECK_EQ (snapMelodyPitch (61, MelodySnap::ChordTones, cMajor, std::nullopt), 60);   // no chord: scale
    CHECK_EQ (snapMelodyPitch (62, MelodySnap::ChordTones, cMajor, std::nullopt), 62);

    const std::optional<Chord> fmaj7 = Chord { 5, ChordQuality::Major7 };
    for (int n = kMelodyLowestNote; n <= kMelodyHighestNote; ++n)
    {
        const int s = snapMelodyPitch (n, MelodySnap::ChordTones, cMajor, fmaj7);
        CHECK (toneRole (s, cMajor, &*fmaj7) == ToneRole::ChordTone || toneRole (s, cMajor, &*fmaj7) == ToneRole::ChordRoot);
        CHECK (std::abs (s - n) <= 2);   // F A C E: never more than 2 semitones away
    }
}

TEST_CASE ("melody snap: grid")
{
    CHECK (near (snapBeatToGrid (1.37, 0.25, false), 1.25));
    CHECK (near (snapBeatToGrid (1.37, 0.25, true), 1.25));
    CHECK (near (snapBeatToGrid (1.38, 0.25, true), 1.5));
    CHECK (near (snapBeatToGrid (1.4999999, 0.25, false), 1.5));   // within 1e-6 of a grid line counts as on it
    CHECK (near (snapBeatToGrid (0.7, 1.0 / 3.0, false), 2.0 / 3.0));
    CHECK (near (snapBeatToGrid (-0.1, 0.5, false), -0.5));
    CHECK (near (snapBeatToGrid (0.3, 0.0, true), 0.3));            // no grid: unchanged
}

//==============================================================================
TEST_CASE ("melody render: one loop, clipped, parked notes left out, channel 3")
{
    const auto events = renderMelody (mel ({ mn (72, 7.5, 2.0), mn (67, 0.0, 1.0, 0.5f), mn (69, 8.0, 1.0),
                                             mn (71, 9.0, 1.0), mn (64, 0.0, 0.5) }),
                                      8.0, 3);
    REQUIRE (events.size() == 3);

    CHECK_EQ (events[0].note, 64);
    CHECK_EQ (events[1].note, 67);
    CHECK (near (events[1].lengthBeats, 1.0));
    CHECK_EQ (events[1].velocity, 0.5f);
    CHECK_EQ (events[2].note, 72);
    CHECK (near (events[2].startBeat, 7.5));
    CHECK (near (events[2].lengthBeats, 0.5));   // clipped at the loop end

    for (const auto& e : events)
        CHECK_EQ (e.channel, 3);

    CHECK (renderMelody (mel ({ mn (60, 0.0, 1.0) }), 0.0).empty());
    CHECK (renderMelody ({}, 16.0).empty());
    CHECK_EQ (renderMelody (mel ({ mn (60, 0.0, 1.0) }), 4.0, 5)[0].channel, 5);
}

TEST_CASE ("melody render: overlapping notes of one pitch retrigger cleanly")
{
    const auto events = renderMelody (mel ({ mn (60, 0.0, 3.0), mn (60, 2.0, 1.0), mn (64, 0.0, 4.0), mn (62, 1.0, 4.0) }),
                                      4.0, 3);
    REQUIRE (events.size() == 4);
    CHECK_EQ (events[0].note, 60);
    CHECK (near (events[0].lengthBeats, 2.0));   // ends where the next C starts
    CHECK_EQ (events[1].note, 64);
    CHECK (near (events[1].lengthBeats, 4.0));
    CHECK_EQ (events[2].note, 62);
    CHECK (near (events[2].lengthBeats, 3.0));   // clipped at the loop end
    CHECK_EQ (events[3].note, 60);
    CHECK (near (events[3].startBeat, 2.0));
}

TEST_CASE ("melody render: random melodies never overlap one pitch and stay inside the loop")
{
    Lcg rng { 777u };

    for (int round = 0; round < 200; ++round)
    {
        Melody m;
        const int count = rng.range (0, 60);

        for (int i = 0; i < count; ++i)
            m.notes.push_back (mn (rng.range (55, 70), std::round (rng.unit() * 40.0 * 4.0) / 4.0, 0.25 + rng.unit() * 6.0));

        const double loop = (double) rng.range (1, 32);
        const auto events = renderMelody (m, loop, 3);

        for (size_t i = 0; i < events.size(); ++i)
        {
            const auto& e = events[i];
            CHECK (e.startBeat >= 0.0 && e.startBeat < loop);
            CHECK (e.lengthBeats > 0.0 && e.startBeat + e.lengthBeats <= loop + kTol);

            if (i > 0)
                CHECK (events[i - 1].startBeat < e.startBeat
                       || (events[i - 1].startBeat == e.startBeat && events[i - 1].note < e.note));

            for (size_t j = i + 1; j < events.size(); ++j)
                if (events[j].note == e.note)
                    CHECK (events[j].startBeat >= e.startBeat + e.lengthBeats - kTol
                           || e.startBeat >= events[j].startBeat + events[j].lengthBeats - kTol);
        }
    }
}

//==============================================================================
TEST_CASE ("melody transpose: same pitch-class move as the chords, the short way round")
{
    const auto m = mel ({ mn (60, 0.0, 1.0), mn (95, 1.0, 1.0), mn (37, 2.0, 1.0) });

    const auto up2 = transposeMelody (m, 2);
    CHECK_EQ (up2.notes[0].note, 62);
    CHECK_EQ (up2.notes[1].note, 85);   // 97 is out of range: an octave lower
    CHECK_EQ (up2.notes[2].note, 39);

    const auto up7 = transposeMelody (m, 7);   // = 5 semitones down
    CHECK_EQ (up7.notes[0].note, 55);
    CHECK_EQ (up7.notes[1].note, 90);
    CHECK_EQ (up7.notes[2].note, 44);   // 32 is out of range: an octave higher

    CHECK_EQ (transposeMelody (m, -7).notes[0].note, 65);
    CHECK_EQ (transposeMelody (m, 6).notes[0].note, 66);
    CHECK_EQ (transposeMelody (m, -6).notes[0].note, 66);
    CHECK (transposeMelody (m, 12) == sanitiseMelody (m));
    CHECK (transposeMelody (m, 0) == sanitiseMelody (m));
}

TEST_CASE ("melody follows slots: remove, move, duplicate, insert, resize")
{
    using Q = ChordQuality;
    const ProgressionSlot c { { 0, Q::Major }, 4 }, g { { 7, Q::Major }, 4 }, am { { 9, Q::Minor }, 4 }, f { { 5, Q::Major }, 4 };
    Progression before;
    before.slots = { c, g, am, f };

    const auto m = mel ({ mn (72, 0.5, 1.0), mn (74, 4.0, 1.0), mn (71, 6.5, 2.0), mn (69, 12.0, 4.0), mn (76, 17.0, 1.0) });

    // Remove G: its notes go, later notes move 4 beats earlier, the note past the end keeps its distance to the end.
    {
        Progression after;
        after.slots = { c, am, f };
        const auto r = melodyFollowingSlots (m, before, after, { 0, 2, 3 });
        CHECK_EQ (starts (r), (std::vector<double> { 0.5, 8.0, 13.0 }));
    }

    // Move F to the front: its note moves with it.
    {
        Progression after;
        after.slots = { f, c, g, am };
        const auto r = melodyFollowingSlots (m, before, after, { 3, 0, 1, 2 });
        CHECK_EQ (starts (r), (std::vector<double> { 0.0, 4.5, 8.0, 10.5, 17.0 }));
        CHECK_EQ (r.notes[0].note, 69);
        CHECK (near (r.notes[0].lengthBeats, 4.0));
    }

    // Duplicate G: both copies get its notes.
    {
        Progression after;
        after.slots = { c, g, g, am, f };
        const auto r = melodyFollowingSlots (m, before, after, { 0, 1, 1, 2, 3 });
        CHECK_EQ (starts (r), (std::vector<double> { 0.5, 4.0, 6.5, 8.0, 10.5, 16.0, 21.0 }));
    }

    // Insert a new chord at index 1: it starts empty, everything after moves 4 beats later.
    {
        Progression after;
        after.slots = { c, f, g, am, f };
        const auto r = melodyFollowingSlots (m, before, after, { 0, -1, 1, 2, 3 });
        CHECK_EQ (starts (r), (std::vector<double> { 0.5, 8.0, 10.5, 16.0, 21.0 }));
    }

    // Shorten G to 2 beats: the note 2.5 beats into it no longer fits and goes; later chords move 2 beats earlier.
    {
        auto shortG = g;
        shortG.lengthBeats = 2;
        Progression after;
        after.slots = { c, shortG, am, f };
        const auto r = melodyFollowingSlots (m, before, after, { 0, 1, 2, 3 });
        CHECK_EQ (starts (r), (std::vector<double> { 0.5, 4.0, 10.0, 15.0 }));
    }

    // Nothing changed / bad mapping: the melody stays (sanitised).
    CHECK (melodyFollowingSlots (m, before, before, { 0, 1, 2, 3 }) == sanitiseMelody (m));
    CHECK (melodyFollowingSlots (m, before, before, { 0, 1 }) == sanitiseMelody (m));

    // From an empty progression every note is past the end: all keep their distance to the (new) end.
    {
        Progression after;
        after.slots = { c };
        const auto r = melodyFollowingSlots (mel ({ mn (60, 1.0, 1.0) }), {}, after, { -1 });
        CHECK_EQ (starts (r), (std::vector<double> { 5.0 }));
    }
}
