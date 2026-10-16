#include "core/Generate.h"

// STUB - to be implemented (owner: core-suggest agent)

namespace cb
{
const char* moodName (Mood m)
{
    static const char* names[] = { "Uplifting", "Emotional", "Dark", "Dreamy", "Epic", "Lo-fi" };
    const int i = (int) m;
    return names[i < 0 || i >= numMoods ? 0 : i];
}

std::vector<Preset> presetsFor (const Key&) { return {}; }

Progression sparkProgression (const Key&, const std::vector<PaletteSection>& palette, std::uint32_t, const SparkSettings& s)
{
    Progression p;
    if (! palette.empty() && ! palette.front().chords.empty())
    {
        ProgressionSlot slot;
        slot.chord = palette.front().chords.front().chord;
        slot.lengthBeats = s.beatsPerChord;
        p.slots.push_back (slot);
    }
    return p;
}
} // namespace cb
