#include "core/Pattern.h"

#include <algorithm>

namespace cb
{
const char* rhythmPatternName (RhythmPattern p)
{
    static const char* names[] = { "Block", "Strum", "Pulse", "Offbeat", "Arp Up", "Arp Down", "Arp Up/Down", "Broken" };
    return names[std::clamp ((int) p, 0, numRhythmPatterns - 1)];
}

const char* bassModeName (BassMode m)
{
    static const char* names[] = { "Off", "Root", "Root + 5th", "Octaves", "Pulse" };
    return names[std::clamp ((int) m, 0, numBassModes - 1)];
}

const char* rateDivisionName (RateDivision r)
{
    static const char* names[] = { "1/4", "1/8", "1/16", "1/8T" };
    return names[std::clamp ((int) r, 0, numRateDivisions - 1)];
}

double rateInBeats (RateDivision r)
{
    switch (r)
    {
        case RateDivision::Quarter:       return 1.0;
        case RateDivision::Eighth:        return 0.5;
        case RateDivision::Sixteenth:     return 0.25;
        case RateDivision::EighthTriplet: return 1.0 / 3.0;
        case RateDivision::NumRates:      break;
    }
    return 0.5;
}

// TODO (core-voicing agent): implement per the contract in Pattern.h
std::vector<NoteEvent> renderProgression (const Progression&, const std::vector<std::vector<int>>&, const RenderSettings&)
{
    return {};
}
} // namespace cb
