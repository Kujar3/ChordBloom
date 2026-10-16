#pragma once

// The chord progression model shared by the processor, the UI and the renderers. Pure C++17, no JUCE.

#include "Theory.h"

namespace cb
{

struct ProgressionSlot
{
    Chord chord;
    int lengthBeats = 4;   // quarter-note beats, 1..16
    int inversion = -1;    // -1 = auto (voice leading decides), otherwise 0..numTones-1
    int octaveShift = 0;   // -2..+2 octaves relative to the global octave

    bool operator== (const ProgressionSlot& o) const
    {
        return chord == o.chord && lengthBeats == o.lengthBeats && inversion == o.inversion && octaveShift == o.octaveShift;
    }
    bool operator!= (const ProgressionSlot& o) const { return ! (*this == o); }
};

constexpr int kMaxSlots = 16;
constexpr int kMinSlotBeats = 1;
constexpr int kMaxSlotBeats = 16;

struct Progression
{
    std::vector<ProgressionSlot> slots;   // at most kMaxSlots

    bool empty() const { return slots.empty(); }
    int totalBeats() const;                    // sum of lengthBeats
    double slotStartBeat (int index) const;    // beat at which slot `index` starts (index == size() -> totalBeats())
    int slotAtBeat (double beat) const;        // slot index containing beat in [0, totalBeats), -1 if outside/empty

    bool operator== (const Progression& o) const { return slots == o.slots; }
    bool operator!= (const Progression& o) const { return ! (*this == o); }
};

} // namespace cb
