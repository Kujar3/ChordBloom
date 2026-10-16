#include "core/Voicing.h"

// STUB - to be implemented (owner: core-voicing agent)

namespace cb
{
const char* voicingStyleName (VoicingStyle s)
{
    static const char* names[] = { "Close", "Open", "Spread" };
    const int i = (int) s;
    return names[i < 0 || i >= numVoicingStyles ? 0 : i];
}

std::vector<int> voiceChord (const Chord& chord, const VoicingSettings& settings, int, int octaveShift, const std::vector<int>&)
{
    std::vector<int> notes;
    for (int i : chord.intervals())
        notes.push_back (settings.centerNote - 6 + pc (chord.root) + i + 12 * octaveShift);
    return notes;
}

std::vector<std::vector<int>> voiceProgression (const Progression& progression, const VoicingSettings& settings)
{
    std::vector<std::vector<int>> out;
    std::vector<int> previous;
    for (auto& slot : progression.slots)
    {
        previous = voiceChord (slot.chord, settings, slot.inversion, slot.octaveShift, previous);
        out.push_back (previous);
    }
    return out;
}
} // namespace cb
