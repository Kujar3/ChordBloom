#include "core/NoteEdit.h"

#include <algorithm>
#include <cmath>

// Roll note editing (final-spec 4.5). Every operation works on the notes of the input list, builds the edited list,
// sanitises it for the limits and finds the notes it kept selected, moved or produced again by (pitch, start).

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
constexpr double kStartTolerance = 1.0e-6;

struct ResolvedLimits
{
    int low, high;
};

ResolvedLimits resolve (const NoteLimits& limits)
{
    const int low = std::clamp (limits.lowest, 0, 127);
    return { low, std::max (low, std::clamp (limits.highest, 0, 127)) };
}

// Octave-folds `note` into [low, high] (clamps when the range cannot hold the pitch class).
int foldIntoLimits (int note, const ResolvedLimits& lim)
{
    if (note > lim.high)
        note -= 12 * ((note - lim.high + 11) / 12);
    else if (note < lim.low)
        note += 12 * ((lim.low - note + 11) / 12);

    return std::clamp (note, lim.low, lim.high);
}

double roundToTick (double beats) { return std::round (beats / kMelodyTick) * kMelodyTick; }

int floorDiv (int a, int b)
{
    int q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0)))
        --q;
    return q;
}

// The selected notes of `melody` (valid indices only), in index order.
std::vector<MelodyNote> pickNotes (const Melody& melody, const std::vector<int>& selection)
{
    std::vector<MelodyNote> out;
    for (int i : validSelection (melody, selection))
        out.push_back (melody.notes[(size_t) i]);
    return out;
}

// Membership flags of the valid selection, one per note of the list.
std::vector<char> selectionFlags (const Melody& melody, const std::vector<int>& selection)
{
    std::vector<char> flags (melody.notes.size(), 0);
    for (int i : validSelection (melody, selection))
        flags[(size_t) i] = 1;
    return flags;
}

// Sanitises `all` and re-identifies the `selected` notes in it by (pitch, start) (after the same sanitising).
NoteEditResult finish (const Melody& all, const std::vector<MelodyNote>& selected, const NoteLimits& limits)
{
    NoteEditResult result;
    result.notes = sanitiseMelody (all, limits);

    Melody wanted;
    wanted.notes = selected;
    result.selection = findNotes (result.notes, sanitiseMelody (wanted, limits).notes);
    return result;
}

// The input sanitised, with its selection kept.
NoteEditResult unchanged (const Melody& melody, const std::vector<int>& selection, const NoteLimits& limits)
{
    return finish (melody, pickNotes (melody, selection), limits);
}

// A note list where every selected note went through `edit` (a callable taking MelodyNote&).
template <typename Edit>
NoteEditResult editSelected (const Melody& melody, const std::vector<int>& selection, const NoteLimits& limits,
                             Edit&& edit)
{
    const auto flags = selectionFlags (melody, selection);
    Melody all;
    std::vector<MelodyNote> selected;
    all.notes.reserve (melody.notes.size());

    for (size_t i = 0; i < melody.notes.size(); ++i)
    {
        auto n = melody.notes[i];

        if (flags[i])
        {
            edit (n);
            selected.push_back (n);
        }

        all.notes.push_back (n);
    }

    return finish (all, selected, limits);
}

// Earliest finite start of `notes` (0 when there is none), used to stop a negative offset at beat 0.
double earliestStart (const std::vector<MelodyNote>& notes)
{
    double first = 0.0;
    bool any = false;

    for (const auto& n : notes)
        if (std::isfinite (n.startBeat))
        {
            first = any ? std::min (first, n.startBeat) : n.startBeat;
            any = true;
        }

    return std::max (first, 0.0);
}

// Scale position helpers for transposeNotesInScale: index n = 7 * octave + degree of `key`.
int scalePitchAt (const Key& key, int index)
{
    const int octave = floorDiv (index, 7);
    return key.tonic + 12 * octave + scaleInfo (key.scale).offsets[(size_t) (index - 7 * octave)];
}
} // namespace

std::vector<int> notesInArea (const Melody& melody, double beatFrom, double beatTo, int pitchLow, int pitchHigh)
{
    if (beatFrom > beatTo)
        std::swap (beatFrom, beatTo);

    if (pitchLow > pitchHigh)
        std::swap (pitchLow, pitchHigh);

    std::vector<int> out;

    for (int i = 0; i < (int) melody.notes.size(); ++i)
    {
        const auto& n = melody.notes[(size_t) i];

        if (n.note >= pitchLow && n.note <= pitchHigh && n.startBeat < beatTo && n.startBeat + n.lengthBeats > beatFrom)
            out.push_back (i);
    }

    return out;
}

std::vector<int> notesStartingIn (const Melody& melody, double beatFrom, double beatTo)
{
    if (beatFrom > beatTo)
        std::swap (beatFrom, beatTo);

    std::vector<int> out;

    for (int i = 0; i < (int) melody.notes.size(); ++i)
    {
        const double start = melody.notes[(size_t) i].startBeat;

        if (start >= beatFrom - 1.0e-9 && start < beatTo - 1.0e-9)
            out.push_back (i);
    }

    return out;
}

std::vector<int> findNotes (const Melody& melody, const std::vector<MelodyNote>& wanted)
{
    struct Entry
    {
        double start;
        int note;
        int index;
    };

    std::vector<Entry> entries;
    entries.reserve (melody.notes.size());

    for (int i = 0; i < (int) melody.notes.size(); ++i)
        if (std::isfinite (melody.notes[(size_t) i].startBeat))
            entries.push_back ({ melody.notes[(size_t) i].startBeat, melody.notes[(size_t) i].note, i });

    std::sort (entries.begin(), entries.end(), [] (const Entry& a, const Entry& b)
    {
        return a.start != b.start ? a.start < b.start : (a.note != b.note ? a.note < b.note : a.index < b.index);
    });

    std::vector<int> out;

    for (const auto& w : wanted)
    {
        if (! std::isfinite (w.startBeat))
            continue;

        auto it = std::lower_bound (entries.begin(), entries.end(), w.startBeat - kStartTolerance,
                                    [] (const Entry& e, double value) { return e.start < value; });
        int best = -1;

        for (; it != entries.end() && it->start <= w.startBeat + kStartTolerance; ++it)
            if (it->note == w.note && (best < 0 || it->index < best))
                best = it->index;

        if (best >= 0)
            out.push_back (best);
    }

    std::sort (out.begin(), out.end());
    out.erase (std::unique (out.begin(), out.end()), out.end());
    return out;
}

NoteEditResult moveNotes (const Melody& melody, const std::vector<int>& selection, double deltaBeats,
                          int deltaSemitones, const NoteEditContext& context)
{
    const auto selected = pickNotes (melody, selection);
    const auto lim = resolve (context.limits);

    if (selected.empty())
        return unchanged (melody, selection, context.limits);

    // One clamp for the whole group, so no note is ever folded or squeezed on its own. Notes that already lie outside
    // the allowed region can still move back towards it.
    double minStart = 0.0, maxStart = 0.0;
    int minPitch = 0, maxPitch = 0;
    bool first = true;

    for (const auto& n : selected)
    {
        if (! std::isfinite (n.startBeat))
            continue;

        if (first)
        {
            minStart = maxStart = n.startBeat;
            minPitch = maxPitch = n.note;
            first = false;
        }
        else
        {
            minStart = std::min (minStart, n.startBeat);
            maxStart = std::max (maxStart, n.startBeat);
            minPitch = std::min (minPitch, n.note);
            maxPitch = std::max (maxPitch, n.note);
        }
    }

    if (first)
        return unchanged (melody, selection, context.limits);

    if (! std::isfinite (deltaBeats))
        deltaBeats = 0.0;

    // The delta is rounded to the tick first (as every start is afterwards), so a sub-tick drag is a true zero move.
    const double maxBeat = std::isfinite (context.maxBeat) ? std::min (context.maxBeat, kMaxMelodyBeat) : kMaxMelodyBeat;
    const double beatLow = std::min (-minStart, 0.0);
    const double beatHigh = std::max ((maxBeat - kMinMelodyNoteBeats) - maxStart, 0.0);
    deltaBeats = std::clamp (roundToTick (deltaBeats), beatLow, beatHigh);

    const int pitchLow = std::min (lim.low - minPitch, 0);
    const int pitchHigh = std::max (lim.high - maxPitch, 0);
    deltaSemitones = std::clamp (deltaSemitones, pitchLow, pitchHigh);

    if (deltaBeats == 0.0 && deltaSemitones == 0)
        return unchanged (melody, selection, context.limits);

    return editSelected (melody, selection, context.limits, [&] (MelodyNote& n)
    {
        n.startBeat = roundToTick (n.startBeat + deltaBeats);
        n.note += deltaSemitones;

        if (context.snap != MelodySnap::Off)
        {
            std::optional<Chord> chord;

            if (context.progression != nullptr)
                chord = chordAtBeat (*context.progression, n.startBeat);

            n.note = snapMelodyPitch (n.note, context.snap, context.key, chord, context.limits);
        }
    });
}

NoteEditResult resizeNotes (const Melody& melody, const std::vector<int>& selection, double deltaBeats,
                            double minLength, const NoteLimits& limits)
{
    if (! std::isfinite (deltaBeats))
        deltaBeats = 0.0;

    if (! std::isfinite (minLength))
        minLength = kMinMelodyNoteBeats;

    minLength = std::clamp (minLength, kMinMelodyNoteBeats, kMaxMelodyNoteBeats);

    return editSelected (melody, selection, limits, [&] (MelodyNote& n)
    {
        if (! std::isfinite (n.startBeat) || ! std::isfinite (n.lengthBeats))
            return;

        double length = std::clamp (n.lengthBeats + deltaBeats, minLength, kMaxMelodyNoteBeats);
        length = std::min (length, kMaxMelodyBeat - n.startBeat);
        n.lengthBeats = length;
    });
}

double defaultDuplicateOffset (const Melody& melody, const std::vector<int>& selection, double grid)
{
    const auto selected = pickNotes (melody, selection);
    double first = 0.0, last = 0.0;
    bool any = false;

    for (const auto& n : selected)
    {
        if (! std::isfinite (n.startBeat) || ! std::isfinite (n.lengthBeats))
            continue;

        first = any ? std::min (first, n.startBeat) : n.startBeat;
        last = any ? std::max (last, n.startBeat + n.lengthBeats) : n.startBeat + n.lengthBeats;
        any = true;
    }

    if (! any)
        return 4.0;

    const bool useGrid = grid > 0.0 && std::isfinite (grid);
    double offset = last - first;

    if (useGrid)
        offset = std::ceil (offset / grid - kStartTolerance) * grid;

    if (offset >= 1.0 - kStartTolerance)
        offset = std::ceil (offset - kStartTolerance);

    if (offset > 2.0 + kStartTolerance)
        offset = std::ceil (offset / 4.0 - kStartTolerance) * 4.0;

    return std::max (offset, useGrid ? grid : kMelodyTick);
}

NoteEditResult duplicateNotes (const Melody& melody, const std::vector<int>& selection, double offsetBeats,
                               const NoteLimits& limits)
{
    if (! std::isfinite (offsetBeats))
        return unchanged (melody, selection, limits);

    Melody all = melody;
    std::vector<MelodyNote> copies;
    const auto source = pickNotes (melody, selection);

    // One clamp for the whole group: a negative offset stops where the earliest copy starts at beat 0.
    offsetBeats = std::max (offsetBeats, -earliestStart (source));

    for (auto n : source)
    {
        n.startBeat += offsetBeats;

        if (std::isfinite (n.startBeat) && n.startBeat < kMaxMelodyBeat)
            copies.push_back (n);
    }

    all.notes.insert (all.notes.end(), copies.begin(), copies.end());
    return finish (all, copies, limits);
}

NoteEditResult deleteNotes (const Melody& melody, const std::vector<int>& selection, const NoteLimits& limits)
{
    const auto flags = selectionFlags (melody, selection);
    Melody rest;

    for (size_t i = 0; i < melody.notes.size(); ++i)
        if (! flags[i])
            rest.notes.push_back (melody.notes[i]);

    return { sanitiseMelody (rest, limits), {} };
}

NoteEditResult setNoteVelocity (const Melody& melody, const std::vector<int>& selection, float velocity,
                                const NoteLimits& limits)
{
    if (! std::isfinite (velocity))
        return unchanged (melody, selection, limits);

    const float v = std::clamp (velocity, 0.05f, 1.0f);
    return editSelected (melody, selection, limits, [v] (MelodyNote& n) { n.velocity = v; });
}

NoteEditResult offsetNoteVelocity (const Melody& melody, const std::vector<int>& selection, float delta,
                                   const NoteLimits& limits)
{
    if (! std::isfinite (delta))
        return unchanged (melody, selection, limits);

    return editSelected (melody, selection, limits, [delta] (MelodyNote& n)
    {
        const float base = std::isfinite (n.velocity) ? n.velocity : kDefaultMelodyVelocity;
        n.velocity = std::clamp (base + delta, 0.05f, 1.0f);
    });
}

NoteEditResult transposeNotes (const Melody& melody, const std::vector<int>& selection, int semitones,
                               const NoteLimits& limits)
{
    const auto lim = resolve (limits);

    // No delta beyond an octave-span can land inside 0..127; refusing here keeps the sums below free of overflow.
    if (semitones < -127 || semitones > 127)
        return unchanged (melody, selection, limits);

    for (const auto& n : pickNotes (melody, selection))
    {
        const int moved = n.note + semitones;

        if (moved < lim.low || moved > lim.high)
            return unchanged (melody, selection, limits);
    }

    return editSelected (melody, selection, limits, [semitones] (MelodyNote& n) { n.note += semitones; });
}

NoteEditResult transposeNotesInScale (const Melody& melody, const std::vector<int>& selection, int steps,
                                      const Key& key, const NoteLimits& limits)
{
    const auto lim = resolve (limits);
    const auto& offsets = scaleInfo (key.scale).offsets;

    if (steps < -128 || steps > 128)
        return unchanged (melody, selection, limits);

    auto transposed = [&] (int pitch)
    {
        if (steps == 0)
            return pitch;

        const int relative = pitch - key.tonic;
        const int octave = floorDiv (relative, 12);
        const int within = relative - 12 * octave;
        int degree = -1;

        for (int d = 0; d < 7; ++d)
            if (offsets[(size_t) d] == within)
                degree = d;

        if (degree >= 0)
            return scalePitchAt (key, octave * 7 + degree + steps);

        // Not a scale tone: the nearest scale tone in the direction of travel is the first step.
        int next = octave * 7 + 7;

        for (int n = octave * 7 + 1; n <= octave * 7 + 7; ++n)
            if (scalePitchAt (key, n) > pitch)
            {
                next = n;
                break;
            }

        return scalePitchAt (key, steps > 0 ? next + steps - 1 : next + steps);
    };

    for (const auto& n : pickNotes (melody, selection))
    {
        const int moved = transposed (n.note);

        if (moved < lim.low || moved > lim.high)
            return unchanged (melody, selection, limits);
    }

    return editSelected (melody, selection, limits, [&] (MelodyNote& n) { n.note = transposed (n.note); });
}

NoteEditResult quantizeNotes (const Melody& melody, const std::vector<int>& selection, double grid,
                              const NoteLimits& limits)
{
    if (! (grid > 0.0) || ! std::isfinite (grid))
        return unchanged (melody, selection, limits);

    return editSelected (melody, selection, limits, [grid] (MelodyNote& n)
    {
        if (std::isfinite (n.startBeat))
            n.startBeat = snapBeatToGrid (n.startBeat, grid, true);
    });
}

NoteEditResult legatoNotes (const Melody& melody, const std::vector<int>& selection, const NoteLimits& limits)
{
    std::vector<double> starts;
    starts.reserve (melody.notes.size());

    for (const auto& n : melody.notes)
        if (std::isfinite (n.startBeat))
            starts.push_back (n.startBeat);

    std::sort (starts.begin(), starts.end());

    return editSelected (melody, selection, limits, [&] (MelodyNote& n)
    {
        if (! std::isfinite (n.startBeat))
            return;

        const auto it = std::upper_bound (starts.begin(), starts.end(), n.startBeat + 1.0e-9);

        if (it != starts.end())
            n.lengthBeats = std::min (*it - n.startBeat, kMaxMelodyNoteBeats);
    });
}

NoteClip copyNotes (const Melody& melody, const std::vector<int>& selection, double grid)
{
    NoteClip clip;
    const auto selected = pickNotes (melody, selection);

    if (selected.empty())
        return clip;

    double first = 0.0;
    bool any = false;

    for (const auto& n : selected)
        if (std::isfinite (n.startBeat))
        {
            first = any ? std::min (first, n.startBeat) : n.startBeat;
            any = true;
        }

    if (! any)
        return clip;

    clip.sourceBeat = grid > 0.0 && std::isfinite (grid) ? snapBeatToGrid (first, grid, false) : first;

    for (auto n : selected)
    {
        if (! std::isfinite (n.startBeat))
            continue;

        n.startBeat -= clip.sourceBeat;
        clip.notes.notes.push_back (n);
    }

    return clip;
}

NoteEditResult pasteNotes (const Melody& melody, const NoteClip& clip, double atBeat, const NoteLimits& limits)
{
    const auto lim = resolve (limits);
    Melody all = melody;
    std::vector<MelodyNote> pasted;

    if (std::isfinite (atBeat))
    {
        // Same group clamp as duplicateNotes: the earliest pasted note never starts before beat 0.
        atBeat = std::max (atBeat, -earliestStart (clip.notes.notes));

        for (auto n : clip.notes.notes)
        {
            n.startBeat += atBeat;
            n.note = foldIntoLimits (n.note, lim);

            if (std::isfinite (n.startBeat) && n.startBeat < kMaxMelodyBeat)
                pasted.push_back (n);
        }
    }

    all.notes.insert (all.notes.end(), pasted.begin(), pasted.end());
    return finish (all, pasted, limits);
}
} // namespace cb
