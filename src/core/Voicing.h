#pragma once

// Chord voicing: turning a Chord into concrete MIDI notes, with inversions and voice leading. Pure C++17, no JUCE.

#include "Progression.h"

namespace cb
{

enum class VoicingStyle
{
    Close = 0,  // core packed within an octave, extensions above the core
    Open,       // drop-2: second-highest voice of the close voicing dropped an octave
    Spread,     // root low, remaining tones spread upward over roughly two octaves (wide, pad-like)
    NumStyles
};

constexpr int numVoicingStyles = (int) VoicingStyle::NumStyles;
const char* voicingStyleName (VoicingStyle);   // "Close", "Open", "Spread"

struct VoicingSettings
{
    VoicingStyle style = VoicingStyle::Close;
    int centerNote = 60;        // MIDI note the voicing should be centred around (global octave moves this by 12)
    bool voiceLeading = true;   // with inversion == -1 and a previous voicing: balance movement with bass clarity
};

// Voices one chord.
//  inversion: -1 = auto, 0 = root position, 1 = first inversion (3rd in bass), ... clamped to numTones-1.
//  octaveShift: added as 12 * octaveShift after placement.
//  previous: the previous chord's voicing (may be empty) for voice leading when inversion == -1.
// Placement uses the core-tone mean for Close, the full mean for Open/Spread. Extensions
// (all intervals >= 12) stay above the core in interval order, unless explicitly requested
// in the bass. All chord tones are retained, including the fifth of six-note chords.
// Auto voice leading considers core inversions within +-7 semitones of centerNote,
// minimising movement plus 4 for a sixth/seventh bass and 8 for a bottom interval <=2.
// Explicit inversions bypass these penalties. Range limits take priority over octaveShift.
// Result: ascending, unique, every note in [21, 108].
std::vector<int> voiceChord (const Chord&, const VoicingSettings&, int inversion, int octaveShift,
                             const std::vector<int>& previous);

// Voices every slot in order, feeding each result as `previous` to the next (wrapping is not considered).
std::vector<std::vector<int>> voiceProgression (const Progression&, const VoicingSettings&);

} // namespace cb
