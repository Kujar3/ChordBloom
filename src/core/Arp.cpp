#include "core/Arp.h"

#include "core/Melody.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>

// Arp layer (final-spec 4.3): the name / table / mask functions, then renderArp.

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
        case ArpRhythm::KickGap:
        {
            // Rests on the steps that start a beat (where the kick is), at every rate: steps 0, 4, 8, 12 at 1/16, the
            // 8ths on the beat at 1/8, the triplets 0, 3, 6, 9 at 1/8T. At 1/4 every step starts a beat, so the
            // 16th-group mask (rest on the bar's first step only) stays.
            const int stepsPerBeat = stepsPerBar > 0 && stepsPerBar % 4 == 0 ? stepsPerBar / 4 : 4;
            return stepsPerBeat > 1 ? stepInBar % stepsPerBeat != 0 : inGroup != 0;
        }
        case ArpRhythm::Skip2nd:       return inGroup != 1;
        case ArpRhythm::Skip3rd:       return inGroup != 2;
        case ArpRhythm::Breath:        return stepsPerBar <= 0 || stepInBar != stepsPerBar - 1;
        case ArpRhythm::LateStart:     return stepInBar != 0;
        case ArpRhythm::NumRhythms:
        default:                       break;
    }
    return true;
}


namespace
{
constexpr double kEps = 1.0e-9;
constexpr double kMinNoteLength = 1.0e-4;
constexpr double kHumanizeTiming = 0.015;
constexpr float kMinVelocity = 1.0f / 127.0f;
constexpr int kMinCentre = 40;
constexpr int kMaxCentre = 96;
constexpr int kVoiceLedReach = 6;       // a base this close to the previous final base is "voice-led"
constexpr int kVoiceLedMeanReach = 7;   // ... when the cycle mean stays this close to the centre
constexpr int kClearance = 12;          // the arp stays an octave above the lowest bass note of the slot

uint32_t mix32 (uint32_t h)
{
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

// Deterministic value in [-1, 1]: hash of slot, step and channel.
double jitter (int slot, int step, int channel)
{
    uint32_t h = 0x9e3779b9u;
    h = mix32 (h ^ ((uint32_t) slot * 0x27d4eb2du));
    h = mix32 (h ^ ((uint32_t) step * 0x165667b1u));
    h = mix32 (h ^ ((uint32_t) channel * 0xfd7046c5u));
    h = mix32 (h ^ 0x68e31da4u);
    return (double) h / 4294967295.0 * 2.0 - 1.0;
}

// The slot's tone stack, relative to the chord root: the first min (4, n) intervals, rotated for an explicit
// inversion so that tone 0 is the core tone in the bass (every following tone raised above the one before).
std::vector<int> toneStack (const ProgressionSlot& slot, const std::vector<int>& voicing)
{
    const auto& intervals = qualityIntervals (slot.chord.quality);
    const size_t n = std::min<size_t> (4, intervals.size());
    std::vector<int> core (intervals.begin(), intervals.begin() + (std::ptrdiff_t) n);

    if (slot.inversion >= 0)
    {
        const int bassPc = slotBassPitchClass (slot, voicing);
        for (size_t r = 0; r < n; ++r)
            if (pc (slot.chord.root + core[r]) == bassPc)
            {
                std::vector<int> rotated;
                rotated.push_back (core[r] % 12);
                for (size_t j = 1; j < n; ++j)
                {
                    int v = core[(r + j) % n];
                    while (v <= rotated.back())
                        v += 12;
                    rotated.push_back (v);
                }
                return rotated;
            }
    }

    return core;
}

// Tone `tone` of the stack; tones past its size continue an octave higher.
int stackTone (const std::vector<int>& stack, int tone)
{
    const int size = (int) stack.size();
    int octaves = 0;
    while (tone >= size)
    {
        tone -= size;
        ++octaves;
    }
    return stack[(size_t) tone] + 12 * octaves;
}

struct SlotCycle
{
    std::vector<int> rel;   // the cycle's tones relative to the pitch of tone 0
    long relSum = 0;
    int relMin = 0;
    int basePc = 0;         // pitch class of tone 0
};

SlotCycle makeCycle (const ProgressionSlot& slot, const std::vector<int>& voicing, ArpShape shape)
{
    const auto stack = toneStack (slot, voicing);

    SlotCycle c;
    c.basePc = pc (slot.chord.root + stack.front());
    c.relMin = INT_MAX;
    for (const auto& t : arpShapeTones (shape))
    {
        const int v = stackTone (stack, t.tone) - stack.front() + 12 * t.octave;
        c.rel.push_back (v);
        c.relSum += v;
        c.relMin = std::min (c.relMin, v);
    }
    return c;
}

// The base (pitch of tone 0) of a slot: mean pitch of the cycle nearest the centre (ties lower); a base within
// kVoiceLedReach of the previous slot's final base wins when its mean stays within kVoiceLedMeanReach of the centre.
int chooseBase (const SlotCycle& c, int centre, bool havePrevious, int previous)
{
    const long len = (long) c.rel.size();
    auto meanOffset = [&] (int base) { return std::labs (len * (long) base + c.relSum - (long) centre * len); };

    int nearest = INT_MIN;
    int voiceLed = INT_MIN;
    for (int base = -24; base <= 150; ++base)
    {
        if (pc (base) != c.basePc)
            continue;

        if (nearest == INT_MIN || meanOffset (base) < meanOffset (nearest))
            nearest = base;

        if (havePrevious && std::abs (base - previous) <= kVoiceLedReach
            && meanOffset (base) <= (long) kVoiceLedMeanReach * len
            && (voiceLed == INT_MIN || std::abs (base - previous) < std::abs (voiceLed - previous)))
            voiceLed = base;
    }
    return voiceLed != INT_MIN ? voiceLed : nearest;
}

int foldIntoRange (int note)
{
    while (note < kArpLowestNote)
        note += 12;
    while (note > kArpHighestNote)
        note -= 12;
    return note;
}
} // namespace

std::vector<NoteEvent> renderArp (const Progression& progression, const std::vector<std::vector<int>>& voicings,
                                  const ArpSettings& settings, const RenderSettings& feel,
                                  const std::vector<NoteEvent>& bassEvents, int channel)
{
    std::vector<NoteEvent> events;
    if (progression.slots.empty())
        return events;

    const auto& cycleTones = arpShapeTones (settings.shape);
    const bool freeRuns = arpShapeFreeRuns (settings.shape);
    const int stepsPerBeat = std::max (1, (int) std::lround (1.0 / rateInBeats (settings.rate)));
    const int stepsPerBar = 4 * stepsPerBeat;
    const double step = 1.0 / (double) stepsPerBeat;
    const int centre = std::clamp (settings.centreNote, kMinCentre, kMaxCentre);
    const double gate = std::clamp ((double) settings.gate, 0.05, 1.0);
    const double swing = std::clamp ((double) feel.swing, 0.0, 1.0);
    const double humanize = std::clamp ((double) feel.humanize, 0.0, 1.0);
    const float velocity = std::clamp (feel.velocity, kMinVelocity, 1.0f);
    const double grid = settings.rate == RateDivision::EighthTriplet ? 0.0 : grooveGridBeats (feel);

    int slotStartBeat = 0;
    int cyclePosition = 0;   // sounding steps since the loop start (free-running) or the last bar line
    bool havePrevious = false;
    int previousBase = 0;

    for (int i = 0; i < (int) progression.slots.size(); ++i)
    {
        const auto& slot = progression.slots[(size_t) i];
        if (slot.lengthBeats <= 0)
            continue;

        const int slotFirst = slotStartBeat;
        const int slotLast = slotStartBeat + slot.lengthBeats;
        slotStartBeat = slotLast;

        const std::vector<int> noVoicing;
        const auto& voicing = i < (int) voicings.size() ? voicings[(size_t) i] : noVoicing;
        const auto cycle = makeCycle (slot, voicing, settings.shape);
        const long len = (long) cycle.rel.size();

        int base = chooseBase (cycle, centre, havePrevious, previousBase);

        // Bass clearance: stay an octave above the lowest bass note that starts in this slot.
        int lowestBass = INT_MAX;
        for (const auto& b : bassEvents)
            if (b.startBeat >= (double) slotFirst - kEps && b.startBeat < (double) slotLast - kEps)
                lowestBass = std::min (lowestBass, b.note);

        if (lowestBass != INT_MAX)
            while (base + cycle.relMin < lowestBass + kClearance
                   && len * (long) (base + 12) + cycle.relSum <= (long) (centre + 12) * len)
                base += 12;

        havePrevious = true;
        previousBase = base;

        for (int k = slotFirst * stepsPerBeat; k < slotLast * stepsPerBeat; ++k)
        {
            const int stepInBar = k % stepsPerBar;
            if (! freeRuns && stepInBar == 0)
                cyclePosition = 0;

            if (! arpStepSounds (settings.rhythm, stepInBar, stepsPerBar))
                continue;

            const int rel = cycle.rel[(size_t) (cyclePosition % (int) cycleTones.size())];
            ++cyclePosition;

            const double straightStart = (double) k / (double) stepsPerBeat;
            const double warpedStart = swingWarp (straightStart, grid, swing);
            const double warpedEnd = swingWarp (straightStart + gate * step, grid, swing);
            const double length = std::max (warpedEnd - warpedStart, kMinMelodyNoteBeats);

            double start = warpedStart;
            if (humanize > 0.0)
                start += jitter (i, k, channel) * kHumanizeTiming * humanize;
            start = std::max (start, (double) slotFirst);

            const double end = std::min (start + length, (double) slotLast);
            if (end - start < kMinNoteLength)
                continue;

            NoteEvent e;
            e.note = std::clamp (foldIntoRange (base + rel), 0, 127);
            e.velocity = velocity;
            e.startBeat = start;
            e.lengthBeats = end - start;
            e.channel = channel;
            events.push_back (e);
        }
    }

    // A note ends at or before the next start of the same pitch.
    std::sort (events.begin(), events.end(), [] (const NoteEvent& a, const NoteEvent& b)
    {
        if (a.note != b.note)
            return a.note < b.note;
        return a.startBeat < b.startBeat;
    });

    std::vector<NoteEvent> kept;
    kept.reserve (events.size());
    for (size_t i = 0; i < events.size(); ++i)
    {
        auto e = events[i];
        if (i + 1 < events.size() && events[i + 1].note == e.note && e.startBeat + e.lengthBeats > events[i + 1].startBeat)
            e.lengthBeats = events[i + 1].startBeat - e.startBeat;

        if (e.lengthBeats >= kMinNoteLength)
            kept.push_back (e);
    }

    std::sort (kept.begin(), kept.end(), [] (const NoteEvent& a, const NoteEvent& b)
    {
        if (a.startBeat != b.startBeat)
            return a.startBeat < b.startBeat;
        return a.note < b.note;
    });

    return kept;
}
} // namespace cb
