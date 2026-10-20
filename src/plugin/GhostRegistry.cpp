#include "plugin/GhostRegistry.h"

#include <algorithm>
#include <mutex>

namespace cb::ghosts
{
namespace
{
struct Registry
{
    std::mutex mutex;
    std::vector<Entry> entries;   // every registered instance, by instance number
    int lastNumber = 0;
    std::atomic<uint64_t> serial { 1 };
};

Registry& registry()
{
    static Registry r;   // function-local: safe to use from static plug-in objects
    return r;
}

Entry* find (Registry& r, int instanceNumber)
{
    for (auto& e : r.entries)
        if (e.instanceNumber == instanceNumber)
            return &e;

    return nullptr;
}
} // namespace

int registerInstance()
{
    auto& r = registry();
    const std::lock_guard<std::mutex> lock (r.mutex);
    Entry e;
    e.instanceNumber = ++r.lastNumber;
    r.entries.push_back (e);
    return e.instanceNumber;
}

void unregisterInstance (int instanceNumber)
{
    auto& r = registry();
    const std::lock_guard<std::mutex> lock (r.mutex);
    const auto before = r.entries.size();
    r.entries.erase (std::remove_if (r.entries.begin(), r.entries.end(),
                                     [instanceNumber] (const Entry& e) { return e.instanceNumber == instanceNumber; }),
                     r.entries.end());

    if (r.entries.size() != before)
        r.serial.fetch_add (1);
}

void publish (int instanceNumber, const Melody& melody, double loopBeats)
{
    auto& r = registry();
    const std::lock_guard<std::mutex> lock (r.mutex);
    auto* e = find (r, instanceNumber);

    if (e == nullptr || (e->melody == melody && e->loopBeats == loopBeats))
        return;

    e->melody = melody;
    e->loopBeats = loopBeats;
    r.serial.fetch_add (1);
}

std::vector<Entry> othersThan (int instanceNumber)
{
    auto& r = registry();
    const std::lock_guard<std::mutex> lock (r.mutex);
    std::vector<Entry> result;

    for (const auto& e : r.entries)
        if (e.instanceNumber != instanceNumber && ! e.melody.empty())
            result.push_back (e);

    return result;   // entries are kept in registration order = instance number order
}

uint64_t serial() noexcept
{
    return registry().serial.load();
}
} // namespace cb::ghosts
