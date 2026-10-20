#include "core/Layers.h"

#include <algorithm>
#include <cctype>
#include <cmath>

// Layers (final-spec 4.4): identity / channel / limit data, the auto render as editable notes, and keeping custom
// notes on their chords through progression edits.

namespace cb
{
const char* layerName (Layer layer)
{
    switch (layer)
    {
        case Layer::Melody:    return "Melody";
        case Layer::Arp:       return "Arp";
        case Layer::Bass:      return "Bass";
        case Layer::NumLayers:
        default:               break;
    }
    return "Melody";
}

const char* layerKey (Layer layer)
{
    switch (layer)
    {
        case Layer::Melody:    return "melody";
        case Layer::Arp:       return "arp";
        case Layer::Bass:      return "bass";
        case Layer::NumLayers:
        default:               break;
    }
    return "melody";
}

std::optional<Layer> layerFromKey (const std::string& key)
{
    std::string lower (key);
    std::transform (lower.begin(), lower.end(), lower.begin(),
                    [] (unsigned char c) { return (char) std::tolower (c); });

    for (int i = 0; i < numLayers; ++i)
        if (lower == layerKey ((Layer) i))
            return (Layer) i;
    return std::nullopt;
}

int layerMidiChannel (Layer layer)
{
    switch (layer)
    {
        case Layer::Melody:    return 3;
        case Layer::Arp:       return 4;
        case Layer::Bass:      return 2;
        case Layer::NumLayers:
        default:               break;
    }
    return 3;
}

NoteLimits layerLimits (Layer layer)
{
    switch (layer)
    {
        case Layer::Arp:       return { 28, 108, kMaxLayerNotes };
        case Layer::Bass:      return { 24, 72, kMaxLayerNotes };
        case Layer::Melody:
        case Layer::NumLayers:
        default:               break;
    }
    return kMelodyLimits;
}

//==============================================================================
namespace
{
constexpr double kEpsilon = 1.0e-9;

struct ResolvedLimits
{
    int low, high;
};

ResolvedLimits resolve (const NoteLimits& limits)
{
    const int low = std::clamp (limits.lowest, 0, 127);
    return { low, std::max (low, std::clamp (limits.highest, 0, 127)) };
}

// Octave-folds `note` into [low, high] (clamps when the range is narrower than an octave and cannot hold the pitch
// class).
int foldIntoLimits (int note, const ResolvedLimits& lim)
{
    if (note > lim.high)
        note -= 12 * ((note - lim.high + 11) / 12);
    else if (note < lim.low)
        note += 12 * ((lim.low - note + 11) / 12);

    return std::clamp (note, lim.low, lim.high);
}

// The chord's pitch classes, rotated so that the bass pitch class comes first when it is a chord tone.
std::vector<int> bassRotatedTones (const Chord& chord, int bassPc)
{
    auto tones = chord.pitchClasses();
    const auto it = std::find (tones.begin(), tones.end(), pc (bassPc));

    if (it != tones.end())
        std::rotate (tones.begin(), it, tones.end());

    return tones;
}

// The scale tone of `key` nearest to `pitchClass` (ties down), as a pitch class.
int nearestScalePc (int pitchClass, const Key& key)
{
    for (int d = 0; d <= 6; ++d)
    {
        if (key.contains (pc (pitchClass - d)))
            return pc (pitchClass - d);

        if (key.contains (pc (pitchClass + d)))
            return pc (pitchClass + d);
    }

    return pc (pitchClass);
}

int bassPcOf (const std::vector<int>& list, int slot, const Progression& progression)
{
    if (slot >= 0 && slot < (int) list.size())
        return pc (list[(size_t) slot]);

    return slot >= 0 && slot < (int) progression.slots.size() ? pc (progression.slots[(size_t) slot].chord.root) : 0;
}

bool startsIn (const MelodyNote& n, double from, double to)
{
    return n.startBeat >= from - kEpsilon && n.startBeat < to - kEpsilon;
}

// The notes of `notes` (order kept) that start in [from, to).
std::vector<MelodyNote> startingIn (const std::vector<MelodyNote>& notes, double from, double to)
{
    std::vector<MelodyNote> out;

    for (const auto& n : notes)
        if (startsIn (n, from, to))
            out.push_back (n);

    return out;
}
} // namespace

Melody notesFromEvents (const std::vector<NoteEvent>& events, const NoteLimits& limits)
{
    const auto lim = resolve (limits);
    Melody raw;
    raw.notes.reserve (events.size());

    for (const auto& e : events)
    {
        if (! std::isfinite (e.startBeat) || ! std::isfinite (e.lengthBeats))
            continue;

        MelodyNote n;
        n.note = foldIntoLimits (e.note, lim);
        n.startBeat = std::round (e.startBeat / kMelodyTick) * kMelodyTick;
        n.lengthBeats = std::max (kMinMelodyNoteBeats, std::round (e.lengthBeats / kMelodyTick) * kMelodyTick);
        n.velocity = e.velocity;
        raw.notes.push_back (n);
    }

    return sanitiseMelody (raw, limits);
}

bool sameStructure (const Progression& a, const Progression& b)
{
    if (a.slots.size() != b.slots.size())
        return false;
    for (size_t i = 0; i < a.slots.size(); ++i)
        if (a.slots[i].lengthBeats != b.slots[i].lengthBeats)
            return false;
    return true;
}

int remapPitchToChord (int note, const Chord& from, int fromBassPc, const Chord& to, int toBassPc, const Key& key,
                       const NoteLimits& limits)
{
    const auto lim = resolve (limits);

    fromBassPc = pc (fromBassPc);
    toBassPc = pc (toBassPc);

    if (from == to && fromBassPc == toBassPc)
        return foldIntoLimits (note, lim);

    const int notePc = pc (note);
    int targetPc = 0;

    if (notePc == fromBassPc)
    {
        targetPc = toBassPc;                                           // 1. the bass keeps being the bass
    }
    else
    {
        const auto fromTones = bassRotatedTones (from, fromBassPc);
        const auto it = std::find (fromTones.begin(), fromTones.end(), notePc);

        if (it != fromTones.end())                                     // 2. the same tone of the rotated order
        {
            const auto toTones = bassRotatedTones (to, toBassPc);
            const size_t k = (size_t) (it - fromTones.begin());
            targetPc = toTones.empty() ? pc (to.root) : toTones[k < toTones.size() ? k : 0];
        }
        else                                                           // 3. the interval above the root
        {
            targetPc = pc (to.root + pc (notePc - from.root));

            const auto toTones = to.pitchClasses();
            const bool inTo = std::find (toTones.begin(), toTones.end(), targetPc) != toTones.end();

            if (! key.contains (targetPc) && ! inTo)
                targetPc = nearestScalePc (targetPc, key);
        }
    }

    int diff = pc (targetPc - note);   // 0..11 upwards

    if (diff > 6)
        diff -= 12;
    else if (diff == 6)
        diff = -6;                     // a tritone away: ties down

    return foldIntoLimits (note + diff, lim);
}

std::vector<bool> editedSlots (const Melody& custom, const Melody& autoNotes, const Progression& progression)
{
    std::vector<bool> edited (progression.slots.size(), false);

    for (size_t i = 0; i < progression.slots.size(); ++i)
    {
        const double from = progression.slotStartBeat ((int) i);
        const double to = from + progression.slots[i].lengthBeats;
        edited[i] = startingIn (custom.notes, from, to) != startingIn (autoNotes.notes, from, to);
    }

    return edited;
}

Melody replaceSlotNotes (const Melody& custom, const Melody& autoNotes, const Progression& progression, int slot,
                         const NoteLimits& limits)
{
    if (slot < 0 || slot >= (int) progression.slots.size())
        return sanitiseMelody (custom, limits);

    const double from = progression.slotStartBeat (slot);
    const double to = from + progression.slots[(size_t) slot].lengthBeats;

    Melody out;

    for (const auto& n : custom.notes)
        if (! startsIn (n, from, to))
            out.notes.push_back (n);

    for (const auto& n : startingIn (autoNotes.notes, from, to))
        out.notes.push_back (n);

    return sanitiseMelody (out, limits);
}

Melody layerNotesFollowing (const Melody& notesIn, Layer layer, const LayerFollowContext& context)
{
    if (layer == Layer::Melody)
        return melodyFollowingSlots (notesIn, context.before, context.after, context.sourceSlot);

    const auto limits = layerLimits (layer);
    const auto notes = sanitiseMelody (notesIn, limits);
    const auto& before = context.before;
    const auto& after = context.after;
    const auto& sourceSlot = context.sourceSlot;

    if (sourceSlot.size() != after.slots.size())
        return notes;

    const auto autoBefore = sanitiseMelody (context.autoBefore, limits);
    const auto autoAfter = sanitiseMelody (context.autoAfter, limits);
    const int numBefore = (int) before.slots.size();
    const double beforeTotal = (double) before.totalBeats();
    const double afterTotal = (double) after.totalBeats();
    const auto edited = editedSlots (notes, autoBefore, before);

    auto sourceOf = [&] (size_t i) { return sourceSlot[i] >= 0 && sourceSlot[i] < numBefore ? sourceSlot[i] : -1; };

    Melody out;

    // 1 + 2. Notes of edited slots move with their slot (and are remapped to the new chord); notes past the old loop
    // end keep their distance to it and are never remapped.
    for (const auto& n : notes.notes)
    {
        if (n.startBeat >= beforeTotal)
        {
            auto moved = n;
            moved.startBeat = n.startBeat - beforeTotal + afterTotal;
            out.notes.push_back (moved);
            continue;
        }

        const int slot = before.slotAtBeat (n.startBeat);

        if (slot < 0 || ! edited[(size_t) slot])
            continue;

        const double offset = n.startBeat - before.slotStartBeat (slot);
        const auto& fromChord = before.slots[(size_t) slot].chord;
        const int fromBassPc = bassPcOf (context.beforeBassPc, slot, before);

        for (size_t i = 0; i < sourceSlot.size(); ++i)
        {
            if (sourceOf (i) != slot || offset >= (double) after.slots[i].lengthBeats - kEpsilon)
                continue;

            auto moved = n;
            moved.startBeat = after.slotStartBeat ((int) i) + offset;

            const auto& toChord = after.slots[i].chord;
            const int toBassPc = bassPcOf (context.afterBassPc, (int) i, after);

            if (fromChord != toChord || fromBassPc != toBassPc)
                moved.note = remapPitchToChord (n.note, fromChord, fromBassPc, toChord, toBassPc, context.key, limits);

            out.notes.push_back (moved);
        }
    }

    // 0, 3, 3b. Untouched slots and new slots take the auto notes; a grown edited slot gets the auto notes of its tail.
    for (size_t i = 0; i < sourceSlot.size(); ++i)
    {
        const int src = sourceOf (i);
        const double from = after.slotStartBeat ((int) i);
        const double to = from + after.slots[i].lengthBeats;

        if (src < 0 || ! edited[(size_t) src])
        {
            for (const auto& n : startingIn (autoAfter.notes, from, to))
                out.notes.push_back (n);
        }
        else if (after.slots[i].lengthBeats > before.slots[(size_t) src].lengthBeats)
        {
            for (const auto& n : startingIn (autoAfter.notes, from + before.slots[(size_t) src].lengthBeats, to))
                out.notes.push_back (n);
        }
    }

    return sanitiseMelody (out, limits);
}
} // namespace cb
