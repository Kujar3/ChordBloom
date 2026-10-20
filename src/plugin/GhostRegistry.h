#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "core/Melody.h"

// Process-wide registry of every Chordbloom instance's melody (ghost notes). Plain C++, guarded by a mutex; called from
// the message thread only (never from the audio thread). Hosts that load several Chordbloom instances in one process
// (FL Studio without bridging) let each instance see the others' melodies.
namespace cb::ghosts
{
struct Entry
{
    int instanceNumber = 0;   // 1, 2, 3 ... in registration order (numbers are never reused within the process)
    Melody melody;
    double loopBeats = 0.0;   // that instance's progression length
};

int registerInstance();                       // returns the new instance's number
void unregisterInstance (int instanceNumber);
void publish (int instanceNumber, const Melody&, double loopBeats);   // replaces that instance's entry

// The entries of every other registered instance whose melody has notes, sorted by instance number.
std::vector<Entry> othersThan (int instanceNumber);

// Changes (any thread can read it) whenever an entry is published with different content or an instance goes away.
uint64_t serial() noexcept;
} // namespace cb::ghosts
