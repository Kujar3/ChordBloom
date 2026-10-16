#include "core/Suggest.h"

// STUB - to be implemented (owner: core-theory agent)

namespace cb
{
float transitionScore (const Chord&, const Chord&, const Key&) { return 0.0f; }

std::vector<std::vector<float>> suggestionsFor (const Chord& from, const std::vector<PaletteSection>& palette, const Key& key)
{
    std::vector<std::vector<float>> out;
    for (auto& section : palette)
    {
        std::vector<float> row;
        for (auto& pad : section.chords)
            row.push_back (transitionScore (from, pad.chord, key));
        out.push_back (std::move (row));
    }
    return out;
}
} // namespace cb
