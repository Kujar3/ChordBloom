#include "core/Palette.h"

// STUB - to be implemented (owner: core-theory agent)

namespace cb
{
std::vector<PaletteSection> buildPalette (const Key&)
{
    return {
        { PaletteSectionId::Triads, "Triads", {} },
        { PaletteSectionId::Sevenths, "Sevenths", {} },
        { PaletteSectionId::Color, "Color", {} },
        { PaletteSectionId::Borrowed, "Borrowed", {} },
        { PaletteSectionId::Secondary, "Secondary", {} },
    };
}

Chord diatonicChord (const Key&, int, bool) { return {}; }
} // namespace cb
