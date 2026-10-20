#include "core/Arp.h"

#include <algorithm>

// Arp layer (final-spec 4.3). The name / table / mask functions are WP0's data (frozen behaviour); renderArp is WP2's.

namespace cb
{
const char* arpShapeName (ArpShape s)
{
    static const char* names[] = { "Up 3", "Down 3", "Up 4", "Down 4", "Up-down", "1-3-5-7", "Two octaves",
                                   "Broken 1-5-3-5", "Root pulse", "Octave bounce", "Root + high 3rd",
                                   "Root + high 5th", "Pedal 1-3'-1-1'", "Pedal 1-1-3'-1", "Pedal 1-1-5'-1",
                                   "Alternate 1-3", "Alternate 1-5" };
    static_assert (sizeof (names) / sizeof (names[0]) == (size_t) numArpShapes, "one name per ArpShape");
    return names[std::clamp ((int) s, 0, numArpShapes - 1)];
}

const char* arpShapeFamily (ArpShape s)
{
    const int i = std::clamp ((int) s, 0, numArpShapes - 1);
    if (i <= (int) ArpShape::Broken)
        return "Ladders";
    if (i <= (int) ArpShape::PedalLow5)
        return "Pedal & bounce";
    return "Two-note";
}

const std::vector<ArpTone>& arpShapeTones (ArpShape s)
{
    using T = ArpTone;
    static const std::vector<std::vector<ArpTone>> table {
        { T { 0, 0 }, T { 1, 0 }, T { 2, 0 } },                                       // Up3
        { T { 0, 0 }, T { 2, 0 }, T { 1, 0 } },                                       // Down3
        { T { 0, 0 }, T { 1, 0 }, T { 2, 0 }, T { 0, 1 } },                           // Up4
        { T { 0, 1 }, T { 2, 0 }, T { 1, 0 }, T { 0, 0 } },                           // Down4
        { T { 0, 0 }, T { 1, 0 }, T { 2, 0 }, T { 1, 0 } },                           // UpDown
        { T { 0, 0 }, T { 1, 0 }, T { 2, 0 }, T { 3, 0 } },                           // Climb7
        { T { 0, 0 }, T { 1, 0 }, T { 2, 0 }, T { 0, 1 }, T { 1, 1 }, T { 2, 1 } },   // TwoOctaves
        { T { 0, 0 }, T { 2, 0 }, T { 1, 0 }, T { 2, 0 } },                           // Broken
        { T { 0, 0 } },                                                               // RootPulse
        { T { 0, 0 }, T { 0, 1 } },                                                   // OctaveBounce
        { T { 0, 0 }, T { 1, 1 } },                                                   // RootHigh3
        { T { 0, 0 }, T { 2, 1 } },                                                   // RootHigh5
        { T { 0, 0 }, T { 1, 1 }, T { 0, 0 }, T { 0, 1 } },                           // PedalOctave
        { T { 0, 0 }, T { 0, 0 }, T { 1, 1 }, T { 0, 0 } },                           // PedalLow3
        { T { 0, 0 }, T { 0, 0 }, T { 2, 1 }, T { 0, 0 } },                           // PedalLow5
        { T { 0, 0 }, T { 1, 0 } },                                                   // Alternate13
        { T { 0, 0 }, T { 2, 0 } },                                                   // Alternate15
    };
    return table[(size_t) std::clamp ((int) s, 0, numArpShapes - 1)];
}

bool arpShapeFreeRuns (ArpShape s)
{
    return s == ArpShape::Up3 || s == ArpShape::Down3;
}

const char* arpRhythmName (ArpRhythm r)
{
    static const char* names[] = { "Every step", "3 on, 1 off", "Kick gap", "Skip 2nd", "Skip 3rd", "Breath",
                                   "Late start" };
    static_assert (sizeof (names) / sizeof (names[0]) == (size_t) numArpRhythms, "one name per ArpRhythm");
    return names[std::clamp ((int) r, 0, numArpRhythms - 1)];
}

bool arpStepSounds (ArpRhythm r, int stepInBar, int stepsPerBar)
{
    if (stepsPerBar > 0)
        stepInBar = ((stepInBar % stepsPerBar) + stepsPerBar) % stepsPerBar;
    const int inGroup = ((stepInBar % 4) + 4) % 4;

    switch (r)
    {
        case ArpRhythm::EveryStep:     return true;
        case ArpRhythm::ThreeOnOneOff: return inGroup != 3;
        case ArpRhythm::KickGap:       return inGroup != 0;
        case ArpRhythm::Skip2nd:       return inGroup != 1;
        case ArpRhythm::Skip3rd:       return inGroup != 2;
        case ArpRhythm::Breath:        return stepsPerBar <= 0 || stepInBar != stepsPerBar - 1;
        case ArpRhythm::LateStart:     return stepInBar != 0;
        case ArpRhythm::NumRhythms:
        default:                       break;
    }
    return true;
}

// WP0 STUB (WP2 implements it): the arp renders nothing yet, so the plugin behaves exactly as in phase 1.5.
std::vector<NoteEvent> renderArp (const Progression&, const std::vector<std::vector<int>>&, const ArpSettings&,
                                  const RenderSettings&, const std::vector<NoteEvent>&, int)
{
    return {};
}
} // namespace cb
