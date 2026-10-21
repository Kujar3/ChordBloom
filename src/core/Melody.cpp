#include "core/Melody.h"

#include <algorithm>
#include <cmath>

namespace cb
{
namespace
{
constexpr double kTicksPerBeat = 48.0;   // 1 / kMelodyTick
constexpr double kEpsilon = 1.0e-9;

double roundToTick (double beats) { return std::round (beats * kTicksPerBeat) / kTicksPerBeat; }

bool inChord (int pitchClass, const Chord& chord)
{
    for (int p : chord.pitchClasses())
        if (pc (p) == pitchClass)
            return true;

    return false;
}

bool allowedBySnap (int pitchClass, MelodySnap snap, const Key& key, const std::optional<Chord>& chord)
{
    switch (snap)
    {
        case MelodySnap::Scale:
            return key.contains (pitchClass) || (chord.has_value() && inChord (pitchClass, *chord));

        case MelodySnap::ChordTones:
            return chord.has_value() ? inChord (pitchClass, *chord) : key.contains (pitchClass);

        case MelodySnap::Off:
        case MelodySnap::NumModes:
            break;
    }

    return true;
}

// NoteLimits as the functions use them: pitches clamped to 0..127, highest never below lowest, a non-negative cap.
struct ResolvedLimits
{
    int low, high, maxNotes;
};

ResolvedLimits resolve (const NoteLimits& limits)
{
    const int low = std::clamp (limits.lowest, 0, 127);
    return { low, std::max (low, std::clamp (limits.highest, 0, 127)), std::max (0, limits.maxNotes) };
}

bool canonicalLess (const MelodyNote& a, const MelodyNote& b)
{
    if (a.startBeat != b.startBeat)
        return a.startBeat < b.startBeat;

    if (a.note != b.note)
        return a.note < b.note;

    if (a.lengthBeats != b.lengthBeats)
        return a.lengthBeats < b.lengthBeats;

    return a.velocity < b.velocity;
}
} // namespace

//==============================================================================
Melody sanitiseMelody (const Melody& in)
{
    return sanitiseMelody (in, kMelodyLimits);
}

Melody sanitiseMelody (const Melody& in, const NoteLimits& limits)
{
    const auto lim = resolve (limits);

    Melody out;
    out.notes.reserve (std::min (in.notes.size(), (size_t) lim.maxNotes * 2));

    const double latestStart = kMaxMelodyBeat - kMinMelodyNoteBeats;

    for (auto n : in.notes)
    {
        if (! std::isfinite (n.startBeat) || ! std::isfinite (n.lengthBeats))
            continue;

        n.note = std::clamp (n.note, lim.low, lim.high);
        n.velocity = std::isfinite (n.velocity) ? std::clamp (n.velocity, 0.05f, 1.0f) : kDefaultMelodyVelocity;
        n.startBeat = roundToTick (std::clamp (n.startBeat, 0.0, latestStart));

        const double room = kMaxMelodyBeat - n.startBeat;
        n.lengthBeats = roundToTick (std::clamp (n.lengthBeats, kMinMelodyNoteBeats, std::min (kMaxMelodyNoteBeats, room)));
        n.lengthBeats = std::max (n.lengthBeats, kMinMelodyNoteBeats);

        out.notes.push_back (n);
    }

    std::sort (out.notes.begin(), out.notes.end(), canonicalLess);

    // Same pitch and start: only the longest (the last of the run in canonical order) stays.
    std::vector<MelodyNote> unique;
    unique.reserve (out.notes.size());

    for (size_t i = 0; i < out.notes.size(); ++i)
    {
        const bool lastOfRun = i + 1 == out.notes.size() || out.notes[i + 1].startBeat != out.notes[i].startBeat
                            || out.notes[i + 1].note != out.notes[i].note;

        if (lastOfRun)
            unique.push_back (out.notes[i]);
    }

    if ((int) unique.size() > lim.maxNotes)
        unique.resize ((size_t) lim.maxNotes);

    out.notes = std::move (unique);
    return out;
}

int melodyNoteAt (const Melody& melody, double beat, int pitch)
{
    int best = -1;

    for (int i = 0; i < (int) melody.notes.size(); ++i)
    {
        const auto& n = melody.notes[(size_t) i];

        if (n.note != pitch || beat < n.startBeat || beat >= n.startBeat + n.lengthBeats)
            continue;

        if (best < 0 || n.startBeat >= melody.notes[(size_t) best].startBeat)
            best = i;
    }

    return best;
}

//==============================================================================
ToneRole toneRole (int pitchClass, const Key& key, const Chord* chord)
{
    pitchClass = pc (pitchClass);

    if (chord != nullptr)
    {
        if (pc (chord->root) == pitchClass)
            return ToneRole::ChordRoot;

        if (inChord (pitchClass, *chord))
            return ToneRole::ChordTone;
    }

    return key.contains (pitchClass) ? ToneRole::Scale : ToneRole::Outside;
}

std::optional<Chord> chordAtBeat (const Progression& progression, double beat)
{
    const int slot = progression.slotAtBeat (beat);

    if (slot < 0)
        return std::nullopt;

    return progression.slots[(size_t) slot].chord;
}

//==============================================================================
const char* melodySnapName (MelodySnap snap)
{
    switch (snap)
    {
        case MelodySnap::Off:        return "Off";
        case MelodySnap::Scale:      return "Scale";
        case MelodySnap::ChordTones: return "Chord";
        case MelodySnap::NumModes:   break;
    }

    return "Off";
}

int snapMelodyPitch (int note, MelodySnap snap, const Key& key, const std::optional<Chord>& chord)
{
    return snapMelodyPitch (note, snap, key, chord, kMelodyLimits);
}

int snapMelodyPitch (int note, MelodySnap snap, const Key& key, const std::optional<Chord>& chord,
                     const NoteLimits& limits)
{
    const auto lim = resolve (limits);
    note = std::clamp (note, lim.low, lim.high);

    if (snap == MelodySnap::Off)
        return note;

    for (int d = 0; d <= 12; ++d)
    {
        const int down = note - d, up = note + d;

        if (down >= lim.low && allowedBySnap (pc (down), snap, key, chord))
            return down;

        if (up <= lim.high && allowedBySnap (pc (up), snap, key, chord))
            return up;
    }

    return note;
}

double snapBeatToGrid (double beat, double grid, bool roundToNearest)
{
    if (! (grid > 0.0) || ! std::isfinite (grid) || ! std::isfinite (beat))
        return beat;

    if (roundToNearest)
        return std::round (beat / grid) * grid;

    return std::floor ((beat + 1.0e-6) / grid) * grid;
}

//==============================================================================
std::vector<NoteEvent> renderMelody (const Melody& melodyIn, double loopBeats, int channel)
{
    return renderMelody (melodyIn, loopBeats, channel, kMelodyLimits);
}

std::vector<NoteEvent> renderMelody (const Melody& melodyIn, double loopBeats, int channel, const NoteLimits& limits)
{
    std::vector<NoteEvent> events;

    if (! (loopBeats > 0.0) || ! std::isfinite (loopBeats))
        return events;

    const auto melody = sanitiseMelody (melodyIn, limits);

    for (const auto& n : melody.notes)
    {
        if (n.startBeat >= loopBeats - kEpsilon)
            continue;

        NoteEvent e;
        e.note = n.note;
        e.velocity = n.velocity;
        e.startBeat = n.startBeat;
        e.lengthBeats = std::min (n.startBeat + n.lengthBeats, loopBeats) - n.startBeat;
        e.channel = channel;
        events.push_back (e);
    }

    // Same pitch: an earlier note ends where the next one starts (sanitised notes never share a start).
    std::sort (events.begin(), events.end(), [] (const NoteEvent& a, const NoteEvent& b)
    {
        return a.note != b.note ? a.note < b.note : a.startBeat < b.startBeat;
    });

    for (size_t i = 0; i + 1 < events.size(); ++i)
    {
        auto& e = events[i];
        const auto& next = events[i + 1];

        if (next.note == e.note && e.startBeat + e.lengthBeats > next.startBeat)
            e.lengthBeats = next.startBeat - e.startBeat;
    }

    events.erase (std::remove_if (events.begin(), events.end(), [] (const NoteEvent& e) { return e.lengthBeats <= kEpsilon; }),
                  events.end());

    std::sort (events.begin(), events.end(), [] (const NoteEvent& a, const NoteEvent& b)
    {
        return a.startBeat != b.startBeat ? a.startBeat < b.startBeat : a.note < b.note;
    });

    return events;
}

Melody transposeMelody (const Melody& melody, int semitones)
{
    return transposeMelody (melody, semitones, kMelodyLimits);
}

Melody transposeMelody (const Melody& melody, int semitones, const NoteLimits& limits)
{
    const auto lim = resolve (limits);
    int shift = pc (semitones);

    if (shift > 6)
        shift -= 12;

    Melody out = melody;

    for (auto& n : out.notes)
    {
        int moved = n.note + shift;

        if (moved > lim.high)
            moved -= 12;
        else if (moved < lim.low)
            moved += 12;

        n.note = moved;
    }

    return sanitiseMelody (out, limits);
}

Melody melodyFollowingSlots (const Melody& melodyIn, const Progression& before, const Progression& after,
                             const std::vector<int>& sourceSlot)
{
    return melodyFollowingSlots (melodyIn, before, after, sourceSlot, kMelodyLimits);
}

Melody melodyFollowingSlots (const Melody& melodyIn, const Progression& before, const Progression& after,
                             const std::vector<int>& sourceSlot, const NoteLimits& limits)
{
    const auto melody = sanitiseMelody (melodyIn, limits);

    if (sourceSlot.size() != after.slots.size())
        return melody;

    const double beforeTotal = (double) before.totalBeats();
    const double afterTotal = (double) after.totalBeats();
    Melody out;

    for (const auto& n : melody.notes)
    {
        if (n.startBeat >= beforeTotal)
        {
            auto moved = n;
            moved.startBeat = n.startBeat - beforeTotal + afterTotal;
            out.notes.push_back (moved);
            continue;
        }

        const int slot = before.slotAtBeat (n.startBeat);

        if (slot < 0)
            continue;

        const double offset = n.startBeat - before.slotStartBeat (slot);

        for (size_t i = 0; i < sourceSlot.size(); ++i)
        {
            if (sourceSlot[i] != slot || offset >= (double) after.slots[i].lengthBeats - kEpsilon)
                continue;

            auto moved = n;
            moved.startBeat = after.slotStartBeat ((int) i) + offset;
            out.notes.push_back (moved);
        }
    }

    return sanitiseMelody (out, limits);
}

} // namespace cb
