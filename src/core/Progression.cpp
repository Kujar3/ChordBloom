#include "core/Progression.h"

namespace cb
{
int Progression::totalBeats() const
{
    int total = 0;
    for (auto& s : slots)
        total += s.lengthBeats;
    return total;
}

double Progression::slotStartBeat (int index) const
{
    double beat = 0.0;
    for (int i = 0; i < index && i < (int) slots.size(); ++i)
        beat += slots[(size_t) i].lengthBeats;
    return beat;
}

int Progression::slotAtBeat (double beat) const
{
    if (beat < 0.0)
        return -1;

    double start = 0.0;
    for (int i = 0; i < (int) slots.size(); ++i)
    {
        const double end = start + slots[(size_t) i].lengthBeats;
        if (beat < end)
            return i;
        start = end;
    }
    return -1;
}
} // namespace cb
