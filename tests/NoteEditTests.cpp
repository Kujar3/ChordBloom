#include "TestFramework.h"

#include "core/NoteEdit.h"
#include "core/Layers.h"

#include <cmath>
#include <cstdint>
#include <limits>

// Roll note editing (owner: WP3; final-spec 4.5, 9 WP3). WP0 seeded this file with a selection check; WP3 adds the rest.

using namespace cb;

namespace
{
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
const Chord G { 7, ChordQuality::Major };
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

std::vector<int> pitches (const Melody& m)
{
    std::vector<int> v;
    for (const auto& n : m.notes)
        v.push_back (n.note);
    return v;
}

std::vector<double> starts (const Melody& m)
{
    std::vector<double> v;
    for (const auto& n : m.notes)
        v.push_back (n.startBeat);
    return v;
}

std::vector<double> lengths (const Melody& m)
{
    std::vector<double> v;
    for (const auto& n : m.notes)
        v.push_back (n.lengthBeats);
    return v;
}

// A 1-note list transposed by scale steps; returns the new pitch.
int scaleStep (int pitch, int steps, const Key& key)
{
    const auto r = transposeNotesInScale (mel ({ mn (pitch, 0.0, 1.0) }), { 0 }, steps, key, NoteLimits { 0, 127, 16 });
    return r.notes.notes.empty() ? -1 : r.notes.notes[0].note;
}

// The selection must be ascending, unique and in range.
bool validResultSelection (const NoteEditResult& r)
{
    for (size_t i = 0; i < r.selection.size(); ++i)
    {
        if (r.selection[i] < 0 || r.selection[i] >= (int) r.notes.notes.size())
            return false;
        if (i > 0 && r.selection[i] <= r.selection[i - 1])
            return false;
    }
    return true;
}
} // namespace

TEST_CASE ("note edit: WP0 validSelection and allNoteIndices")
{
    Melody m;
    m.notes = { MelodyNote { 60, 0.0, 1.0, 0.8f }, MelodyNote { 62, 1.0, 1.0, 0.8f } };
    CHECK_EQ (validSelection (m, { 1, -1, 5, 1, 0 }), (std::vector<int> { 0, 1 }));
    CHECK_EQ (allNoteIndices (m), (std::vector<int> { 0, 1 }));
}

TEST_CASE ("note edit: WP0 a zero-delta move changes nothing (no re-snap of off-snap notes)")
{
    // C# (61) is off the C major scale: a click (delta 0, 0) under Scale snap must not snap it to C or D.
    Melody m;
    m.notes = { MelodyNote { 61, 0.0, 1.0, 0.8f }, MelodyNote { 64, 1.0, 1.0, 0.8f } };
    NoteEditContext ctx;
    ctx.key = Key { 0, ScaleType::Major };
    ctx.snap = MelodySnap::Scale;
    const auto r = moveNotes (m, { 0, 1 }, 0.0, 0, ctx);
    CHECK (r.notes == sanitiseMelody (m, ctx.limits));
    CHECK_EQ (r.selection, (std::vector<int> { 0, 1 }));
}

//==============================================================================
TEST_CASE ("note edit: validSelection and allNoteIndices edge cases")
{
    CHECK (validSelection (Melody {}, { 0, 1, -3 }).empty());
    CHECK (allNoteIndices (Melody {}).empty());
    CHECK_EQ (validSelection (mel ({ mn (60, 0, 1) }), { 0, 0, 0 }), (std::vector<int> { 0 }));
}

TEST_CASE ("note edit: notesInArea edges and reversed ranges")
{
    const auto m = mel ({ mn (60, 0.0, 1.0), mn (62, 1.0, 1.0), mn (64, 2.0, 1.0) });

    CHECK_EQ (notesInArea (m, 0.0, 3.0, 0, 127), (std::vector<int> { 0, 1, 2 }));
    CHECK_EQ (notesInArea (m, 1.0, 2.0, 60, 64), (std::vector<int> { 1 }));   // A ends at 1, C starts at 2: both outside
    CHECK_EQ (notesInArea (m, 0.5, 1.5, 60, 64), (std::vector<int> { 0, 1 }));
    CHECK_EQ (notesInArea (m, 0.5, 1.5, 62, 62), (std::vector<int> { 1 }));
    CHECK_EQ (notesInArea (m, 0.99, 1.01, 60, 64), (std::vector<int> { 0, 1 }));
    CHECK (notesInArea (m, 10.0, 20.0, 0, 127).empty());
    CHECK (notesInArea (m, 3.0, 4.0, 0, 127).empty());

    // pitch bounds are inclusive
    CHECK_EQ (notesInArea (m, 0.0, 3.0, 60, 60), (std::vector<int> { 0 }));
    CHECK_EQ (notesInArea (m, 0.0, 3.0, 64, 70), (std::vector<int> { 2 }));
    CHECK_EQ (notesInArea (m, 0.0, 3.0, 61, 63), (std::vector<int> { 1 }));
    CHECK (notesInArea (m, 0.0, 3.0, 65, 70).empty());

    // reversed ranges (a leftward or downward marquee) behave the same
    CHECK_EQ (notesInArea (m, 2.0, 1.0, 64, 60), (std::vector<int> { 1 }));
    CHECK_EQ (notesInArea (m, 3.0, 0.0, 127, 0), (std::vector<int> { 0, 1, 2 }));
    CHECK_EQ (notesInArea (m, 1.5, 0.5, 62, 62), (std::vector<int> { 1 }));
    CHECK_EQ (notesInArea (m, 0.5, 1.5, 64, 60), (std::vector<int> { 0, 1 }));
    CHECK (notesInArea (Melody {}, 0.0, 10.0, 0, 127).empty());

    // a long note that starts before the area still overlaps it
    const auto longNote = mel ({ mn (60, 0.0, 8.0) });
    CHECK_EQ (notesInArea (longNote, 3.0, 4.0, 60, 60), (std::vector<int> { 0 }));
}

TEST_CASE ("note edit: notesStartingIn and findNotes")
{
    const auto m = mel ({ mn (60, 0.0, 1.0), mn (62, 1.0, 1.0), mn (64, 2.0, 1.0) });

    CHECK_EQ (notesStartingIn (m, 1.0, 2.0), (std::vector<int> { 1 }));       // [from, to)
    CHECK_EQ (notesStartingIn (m, 0.0, 3.0), (std::vector<int> { 0, 1, 2 }));
    CHECK_EQ (notesStartingIn (m, 2.0, 1.0), (std::vector<int> { 1 }));       // reversed
    CHECK (notesStartingIn (m, 0.5, 1.0).empty());
    CHECK (notesStartingIn (m, 3.0, 4.0).empty());
    CHECK_EQ (notesStartingIn (m, 0.0, 0.5), (std::vector<int> { 0 }));

    // matched by (pitch, start); ascending, unique, missing skipped; velocity and length are ignored
    const std::vector<MelodyNote> wanted { mn (64, 2.0, 9.0, 0.1f), mn (99, 0.0, 1.0), mn (62, 1.0000001, 1.0), mn (62, 1.0, 5.0) };
    CHECK_EQ (findNotes (m, wanted), (std::vector<int> { 1, 2 }));
    CHECK (findNotes (m, {}).empty());
    CHECK (findNotes (m, { mn (60, 0.5, 1.0), mn (61, 0.0, 1.0) }).empty());   // right pitch, wrong start; right start, wrong pitch
    CHECK (findNotes (Melody {}, wanted).empty());
    CHECK_EQ (findNotes (m, { mn (64, 2.0, 1.0), mn (60, 0.0, 1.0) }), (std::vector<int> { 0, 2 }));

    // the same start at several pitches
    const auto chord = mel ({ mn (60, 0.0, 1.0), mn (64, 0.0, 1.0), mn (67, 0.0, 1.0) });
    CHECK_EQ (findNotes (chord, { mn (67, 0.0, 1.0), mn (64, 0.0, 1.0) }), (std::vector<int> { 1, 2 }));
}

//==============================================================================
TEST_CASE ("note edit: moveNotes clamps the group at beat 0 without folding single notes")
{
    NoteEditContext ctx;
    const auto m = mel ({ mn (60, 0.5, 1.0), mn (62, 1.5, 1.0), mn (64, 4.0, 1.0) });

    const auto r = moveNotes (m, { 0, 1 }, -5.0, 0, ctx);
    CHECK_EQ (starts (r.notes), (std::vector<double> { 0.0, 1.0, 4.0 }));   // clamped once for the group: -0.5
    CHECK_EQ (pitches (r.notes), (std::vector<int> { 60, 62, 64 }));
    CHECK_EQ (r.selection, (std::vector<int> { 0, 1 }));

    // a plain forward move; the unselected note stays
    const auto f = moveNotes (m, { 0, 1 }, 1.0, 0, ctx);
    CHECK_EQ (starts (f.notes), (std::vector<double> { 1.5, 2.5, 4.0 }));
    CHECK_EQ (f.selection, (std::vector<int> { 0, 1 }));
}

TEST_CASE ("note edit: moveNotes clamps the group at maxBeat")
{
    NoteEditContext ctx;
    ctx.maxBeat = 8.0;
    const auto m = mel ({ mn (60, 5.0, 1.0), mn (62, 6.0, 1.0) });

    const auto r = moveNotes (m, { 0, 1 }, 10.0, 0, ctx);
    // the last start may be maxBeat - kMinMelodyNoteBeats = 7.9375: delta 1.9375
    CHECK_EQ (starts (r.notes), (std::vector<double> { 6.9375, 7.9375 }));
    CHECK_EQ (r.selection, (std::vector<int> { 0, 1 }));

    // notes that already lie beyond the allowed region are not dragged back by the clamp, but can move towards it
    const auto parked = mel ({ mn (60, 20.0, 1.0) });
    const auto stay = moveNotes (parked, { 0 }, 5.0, 0, ctx);
    CHECK_EQ (starts (stay.notes), (std::vector<double> { 20.0 }));
    const auto back = moveNotes (parked, { 0 }, -5.0, 0, ctx);
    CHECK_EQ (starts (back.notes), (std::vector<double> { 15.0 }));
}

TEST_CASE ("note edit: moveNotes clamps the group at both pitch limits (never folding single notes)")
{
    NoteEditContext ctx;
    ctx.limits = layerLimits (Layer::Bass);   // 24..72

    const auto low = mel ({ mn (30, 0.0, 1.0), mn (40, 1.0, 1.0) });
    const auto d = moveNotes (low, { 0, 1 }, 0.0, -12, ctx);
    CHECK_EQ (pitches (d.notes), (std::vector<int> { 24, 34 }));   // -6, not -12 and not folded
    CHECK_EQ (d.selection, (std::vector<int> { 0, 1 }));

    const auto high = mel ({ mn (60, 0.0, 1.0), mn (70, 1.0, 1.0) });
    const auto u = moveNotes (high, { 0, 1 }, 0.0, 10, ctx);
    CHECK_EQ (pitches (u.notes), (std::vector<int> { 62, 72 }));

    // melody limits: a group already at the top cannot go up at all
    NoteEditContext melodyCtx;
    const auto top = mel ({ mn (36, 0.0, 1.0), mn (96, 1.0, 1.0) });
    const auto same = moveNotes (top, { 0, 1 }, 0.0, 7, melodyCtx);
    CHECK (same.notes == top);
    CHECK_EQ (same.selection, (std::vector<int> { 0, 1 }));
    const auto none = moveNotes (top, { 0, 1 }, 0.0, -7, melodyCtx);
    CHECK (none.notes == top);

    // a bass note at 28 moves inside the bass limits only
    const auto b = moveNotes (mel ({ mn (28, 0.0, 1.0) }), { 0 }, 0.0, -3, ctx);
    CHECK_EQ (pitches (b.notes), (std::vector<int> { 25 }));
    const auto b2 = moveNotes (mel ({ mn (28, 0.0, 1.0) }), { 0 }, 0.0, -9, ctx);
    CHECK_EQ (pitches (b2.notes), (std::vector<int> { 24 }));
}

TEST_CASE ("note edit: moveNotes re-snaps moved notes against the chord at the new start")
{
    const auto p = prog ({ { C, 4 }, { G, 4 } });
    NoteEditContext ctx;
    ctx.progression = &p;
    ctx.key = cMajor;
    ctx.snap = MelodySnap::ChordTones;

    // E (a C chord tone) moved from beat 3 to beat 5 (a G chord): the nearest G chord tone is D (62)
    const auto m = mel ({ mn (64, 3.0, 1.0) });
    const auto r = moveNotes (m, { 0 }, 2.0, 0, ctx);
    CHECK_EQ (pitches (r.notes), (std::vector<int> { 62 }));
    CHECK_EQ (starts (r.notes), (std::vector<double> { 5.0 }));
    CHECK_EQ (r.selection, (std::vector<int> { 0 }));

    // snap Off: the pitch is kept
    ctx.snap = MelodySnap::Off;
    CHECK_EQ (pitches (moveNotes (m, { 0 }, 2.0, 0, ctx).notes), (std::vector<int> { 64 }));

    // no chords at all: Scale snap uses the key, ChordTones falls back to scale tones
    NoteEditContext noChords;
    noChords.key = cMajor;
    noChords.snap = MelodySnap::ChordTones;
    CHECK_EQ (pitches (moveNotes (mel ({ mn (61, 0.0, 1.0) }), { 0 }, 1.0, 0, noChords).notes), (std::vector<int> { 60 }));

    // a pitch move re-snaps the moved note to the allowed pitches of the chord there
    NoteEditContext scale;
    scale.progression = &p;
    scale.key = cMajor;
    scale.snap = MelodySnap::Scale;
    // E + 1 = F: in the scale, stays
    CHECK_EQ (pitches (moveNotes (mel ({ mn (64, 0.0, 1.0) }), { 0 }, 0.0, 1, scale).notes), (std::vector<int> { 65 }));
    // C + 1 = C#: off the scale, snaps down (ties go down)
    CHECK_EQ (pitches (moveNotes (mel ({ mn (60, 0.0, 1.0) }), { 0 }, 0.0, 1, scale).notes), (std::vector<int> { 60 }));
}

TEST_CASE ("note edit: a pitch-only move re-snaps only the moved notes")
{
    NoteEditContext ctx;
    ctx.key = cMajor;
    ctx.snap = MelodySnap::Scale;

    // 61 (C#, selected) +2 -> 63 (D#, off scale) snaps to D; 63 (unselected, off scale) stays as it was
    const auto m = mel ({ mn (61, 0.0, 1.0), mn (63, 1.0, 1.0) });
    const auto r = moveNotes (m, { 0 }, 0.0, 2, ctx);
    CHECK_EQ (pitches (r.notes), (std::vector<int> { 62, 63 }));
    CHECK_EQ (r.selection, (std::vector<int> { 0 }));

    // a time-only move of an off-scale note re-snaps it too (it is a moved note)
    const auto t = moveNotes (m, { 0 }, 0.5, 0, ctx);
    CHECK_EQ (pitches (t.notes), (std::vector<int> { 60, 63 }));   // C# at 0.5 snaps down to C; the note at 1.0 is untouched
}

TEST_CASE ("note edit: a zero (or fully clamped) group delta returns the input without any re-snap")
{
    NoteEditContext ctx;
    ctx.key = cMajor;
    ctx.snap = MelodySnap::Scale;

    const auto m = mel ({ mn (36, 0.0, 1.0), mn (61, 0.0, 1.0) });

    // clamped to (0, 0): the first note is at beat 0 and at the lowest pitch
    const auto r = moveNotes (m, { 0, 1 }, -3.0, -5, ctx);
    CHECK (r.notes == m);
    CHECK_EQ (r.selection, (std::vector<int> { 0, 1 }));

    const auto z = moveNotes (m, { 1 }, 0.0, 0, ctx);
    CHECK (z.notes == m);
    CHECK_EQ (z.selection, (std::vector<int> { 1 }));

    // NaN deltas count as zero; an empty or invalid selection changes nothing
    const double nan = std::numeric_limits<double>::quiet_NaN();
    CHECK (moveNotes (m, { 0, 1 }, nan, 0, ctx).notes == m);
    CHECK (moveNotes (m, {}, 3.0, 3, ctx).notes == m);
    CHECK (moveNotes (m, { 9, -1 }, 3.0, 3, ctx).notes == m);
    CHECK (moveNotes (m, { 9, -1 }, 3.0, 3, ctx).selection.empty());
}

TEST_CASE ("note edit: two notes moved onto the same pitch and start keep the longer and select it")
{
    NoteEditContext ctx;

    // the unselected note is the longer one: it survives and becomes the selection
    auto r = moveNotes (mel ({ mn (60, 0.0, 2.0), mn (62, 1.0, 1.0) }), { 1 }, -1.0, -2, ctx);
    REQUIRE (r.notes.notes.size() == 1);
    CHECK_EQ (r.notes.notes[0].lengthBeats, 2.0);
    CHECK_EQ (r.selection, (std::vector<int> { 0 }));

    // the moved note is the longer one
    r = moveNotes (mel ({ mn (60, 0.0, 1.0), mn (62, 1.0, 2.0) }), { 1 }, -1.0, -2, ctx);
    REQUIRE (r.notes.notes.size() == 1);
    CHECK_EQ (r.notes.notes[0].lengthBeats, 2.0);
    CHECK_EQ (r.selection, (std::vector<int> { 0 }));

    // two selected notes collapsing onto each other through the snap (both C# and D snap to the same pitch)
    NoteEditContext snap;
    snap.key = cMajor;
    snap.snap = MelodySnap::Scale;
    r = moveNotes (mel ({ mn (62, 0.0, 1.0), mn (62, 0.0, 1.0) }), { 0, 1 }, 0.0, 1, snap);   // duplicates are already collapsed
    CHECK_EQ (r.notes.notes.size(), (size_t) 1);

    // moved notes re-sorted: the selection follows them by (pitch, start)
    r = moveNotes (mel ({ mn (60, 0.0, 1.0), mn (62, 1.0, 1.0), mn (64, 2.0, 1.0) }), { 2 }, -3.0, 0, ctx);
    CHECK_EQ (starts (r.notes), (std::vector<double> { 0.0, 0.0, 1.0 }));
    CHECK_EQ (pitches (r.notes), (std::vector<int> { 60, 64, 62 }));
    CHECK_EQ (r.selection, (std::vector<int> { 1 }));
}

//==============================================================================
TEST_CASE ("note edit: resizeNotes clamps each note's length")
{
    const auto m = mel ({ mn (60, 0.0, 1.0), mn (62, 1.0, 1.0), mn (64, 255.0, 0.5) });
    const NoteLimits limits = kMelodyLimits;

    auto r = resizeNotes (m, { 0, 2 }, 1.0, 0.25, limits);
    CHECK_EQ (lengths (r.notes), (std::vector<double> { 2.0, 1.0, 1.0 }));   // the last note ends at 256 at the latest
    CHECK_EQ (r.selection, (std::vector<int> { 0, 2 }));

    r = resizeNotes (m, { 0, 2 }, -5.0, 0.25, limits);
    CHECK_EQ (lengths (r.notes), (std::vector<double> { 0.25, 1.0, 0.25 }));

    r = resizeNotes (m, { 0 }, 100.0, 0.25, limits);
    CHECK_EQ (lengths (r.notes), (std::vector<double> { 64.0, 1.0, 0.5 }));

    r = resizeNotes (m, { 1 }, -0.5, 0.0, limits);   // minLength below the hard minimum
    CHECK_EQ (lengths (r.notes), (std::vector<double> { 1.0, 0.5, 0.5 }));
    r = resizeNotes (m, { 1 }, -5.0, 0.0, limits);
    CHECK_EQ (r.notes.notes[1].lengthBeats, kMinMelodyNoteBeats);

    // a minimum above the current length lengthens
    r = resizeNotes (m, { 0 }, 0.0, 2.0, limits);
    CHECK_EQ (r.notes.notes[0].lengthBeats, 2.0);

    const double nan = std::numeric_limits<double>::quiet_NaN();
    CHECK (resizeNotes (m, { 0 }, nan, 0.25, limits).notes == m);
    CHECK (resizeNotes (m, {}, 1.0, 0.25, limits).notes == m);
    CHECK (resizeNotes (m, {}, 1.0, 0.25, limits).selection.empty());
}

TEST_CASE ("note edit: defaultDuplicateOffset examples and grid <= 0")
{
    auto spanOf = [] (double span, double grid)
    {
        return defaultDuplicateOffset (mel ({ mn (60, 0.0, span) }), { 0 }, grid);
    };

    // grid 1/4: 0.5 -> 0.5, 1.5 -> 2, 3.5 -> 4, 5 -> 8
    CHECK_EQ (spanOf (0.5, 0.25), 0.5);
    CHECK_EQ (spanOf (1.5, 0.25), 2.0);
    CHECK_EQ (spanOf (3.5, 0.25), 4.0);
    CHECK_EQ (spanOf (5.0, 0.25), 8.0);
    CHECK_EQ (spanOf (1.0, 0.25), 1.0);
    CHECK_EQ (spanOf (2.0, 0.25), 2.0);
    CHECK_EQ (spanOf (2.25, 0.25), 4.0);
    CHECK_EQ (spanOf (4.0, 0.25), 4.0);
    CHECK_EQ (spanOf (8.0, 0.25), 8.0);
    CHECK_EQ (spanOf (0.3, 0.25), 0.5);
    CHECK_EQ (spanOf (0.01, 0.25), 0.25);   // at least one grid step
    CHECK_EQ (spanOf (0.9, 0.25), 1.0);

    // another grid
    CHECK_EQ (spanOf (0.3, 1.0 / 3.0), 1.0 / 3.0);

    // the span runs from the earliest start to the latest end
    CHECK_EQ (defaultDuplicateOffset (mel ({ mn (60, 1.0, 1.0), mn (62, 2.25, 1.0) }), { 0, 1 }, 0.25), 4.0);
    CHECK_EQ (defaultDuplicateOffset (mel ({ mn (60, 1.0, 1.0), mn (62, 2.25, 1.0) }), { 0 }, 0.25), 1.0);
    CHECK_EQ (defaultDuplicateOffset (mel ({ mn (60, 0.0, 4.0), mn (62, 1.0, 0.5) }), { 0, 1 }, 0.25), 4.0);

    // empty / invalid selection
    CHECK_EQ (defaultDuplicateOffset (mel ({ mn (60, 0.0, 1.0) }), {}, 0.25), 4.0);
    CHECK_EQ (defaultDuplicateOffset (mel ({ mn (60, 0.0, 1.0) }), { 5 }, 0.0), 4.0);
    CHECK_EQ (defaultDuplicateOffset (Melody {}, {}, 0.25), 4.0);

    // grid <= 0: no grid rounding (the beat / bar rounding still applies), at least one tick
    CHECK_EQ (spanOf (0.5, 0.0), 0.5);
    CHECK_EQ (spanOf (0.3, -1.0), 0.3);
    CHECK_EQ (spanOf (1.5, 0.0), 2.0);
    CHECK_EQ (spanOf (3.5, 0.0), 4.0);
    CHECK_EQ (spanOf (5.0, 0.0), 8.0);
    CHECK_EQ (spanOf (0.001, 0.0), kMelodyTick);
}

TEST_CASE ("note edit: duplicateNotes copies, selects the copies and drops copies past the end")
{
    const auto m = mel ({ mn (60, 0.0, 1.0, 0.5f), mn (62, 1.0, 1.0) });

    auto r = duplicateNotes (m, { 0 }, 4.0, kMelodyLimits);
    REQUIRE (r.notes.notes.size() == 3);
    CHECK_EQ (pitches (r.notes), (std::vector<int> { 60, 62, 60 }));
    CHECK_EQ (starts (r.notes), (std::vector<double> { 0.0, 1.0, 4.0 }));
    CHECK_EQ (r.notes.notes[2].velocity, 0.5f);
    CHECK_EQ (r.selection, (std::vector<int> { 2 }));

    // the whole selection, relative positions kept
    r = duplicateNotes (m, { 0, 1 }, 2.0, kMelodyLimits);
    CHECK_EQ (starts (r.notes), (std::vector<double> { 0.0, 1.0, 2.0, 3.0 }));
    CHECK_EQ (pitches (r.notes), (std::vector<int> { 60, 62, 60, 62 }));
    CHECK_EQ (r.selection, (std::vector<int> { 2, 3 }));

    // copies that start at or after kMaxMelodyBeat are dropped
    r = duplicateNotes (mel ({ mn (60, 254.0, 1.0) }), { 0 }, 4.0, kMelodyLimits);
    CHECK_EQ (r.notes.notes.size(), (size_t) 1);
    CHECK (r.selection.empty());
    r = duplicateNotes (mel ({ mn (60, 252.0, 1.0) }), { 0 }, 4.0, kMelodyLimits);   // lands exactly at 256
    CHECK_EQ (r.notes.notes.size(), (size_t) 1);
    CHECK (r.selection.empty());
    r = duplicateNotes (mel ({ mn (60, 250.0, 1.0), mn (62, 253.0, 1.0) }), { 0, 1 }, 4.0, kMelodyLimits);
    CHECK_EQ (r.notes.notes.size(), (size_t) 3);   // only the first copy fits (254); 257 is dropped
    CHECK_EQ (r.selection, (std::vector<int> { 2 }));

    // a copy landing on an existing note: the longer survives and is selected
    r = duplicateNotes (mel ({ mn (60, 0.0, 1.0), mn (60, 1.0, 2.0) }), { 0 }, 1.0, kMelodyLimits);
    REQUIRE (r.notes.notes.size() == 2);
    CHECK_EQ (r.notes.notes[1].lengthBeats, 2.0);
    CHECK_EQ (r.selection, (std::vector<int> { 1 }));

    // offset 0: the copies collapse onto the originals
    r = duplicateNotes (m, { 0 }, 0.0, kMelodyLimits);
    CHECK (r.notes == m);
    CHECK_EQ (r.selection, (std::vector<int> { 0 }));

    // empty selection and invalid indices
    r = duplicateNotes (m, {}, 4.0, kMelodyLimits);
    CHECK (r.notes == m);
    CHECK (r.selection.empty());
    CHECK (duplicateNotes (m, { 7 }, 4.0, kMelodyLimits).notes == m);

    // the cap of the limits applies
    Melody full;
    for (int i = 0; i < kMaxMelodyNotes; ++i)
        full.notes.push_back (mn (40 + i % 12, i * 0.25, 0.25));
    r = duplicateNotes (full, { 0 }, 1000.0, kMelodyLimits);   // would start past the end: dropped
    CHECK_EQ (r.notes.notes.size(), (size_t) kMaxMelodyNotes);
}

TEST_CASE ("note edit: deleteNotes removes the selection and clears it")
{
    const auto m = mel ({ mn (60, 0.0, 1.0), mn (62, 1.0, 1.0), mn (64, 2.0, 1.0) });

    auto r = deleteNotes (m, { 0, 2, 7, -1 }, kMelodyLimits);
    CHECK_EQ (pitches (r.notes), (std::vector<int> { 62 }));
    CHECK (r.selection.empty());

    r = deleteNotes (m, {}, kMelodyLimits);
    CHECK (r.notes == m);
    CHECK (r.selection.empty());

    r = deleteNotes (m, { 0, 1, 2 }, kMelodyLimits);
    CHECK (r.notes.empty());
    CHECK (r.selection.empty());

    r = deleteNotes (mel ({ mn (28, 0.0, 1.0), mn (30, 1.0, 1.0) }), { 1 }, layerLimits (Layer::Bass));
    CHECK_EQ (pitches (r.notes), (std::vector<int> { 28 }));
}

TEST_CASE ("note edit: velocity set and offset clamp to 0.05..1")
{
    const auto m = mel ({ mn (60, 0.0, 1.0, 0.5f), mn (62, 1.0, 1.0, 0.5f) });

    auto r = setNoteVelocity (m, { 1 }, 2.0f, kMelodyLimits);
    CHECK_EQ (r.notes.notes[0].velocity, 0.5f);
    CHECK_EQ (r.notes.notes[1].velocity, 1.0f);
    CHECK_EQ (r.selection, (std::vector<int> { 1 }));

    r = setNoteVelocity (m, { 0, 1 }, 0.0f, kMelodyLimits);
    CHECK_EQ (r.notes.notes[0].velocity, 0.05f);
    CHECK_EQ (r.notes.notes[1].velocity, 0.05f);
    CHECK_EQ (r.selection, (std::vector<int> { 0, 1 }));

    r = setNoteVelocity (m, { 0 }, 0.6f, kMelodyLimits);
    CHECK_EQ (r.notes.notes[0].velocity, 0.6f);

    r = offsetNoteVelocity (m, { 1 }, 0.7f, kMelodyLimits);
    CHECK_EQ (r.notes.notes[1].velocity, 1.0f);
    r = offsetNoteVelocity (m, { 0, 1 }, -1.0f, kMelodyLimits);
    CHECK_EQ (r.notes.notes[0].velocity, 0.05f);
    CHECK_EQ (r.notes.notes[1].velocity, 0.05f);
    r = offsetNoteVelocity (m, { 0 }, 0.25f, kMelodyLimits);
    CHECK_EQ (r.notes.notes[0].velocity, 0.75f);
    CHECK_EQ (r.notes.notes[1].velocity, 0.5f);

    // non-finite input changes nothing
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK (setNoteVelocity (m, { 0 }, nan, kMelodyLimits).notes == m);
    CHECK (offsetNoteVelocity (m, { 0 }, nan, kMelodyLimits).notes == m);

    // empty selection
    r = setNoteVelocity (m, {}, 1.0f, kMelodyLimits);
    CHECK (r.notes == m);
    CHECK (r.selection.empty());
}

TEST_CASE ("note edit: transposeNotes moves by semitones and is refused at the limits")
{
    auto r = transposeNotes (mel ({ mn (60, 0.0, 1.0), mn (62, 1.0, 1.0) }), { 0, 1 }, 5, kMelodyLimits);
    CHECK_EQ (pitches (r.notes), (std::vector<int> { 65, 67 }));
    CHECK_EQ (r.selection, (std::vector<int> { 0, 1 }));

    // the selection follows the notes when they re-sort
    r = transposeNotes (mel ({ mn (60, 0.0, 1.0), mn (62, 0.0, 1.0) }), { 0 }, 5, kMelodyLimits);
    CHECK_EQ (pitches (r.notes), (std::vector<int> { 62, 65 }));
    CHECK_EQ (r.selection, (std::vector<int> { 1 }));

    // refused when any selected note would leave the limits: the input comes back sanitised, the selection kept
    const auto edge = mel ({ mn (60, 0.0, 1.0), mn (95, 1.0, 1.0) });
    r = transposeNotes (edge, { 0, 1 }, 2, kMelodyLimits);
    CHECK (r.notes == edge);
    CHECK_EQ (r.selection, (std::vector<int> { 0, 1 }));
    r = transposeNotes (edge, { 0 }, 2, kMelodyLimits);   // the note at 95 is not selected
    CHECK_EQ (pitches (r.notes), (std::vector<int> { 62, 95 }));
    r = transposeNotes (edge, { 0, 1 }, 1, kMelodyLimits);
    CHECK_EQ (pitches (r.notes), (std::vector<int> { 61, 96 }));
    r = transposeNotes (edge, { 0, 1 }, -24, kMelodyLimits);   // 36 and 71: fine
    CHECK_EQ (pitches (r.notes), (std::vector<int> { 36, 71 }));
    r = transposeNotes (edge, { 0, 1 }, -25, kMelodyLimits);
    CHECK (r.notes == edge);

    // bass limits: 28 - 4 = 24 fits, 28 - 5 does not
    const auto bass = layerLimits (Layer::Bass);
    const auto low = mel ({ mn (28, 0.0, 1.0) });
    CHECK_EQ (pitches (transposeNotes (low, { 0 }, -4, bass).notes), (std::vector<int> { 24 }));
    CHECK (transposeNotes (low, { 0 }, -5, bass).notes == low);
    CHECK_EQ (pitches (transposeNotes (low, { 0 }, 44, bass).notes), (std::vector<int> { 72 }));
    CHECK (transposeNotes (low, { 0 }, 45, bass).notes == low);

    // zero and empty
    CHECK (transposeNotes (low, { 0 }, 0, bass).notes == low);
    CHECK (transposeNotes (low, {}, 5, bass).notes == low);
    CHECK (transposeNotes (low, {}, 5, bass).selection.empty());
}

TEST_CASE ("note edit: transposeNotesInScale in A minor and C major, including non-scale notes")
{
    // A natural minor: A B C D E F G
    CHECK_EQ (scaleStep (57, 1, aMinor), 59);
    CHECK_EQ (scaleStep (57, 2, aMinor), 60);
    CHECK_EQ (scaleStep (57, 7, aMinor), 69);
    CHECK_EQ (scaleStep (57, -1, aMinor), 55);
    CHECK_EQ (scaleStep (57, -7, aMinor), 45);
    CHECK_EQ (scaleStep (60, 3, aMinor), 65);     // C -> D E F
    CHECK_EQ (scaleStep (64, 2, aMinor), 67);     // E -> F G
    CHECK_EQ (scaleStep (59, 1, aMinor), 60);     // B -> C across the octave boundary of the key
    CHECK_EQ (scaleStep (60, -1, aMinor), 59);

    // a non-scale note first goes to the nearest scale tone in the direction of travel (that is the first step)
    CHECK_EQ (scaleStep (61, 1, aMinor), 62);     // C# +1 -> D
    CHECK_EQ (scaleStep (61, 2, aMinor), 64);     // C# +2 -> E
    CHECK_EQ (scaleStep (61, -1, aMinor), 60);    // C# -1 -> C
    CHECK_EQ (scaleStep (61, -2, aMinor), 59);    // C# -2 -> B

    // C major
    CHECK_EQ (scaleStep (60, 1, cMajor), 62);
    CHECK_EQ (scaleStep (64, 3, cMajor), 69);
    CHECK_EQ (scaleStep (71, 1, cMajor), 72);
    CHECK_EQ (scaleStep (67, -3, cMajor), 62);
    CHECK_EQ (scaleStep (60, -1, cMajor), 59);
    CHECK_EQ (scaleStep (60, 7, cMajor), 72);
    CHECK_EQ (scaleStep (60, 14, cMajor), 84);
    CHECK_EQ (scaleStep (61, 1, cMajor), 62);     // C# +1 -> D
    CHECK_EQ (scaleStep (61, 2, cMajor), 64);     // C# +2 -> E
    CHECK_EQ (scaleStep (61, -1, cMajor), 60);
    CHECK_EQ (scaleStep (61, -2, cMajor), 59);
    CHECK_EQ (scaleStep (66, 1, cMajor), 67);     // F# +1 -> G
    CHECK_EQ (scaleStep (70, -1, cMajor), 69);    // Bb -1 -> A
    CHECK_EQ (scaleStep (60, 0, cMajor), 60);

    // another mode: D Dorian (D E F G A B C)
    CHECK_EQ (scaleStep (62, 2, Key { 2, ScaleType::Dorian }), 65);
    CHECK_EQ (scaleStep (72, -1, Key { 2, ScaleType::Dorian }), 71);   // C -> B

    // refused at the limits: the whole group stays
    const auto top = mel ({ mn (60, 0.0, 1.0), mn (96, 1.0, 1.0) });
    auto r = transposeNotesInScale (top, { 0, 1 }, 1, cMajor, kMelodyLimits);
    CHECK (r.notes == top);
    CHECK_EQ (r.selection, (std::vector<int> { 0, 1 }));
    r = transposeNotesInScale (top, { 0 }, 1, cMajor, kMelodyLimits);
    CHECK_EQ (pitches (r.notes), (std::vector<int> { 62, 96 }));
    r = transposeNotesInScale (mel ({ mn (36, 0.0, 1.0) }), { 0 }, -1, cMajor, kMelodyLimits);
    CHECK_EQ (pitches (r.notes), (std::vector<int> { 36 }));
    r = transposeNotesInScale (mel ({ mn (36, 0.0, 1.0) }), { 0 }, 0, cMajor, kMelodyLimits);
    CHECK_EQ (pitches (r.notes), (std::vector<int> { 36 }));

    // selection follows re-sorted notes
    r = transposeNotesInScale (mel ({ mn (60, 0.0, 1.0), mn (62, 0.0, 1.0) }), { 0 }, 1, cMajor, kMelodyLimits);   // C -> D collides
    CHECK_EQ (r.notes.notes.size(), (size_t) 1);
    CHECK_EQ (r.selection, (std::vector<int> { 0 }));
    r = transposeNotesInScale (mel ({ mn (60, 0.0, 1.0), mn (62, 0.0, 1.0) }), { 0 }, 2, cMajor, kMelodyLimits);   // C -> E
    CHECK_EQ (pitches (r.notes), (std::vector<int> { 62, 64 }));
    CHECK_EQ (r.selection, (std::vector<int> { 1 }));
}

TEST_CASE ("note edit: quantizeNotes rounds starts to the nearest grid line and keeps lengths")
{
    const auto m = mel ({ mn (60, 5.0 / 48.0, 1.0), mn (62, 7.0 / 48.0, 0.5), mn (64, 0.5, 0.25), mn (65, 0.625, 2.0) });

    auto r = quantizeNotes (m, { 0, 1, 2, 3 }, 0.25, kMelodyLimits);
    CHECK_EQ (starts (r.notes), (std::vector<double> { 0.0, 0.25, 0.5, 0.75 }));   // 0.625 is a tie: rounds up
    CHECK_EQ (lengths (r.notes), (std::vector<double> { 1.0, 0.5, 0.25, 2.0 }));
    CHECK_EQ (r.selection, (std::vector<int> { 0, 1, 2, 3 }));

    // only the selected notes
    r = quantizeNotes (m, { 1 }, 0.25, kMelodyLimits);
    CHECK_EQ (starts (r.notes), (std::vector<double> { 5.0 / 48.0, 0.25, 0.5, 0.625 }));

    // the selection follows notes that re-sort
    r = quantizeNotes (mel ({ mn (60, 0.1875, 1.0), mn (62, 0.2, 1.0) }), { 1 }, 0.25, kMelodyLimits);
    CHECK_EQ (r.selection.size(), (size_t) 1);

    // grid <= 0: unchanged
    CHECK (quantizeNotes (m, { 0, 1, 2, 3 }, 0.0, kMelodyLimits).notes == m);
    CHECK (quantizeNotes (m, { 0, 1, 2, 3 }, -0.25, kMelodyLimits).notes == m);
    CHECK_EQ (quantizeNotes (m, { 0, 1 }, 0.0, kMelodyLimits).selection, (std::vector<int> { 0, 1 }));

    // a quantised note landing on an existing one of the same pitch: the longer survives and is selected
    r = quantizeNotes (mel ({ mn (60, 11.0 / 48.0, 1.0), mn (60, 0.25, 0.5) }), { 0 }, 0.25, kMelodyLimits);
    REQUIRE (r.notes.notes.size() == 1);
    CHECK_EQ (r.notes.notes[0].lengthBeats, 1.0);
    CHECK_EQ (r.selection, (std::vector<int> { 0 }));

    // a triplet grid
    r = quantizeNotes (mel ({ mn (60, 0.3, 1.0) }), { 0 }, 1.0 / 3.0, kMelodyLimits);
    CHECK_EQ (starts (r.notes), (std::vector<double> { 16.0 / 48.0 }));
}

TEST_CASE ("note edit: legatoNotes ends each note where the next later onset begins")
{
    // A (selected) is bounded by B (not selected), E starts with A and is ignored (not strictly later)
    const auto m = mel ({ mn (60, 0.0, 0.5), mn (67, 0.0, 1.0), mn (62, 1.0, 0.5), mn (64, 1.5, 0.25), mn (65, 3.0, 1.0) });

    auto r = legatoNotes (m, { 0, 3, 4 }, kMelodyLimits);
    CHECK_EQ (lengths (r.notes), (std::vector<double> { 1.0, 1.0, 0.5, 1.5, 1.0 }));
    CHECK_EQ (r.selection, (std::vector<int> { 0, 3, 4 }));

    // everything selected: each ends at the next onset; the last keeps its length
    r = legatoNotes (m, allNoteIndices (m), kMelodyLimits);
    CHECK_EQ (lengths (r.notes), (std::vector<double> { 1.0, 1.0, 0.5, 1.5, 1.0 }));

    // the cap at kMaxMelodyNoteBeats
    r = legatoNotes (mel ({ mn (60, 0.0, 1.0), mn (62, 100.0, 1.0) }), { 0 }, kMelodyLimits);
    CHECK_EQ (r.notes.notes[0].lengthBeats, kMaxMelodyNoteBeats);
    CHECK_EQ (r.notes.notes[1].lengthBeats, 1.0);

    // a note with no later onset keeps its length, a later onset of another pitch counts
    r = legatoNotes (mel ({ mn (60, 0.0, 0.5), mn (72, 2.0, 0.5) }), { 0, 1 }, kMelodyLimits);
    CHECK_EQ (lengths (r.notes), (std::vector<double> { 2.0, 0.5 }));

    // a very short gap is raised to the minimum note length by the sanitising
    r = legatoNotes (mel ({ mn (60, 0.0, 2.0), mn (62, 1.0 / 48.0, 1.0) }), { 0 }, kMelodyLimits);
    CHECK_EQ (r.notes.notes[0].lengthBeats, kMinMelodyNoteBeats);

    // empty selection
    CHECK (legatoNotes (m, {}, kMelodyLimits).notes == m);
    CHECK (legatoNotes (m, {}, kMelodyLimits).selection.empty());
}

TEST_CASE ("note edit: copy and paste origin, folding into the target limits and the end of the song")
{
    const auto m = mel ({ mn (60, 0.5, 1.0), mn (64, 1.3125, 2.0, 0.6f), mn (67, 5.0, 1.0) });

    // the origin is the first selected start floored to the grid
    auto clip = copyNotes (m, { 1, 2 }, 0.5);
    CHECK (! clip.empty());
    CHECK_EQ (clip.sourceBeat, 1.0);
    CHECK_EQ (starts (clip.notes), (std::vector<double> { 0.3125, 4.0 }));
    CHECK_EQ (pitches (clip.notes), (std::vector<int> { 64, 67 }));
    CHECK_EQ (clip.notes.notes[0].velocity, 0.6f);
    CHECK_EQ (clip.notes.notes[0].lengthBeats, 2.0);

    // grid <= 0: not floored
    clip = copyNotes (m, { 1 }, 0.0);
    CHECK_EQ (clip.sourceBeat, 1.3125);
    CHECK_EQ (starts (clip.notes), (std::vector<double> { 0.0 }));
    clip = copyNotes (m, { 1 }, -1.0);
    CHECK_EQ (clip.sourceBeat, 1.3125);

    // a start exactly on a grid line stays there
    clip = copyNotes (m, { 2 }, 1.0);
    CHECK_EQ (clip.sourceBeat, 5.0);

    // empty / invalid selection
    clip = copyNotes (m, {}, 0.25);
    CHECK (clip.empty());
    CHECK_EQ (clip.sourceBeat, 0.0);
    CHECK (copyNotes (m, { 8, -1 }, 0.25).empty());

    // paste at a beat: starts are relative to the origin
    const auto target = mel ({ mn (50, 0.0, 1.0) });
    clip = copyNotes (m, { 1, 2 }, 0.5);
    auto r = pasteNotes (target, clip, 8.0, kMelodyLimits);
    CHECK_EQ (pitches (r.notes), (std::vector<int> { 50, 64, 67 }));
    CHECK_EQ (starts (r.notes), (std::vector<double> { 0.0, 8.3125, 12.0 }));
    CHECK_EQ (r.selection, (std::vector<int> { 1, 2 }));

    // pitches fold by octaves into the target limits (bass 24..72)
    NoteClip high;
    high.notes = mel ({ mn (90, 0.0, 1.0), mn (20, 1.0, 1.0), mn (50, 2.0, 1.0) });
    r = pasteNotes (Melody {}, high, 0.0, layerLimits (Layer::Bass));
    CHECK_EQ (pitches (r.notes), (std::vector<int> { 66, 32, 50 }));
    CHECK_EQ (r.selection, (std::vector<int> { 0, 1, 2 }));

    // a bass note below the melody range survives a paste into the bass limits and is folded for the melody
    NoteClip low;
    low.notes = mel ({ mn (28, 0.0, 1.0) });
    CHECK_EQ (pitches (pasteNotes (Melody {}, low, 0.0, layerLimits (Layer::Bass)).notes), (std::vector<int> { 28 }));
    CHECK_EQ (pitches (pasteNotes (Melody {}, low, 0.0, kMelodyLimits).notes), (std::vector<int> { 40 }));

    // notes at or after kMaxMelodyBeat are dropped
    NoteClip pair;
    pair.notes = mel ({ mn (60, 0.0, 1.0), mn (62, 1.0, 1.0) });
    r = pasteNotes (Melody {}, pair, 255.5, kMelodyLimits);
    CHECK_EQ (pitches (r.notes), (std::vector<int> { 60 }));
    CHECK_EQ (r.selection, (std::vector<int> { 0 }));
    CHECK (pasteNotes (Melody {}, pair, 300.0, kMelodyLimits).notes.empty());
    CHECK (pasteNotes (Melody {}, pair, 300.0, kMelodyLimits).selection.empty());

    // an empty clip changes nothing
    r = pasteNotes (target, NoteClip {}, 4.0, kMelodyLimits);
    CHECK (r.notes == target);
    CHECK (r.selection.empty());

    // pasting onto an existing note: the longer survives and is selected
    r = pasteNotes (mel ({ mn (60, 8.0, 2.0) }), NoteClip { mel ({ mn (60, 0.0, 1.0) }), 0.0 }, 8.0, kMelodyLimits);
    REQUIRE (r.notes.notes.size() == 1);
    CHECK_EQ (r.notes.notes[0].lengthBeats, 2.0);
    CHECK_EQ (r.selection, (std::vector<int> { 0 }));

    // copy then paste a note at the same place: nothing changes
    clip = copyNotes (m, { 0 }, 0.0);
    r = pasteNotes (m, clip, 0.5, kMelodyLimits);
    CHECK (r.notes == m);
    CHECK_EQ (r.selection, (std::vector<int> { 0 }));
}

//==============================================================================
TEST_CASE ("note edit: 10k-step random fuzz keeps lists canonical with valid selections")
{
    Rng rng (0xc0ffee11u);

    const NoteLimits limitSets[3] = { kMelodyLimits, layerLimits (Layer::Arp), layerLimits (Layer::Bass) };
    const Progression progs[2] = { prog ({ { C, 4 }, { G, 4 }, { C, 2 }, { G, 6 } }), prog ({ { G, 3 }, { C, 1 } }) };
    const Key keys[2] = { cMajor, aMinor };

    Melody current;
    NoteClip clip;
    std::vector<int> selection;

    for (int step = 0; step < 10000; ++step)
    {
        const NoteLimits limits = limitSets[rng.range (0, 2)];

        NoteEditContext ctx;
        ctx.limits = limits;
        ctx.key = keys[rng.range (0, 1)];
        ctx.snap = (MelodySnap) rng.range (0, numMelodySnapModes - 1);
        ctx.progression = rng.chance (70) ? &progs[rng.range (0, 1)] : nullptr;
        ctx.maxBeat = rng.chance (50) ? kMaxMelodyBeat : (double) rng.range (4, 40);

        // a random selection: sometimes the previous result, sometimes arbitrary (invalid indices included)
        if (rng.chance (35) || selection.empty())
        {
            selection.clear();
            const int count = rng.range (0, 6);
            for (int i = 0; i < count; ++i)
                selection.push_back (rng.range (-2, (int) current.notes.size() + 2));
        }

        const double delta = rng.chance (4) ? std::numeric_limits<double>::quiet_NaN() : rng.range (-40, 40) * 0.25;
        NoteEditResult r;

        switch (rng.range (0, 15))
        {
            case 0:
            case 1:   r = moveNotes (current, selection, delta, rng.range (-14, 14), ctx); break;
            case 2:   r = resizeNotes (current, selection, delta, rng.chance (50) ? 0.25 : 1.0 / 16.0, limits); break;
            case 3:   r = duplicateNotes (current, selection, rng.chance (50) ? defaultDuplicateOffset (current, selection, 0.25) : delta, limits); break;
            case 4:   r = deleteNotes (current, selection, limits); break;
            case 5:   r = setNoteVelocity (current, selection, (float) (rng.unit() * 1.4 - 0.2), limits); break;
            case 6:   r = offsetNoteVelocity (current, selection, (float) (rng.unit() - 0.5), limits); break;
            case 7:   r = transposeNotes (current, selection, rng.range (-30, 30), limits); break;
            case 8:   r = transposeNotesInScale (current, selection, rng.range (-9, 9), ctx.key, limits); break;
            case 9:   r = quantizeNotes (current, selection, rng.chance (80) ? 0.25 : (rng.chance (50) ? 1.0 / 3.0 : 0.0), limits); break;
            case 10:  r = legatoNotes (current, selection, limits); break;
            case 11:  clip = copyNotes (current, selection, rng.chance (50) ? 0.25 : 0.0); r = deleteNotes (current, {}, limits); break;
            default:  // 12..15: paste or draw (a one-note clip)
            {
                if (rng.chance (60) || clip.empty())
                {
                    NoteClip one;
                    one.notes = mel ({ mn (rng.range (10, 120), rng.range (0, 8) * 0.25, 0.25 * rng.range (1, 8)) });
                    r = pasteNotes (current, one, rng.range (0, 4 * 80) * 0.25, limits);
                }
                else
                    r = pasteNotes (current, clip, rng.range (0, 4 * 80) * 0.25, limits);
                break;
            }
        }

        // canonical for the limits, in range, capped, with a valid selection
        if (! (r.notes == sanitiseMelody (r.notes, limits)))
        {
            cbtest::reportFailure (__FILE__, __LINE__, "result is not canonical at step " + std::to_string (step));
            return;
        }

        if (! validResultSelection (r))
        {
            cbtest::reportFailure (__FILE__, __LINE__, "invalid selection at step " + std::to_string (step));
            return;
        }

        for (const auto& n : r.notes.notes)
            if (n.note < limits.lowest || n.note > limits.highest)
            {
                cbtest::reportFailure (__FILE__, __LINE__, "pitch out of limits at step " + std::to_string (step));
                return;
            }

        current = r.notes;
        selection = r.selection;

        // keep the working list small enough to stay interesting
        if ((int) current.notes.size() > 60)
        {
            std::vector<int> half;
            for (int i = 0; i < (int) current.notes.size(); i += 2)
                half.push_back (i);
            current = deleteNotes (current, half, limits).notes;
            selection.clear();
        }
    }

    CHECK (true);
}
