#pragma once

// Chord voicing: turning a Chord into concrete MIDI notes, with inversions and voice leading. Pure C++17, no JUCE.

#include "Progression.h"

namespace cb
{

enum class VoicingStyle
{
    Close = 0,  // all tones packed within one octave above the lowest note
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
    bool voiceLeading = true;   // with inversion == -1 and a previous voicing: choose the candidate with least total movement
};

// Voices one chord.
//  inversion: -1 = auto, 0 = root position, 1 = first inversion (3rd in bass), ... clamped to numTones-1.
//  octaveShift: added as 12 * octaveShift after placement.
//  previous: the previous chord's voicing (may be empty) for voice leading when inversion == -1.
// With inversion == -1 and no previous voicing (or voiceLeading off): root position placed so the voicing's mean is
// closest to settings.centerNote. Explicit inversions are also placed nearest to centerNote.
// Voice leading candidates: every inversion of the chosen style at every octave placement whose mean lies within
// +-7 semitones of centerNote; pick the least sum of |movement| (pair notes by sorted order, extra notes vs nearest).
// Result: ascending, unique, every note in [21, 108].
std::vector<int> voiceChord (const Chord&, const VoicingSettings&, int inversion, int octaveShift,
                             const std::vector<int>& previous);

// Voices every slot in order, feeding each result as `previous` to the next (wrapping is not considered).
std::vector<std::vector<int>> voiceProgression (const Progression&, const VoicingSettings&);

} // namespace cb
