#include "core/NoteEdit.h"

#include <algorithm>

// Roll note editing (final-spec 4.5). validSelection / allNoteIndices are complete; everything marked "WP0 STUB" is
// WP3's to implement (the stubs return their input sanitised with the selection validated, and the queries nothing).

namespace cb
{
std::vector<int> validSelection (const Melody& melody, const std::vector<int>& selection)
{
    std::vector<int> out;
    for (int i : selection)
        if (i >= 0 && i < (int) melody.notes.size())
            out.push_back (i);
    std::sort (out.begin(), out.end());
    out.erase (std::unique (out.begin(), out.end()), out.end());
    return out;
}

std::vector<int> allNoteIndices (const Melody& melody)
{
    std::vector<int> out ((size_t) melody.notes.size());
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = (int) i;
    return out;
}

namespace
{
NoteEditResult unchanged (const Melody& melody, const std::vector<int>& selection, const NoteLimits& limits)
{
    return { sanitiseMelody (melody, limits), validSelection (melody, selection) };
}
} // namespace

// WP0 STUB
std::vector<int> notesInArea (const Melody&, double, double, int, int) { return {}; }

// WP0 STUB
std::vector<int> notesStartingIn (const Melody&, double, double) { return {}; }

// WP0 STUB
std::vector<int> findNotes (const Melody&, const std::vector<MelodyNote>&) { return {}; }

// WP0 STUB
NoteEditResult moveNotes (const Melody& melody, const std::vector<int>& selection, double, int,
                          const NoteEditContext& context)
{
    return unchanged (melody, selection, context.limits);
}

// WP0 STUB
NoteEditResult resizeNotes (const Melody& melody, const std::vector<int>& selection, double, double,
                            const NoteLimits& limits)
{
    return unchanged (melody, selection, limits);
}

// WP0 STUB
double defaultDuplicateOffset (const Melody&, const std::vector<int>&, double)
{
    return 4.0;
}

// WP0 STUB
NoteEditResult duplicateNotes (const Melody& melody, const std::vector<int>& selection, double, const NoteLimits& limits)
{
    return unchanged (melody, selection, limits);
}

// WP0 STUB
NoteEditResult deleteNotes (const Melody& melody, const std::vector<int>& selection, const NoteLimits& limits)
{
    return unchanged (melody, selection, limits);
}

// WP0 STUB
NoteEditResult setNoteVelocity (const Melody& melody, const std::vector<int>& selection, float, const NoteLimits& limits)
{
    return unchanged (melody, selection, limits);
}

// WP0 STUB
NoteEditResult offsetNoteVelocity (const Melody& melody, const std::vector<int>& selection, float,
                                   const NoteLimits& limits)
{
    return unchanged (melody, selection, limits);
}

// WP0 STUB
NoteEditResult transposeNotes (const Melody& melody, const std::vector<int>& selection, int, const NoteLimits& limits)
{
    return unchanged (melody, selection, limits);
}

// WP0 STUB
NoteEditResult transposeNotesInScale (const Melody& melody, const std::vector<int>& selection, int, const Key&,
                                      const NoteLimits& limits)
{
    return unchanged (melody, selection, limits);
}

// WP0 STUB
NoteEditResult quantizeNotes (const Melody& melody, const std::vector<int>& selection, double, const NoteLimits& limits)
{
    return unchanged (melody, selection, limits);
}

// WP0 STUB
NoteEditResult legatoNotes (const Melody& melody, const std::vector<int>& selection, const NoteLimits& limits)
{
    return unchanged (melody, selection, limits);
}

// WP0 STUB
NoteClip copyNotes (const Melody&, const std::vector<int>&, double)
{
    return {};
}

// WP0 STUB
NoteEditResult pasteNotes (const Melody& melody, const NoteClip&, double, const NoteLimits& limits)
{
    return { sanitiseMelody (melody, limits), {} };
}
} // namespace cb
