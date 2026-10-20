#include "TestFramework.h"

#include "core/NoteEdit.h"

// Roll note editing (owner: WP3; final-spec 4.5, 9 WP3). WP0 seeded this file with a selection check; WP3 adds the rest.

using namespace cb;

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
