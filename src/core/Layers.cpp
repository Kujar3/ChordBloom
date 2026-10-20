#include "core/Layers.h"

#include <algorithm>
#include <cctype>

// Layers (final-spec 4.4). Identity / channel / limit functions are WP0's data (frozen behaviour); everything marked
// "WP0 STUB" is WP3's to implement.

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

// WP0 STUB: no auto notes yet (the processor stub shows no auto layer content).
Melody notesFromEvents (const std::vector<NoteEvent>&, const NoteLimits&)
{
    return {};
}

// WP0 STUB (already the specified behaviour; WP3 owns it).
bool sameStructure (const Progression& a, const Progression& b)
{
    if (a.slots.size() != b.slots.size())
        return false;
    for (size_t i = 0; i < a.slots.size(); ++i)
        if (a.slots[i].lengthBeats != b.slots[i].lengthBeats)
            return false;
    return true;
}

// WP0 STUB: the note unchanged, clamped to the limits.
int remapPitchToChord (int note, const Chord&, int, const Chord&, int, const Key&, const NoteLimits& limits)
{
    const int low = std::clamp (limits.lowest, 0, 127);
    const int high = std::max (low, std::clamp (limits.highest, 0, 127));
    return std::clamp (note, low, high);
}

// WP0 STUB: the notes sanitised for the layer, unmoved.
Melody layerNotesFollowing (const Melody& notes, Layer layer, const LayerFollowContext&)
{
    return sanitiseMelody (notes, layerLimits (layer));
}

// WP0 STUB: nothing edited.
std::vector<bool> editedSlots (const Melody&, const Melody&, const Progression& progression)
{
    return std::vector<bool> (progression.slots.size(), false);
}

// WP0 STUB: `custom` sanitised.
Melody replaceSlotNotes (const Melody& custom, const Melody&, const Progression&, int, const NoteLimits& limits)
{
    return sanitiseMelody (custom, limits);
}
} // namespace cb
