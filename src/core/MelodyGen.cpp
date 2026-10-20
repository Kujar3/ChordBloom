#include "core/MelodyGen.h"
#include "core/MelodyGenTables.h"

#include <algorithm>

// Melody generator (final-spec 4.6 / 5.4). Names, the mood table and MelodyGenSettings::operator== are WP0's
// (frozen behaviour); everything marked "WP0 STUB" is WP1's to implement.

namespace cb
{
const char* melodyStyleName (MelodyStyle s)
{
    static const char* names[] = { "Uplifting lead", "Lyrical hook", "Driving riff", "Sparse" };
    static_assert (sizeof (names) / sizeof (names[0]) == (size_t) numMelodyStyles, "one name per MelodyStyle");
    return names[std::clamp ((int) s, 0, numMelodyStyles - 1)];
}

const char* melodyStyleShortName (MelodyStyle s)
{
    static const char* names[] = { "Uplifting", "Lyrical", "Driving", "Sparse" };
    static_assert (sizeof (names) / sizeof (names[0]) == (size_t) numMelodyStyles, "one name per MelodyStyle");
    return names[std::clamp ((int) s, 0, numMelodyStyles - 1)];
}

const char* melodyStyleDescription (MelodyStyle s)
{
    static const char* lines[] = {
        "Anthemic lead: chord tones on the beat, rising phrases, a big ending",
        "Singable hook: mostly steps, held notes, question-and-answer phrases",
        "Busy riff: short repeated cells and pedal notes in tight 16ths",
        "A few long notes that leave room for the chords",
    };
    static_assert (sizeof (lines) / sizeof (lines[0]) == (size_t) numMelodyStyles, "one line per MelodyStyle");
    return lines[std::clamp ((int) s, 0, numMelodyStyles - 1)];
}

MelodyStyle melodyStyleForMood (Mood mood)
{
    switch (mood)
    {
        case Mood::Uplifting: return MelodyStyle::Uplifting;
        case Mood::Emotional: return MelodyStyle::Lyrical;
        case Mood::Dark:      return MelodyStyle::Driving;
        case Mood::Dreamy:    return MelodyStyle::Sparse;
        case Mood::Epic:      return MelodyStyle::Uplifting;
        case Mood::LoFi:      return MelodyStyle::Lyrical;
        case Mood::NumMoods:
        default:              break;
    }
    return MelodyStyle::Lyrical;
}

const char* melodyContourName (MelodyContour c)
{
    static const char* names[] = { "Auto", "Rising", "Falling", "Arch", "Wave" };
    static_assert (sizeof (names) / sizeof (names[0]) == (size_t) numMelodyContours, "one name per MelodyContour");
    return names[std::clamp ((int) c, 0, numMelodyContours - 1)];
}

bool MelodyGenSettings::operator== (const MelodyGenSettings& o) const
{
    return style == o.style && density == o.density && repetition == o.repetition && registerShift == o.registerShift
        && contour == o.contour && resolveEnding == o.resolveEnding;
}

// WP0 STUB: no melody yet.
Melody generateMelody (const Progression&, const Key&, const MelodyGenContext&, std::uint32_t, const MelodyGenSettings&)
{
    return {};
}

// WP0 STUB
std::pair<int, int> melodyRegisterWindow (const Progression&, const MelodyGenContext&, const MelodyGenSettings&)
{
    return { 60, 84 };
}

// WP0 STUB
std::string melodyPlanSignature (const Progression&, const Key&, std::uint32_t, const MelodyGenSettings&)
{
    return {};
}

// WP0 STUB: all zeros.
MelodyStats analyseMelody (const Melody&, const Progression&, const Key&, const std::vector<NoteEvent>&)
{
    return {};
}
} // namespace cb
