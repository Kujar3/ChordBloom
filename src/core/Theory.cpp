#include "core/Theory.h"

#include <algorithm>

namespace cb
{
//==============================================================================
std::string pitchClassName (int pitchClass, bool preferFlats)
{
    static const char* sharps[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    static const char* flats[]  = { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" };
    return preferFlats ? flats[pc (pitchClass)] : sharps[pc (pitchClass)];
}

//==============================================================================
const ScaleInfo& scaleInfo (ScaleType type)
{
    static const ScaleInfo scales[] = {
        { "Major",          { 0, 2, 4, 5, 7, 9, 11 }, false },
        { "Natural Minor",  { 0, 2, 3, 5, 7, 8, 10 }, true },
        { "Harmonic Minor", { 0, 2, 3, 5, 7, 8, 11 }, true },
        { "Melodic Minor",  { 0, 2, 3, 5, 7, 9, 11 }, true },
        { "Dorian",         { 0, 2, 3, 5, 7, 9, 10 }, true },
        { "Phrygian",       { 0, 1, 3, 5, 7, 8, 10 }, true },
        { "Lydian",         { 0, 2, 4, 6, 7, 9, 11 }, false },
        { "Mixolydian",     { 0, 2, 4, 5, 7, 9, 10 }, false },
        { "Locrian",        { 0, 1, 3, 5, 6, 8, 10 }, true },
    };

    const int index = std::clamp ((int) type, 0, numScaleTypes - 1);
    return scales[index];
}

//==============================================================================
std::array<int, 7> Key::pitchClasses() const
{
    std::array<int, 7> result {};
    const auto& info = scaleInfo (scale);
    for (size_t i = 0; i < 7; ++i)
        result[i] = pc (tonic + info.offsets[i]);
    return result;
}

int Key::degreeOf (int pitchClass) const
{
    const auto pcs = pitchClasses();
    for (int i = 0; i < 7; ++i)
        if (pcs[(size_t) i] == pc (pitchClass))
            return i;
    return -1;
}

bool Key::contains (int pitchClass) const { return degreeOf (pitchClass) >= 0; }

Key Key::parallel() const
{
    return { tonic, scaleInfo (scale).isMinorFamily ? ScaleType::Major : ScaleType::NaturalMinor };
}

std::string Key::tonicName() const
{
    static const char* majorNames[] = { "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    static const char* minorNames[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "G#", "A", "Bb", "B" };
    return scaleInfo (scale).isMinorFamily ? minorNames[pc (tonic)] : majorNames[pc (tonic)];
}

bool Key::prefersFlats() const
{
    const auto t = tonicName();
    if (t.find ('b') != std::string::npos)
        return true;

    const int p = pc (tonic);
    if (scaleInfo (scale).isMinorFamily)
        return p == 0 || p == 2 || p == 5 || p == 7 || p == 10 || p == 3;   // C, D, F, G, Bb, Eb minor

    return p == 0 || p == 5;   // C, F major
}

std::string Key::spell (int pitchClass) const
{
    const int p = pc (pitchClass);
    const int degree = degreeOf (p);
    if (degree < 0)
        return pitchClassName (p, prefersFlats());

    static const char letters[] = { 'C', 'D', 'E', 'F', 'G', 'A', 'B' };
    static const int letterPcs[] = { 0, 2, 4, 5, 7, 9, 11 };

    const auto t = tonicName();
    int tonicLetter = 0;
    for (int i = 0; i < 7; ++i)
        if (letters[i] == t[0])
            tonicLetter = i;

    const int letterIndex = (tonicLetter + degree) % 7;
    int diff = p - letterPcs[letterIndex];
    while (diff > 6)  diff -= 12;
    while (diff < -6) diff += 12;

    if (diff == 0)  return std::string (1, letters[letterIndex]);
    if (diff == 1)  return std::string (1, letters[letterIndex]) + "#";
    if (diff == -1) return std::string (1, letters[letterIndex]) + "b";
    return pitchClassName (p, prefersFlats());
}

std::string Key::name() const { return tonicName() + " " + scaleInfo (scale).name; }

//==============================================================================
const char* qualitySuffix (ChordQuality q)
{
    switch (q)
    {
        case ChordQuality::Major:           return "";
        case ChordQuality::Minor:           return "m";
        case ChordQuality::Diminished:      return "dim";
        case ChordQuality::Augmented:       return "aug";
        case ChordQuality::Sus2:            return "sus2";
        case ChordQuality::Sus4:            return "sus4";
        case ChordQuality::Major6:          return "6";
        case ChordQuality::Minor6:          return "m6";
        case ChordQuality::Add9:            return "add9";
        case ChordQuality::MinorAdd9:       return "m(add9)";
        case ChordQuality::Major7:          return "maj7";
        case ChordQuality::Minor7:          return "m7";
        case ChordQuality::Dominant7:       return "7";
        case ChordQuality::HalfDiminished7: return "m7b5";
        case ChordQuality::Diminished7:     return "dim7";
        case ChordQuality::MinorMajor7:     return "m(maj7)";
        case ChordQuality::Dominant7Sus4:   return "7sus4";
        case ChordQuality::Major9:          return "maj9";
        case ChordQuality::Minor9:          return "m9";
        case ChordQuality::Dominant9:       return "9";
        case ChordQuality::NumQualities:    break;
    }
    return "";
}

const std::vector<int>& qualityIntervals (ChordQuality q)
{
    static const std::vector<int> table[] = {
        { 0, 4, 7 },          // Major
        { 0, 3, 7 },          // Minor
        { 0, 3, 6 },          // Diminished
        { 0, 4, 8 },          // Augmented
        { 0, 2, 7 },          // Sus2
        { 0, 5, 7 },          // Sus4
        { 0, 4, 7, 9 },       // Major6
        { 0, 3, 7, 9 },       // Minor6
        { 0, 4, 7, 14 },      // Add9
        { 0, 3, 7, 14 },      // MinorAdd9
        { 0, 4, 7, 11 },      // Major7
        { 0, 3, 7, 10 },      // Minor7
        { 0, 4, 7, 10 },      // Dominant7
        { 0, 3, 6, 10 },      // HalfDiminished7
        { 0, 3, 6, 9 },       // Diminished7
        { 0, 3, 7, 11 },      // MinorMajor7
        { 0, 5, 7, 10 },      // Dominant7Sus4
        { 0, 4, 7, 11, 14 },  // Major9
        { 0, 3, 7, 10, 14 },  // Minor9
        { 0, 4, 7, 10, 14 },  // Dominant9
    };

    const int index = std::clamp ((int) q, 0, numChordQualities - 1);
    return table[index];
}

//==============================================================================
std::vector<int> Chord::pitchClasses() const
{
    std::vector<int> result;
    for (int i : qualityIntervals (quality))
        result.push_back (pc (root + i));
    return result;
}

std::string Chord::name (const Key& key) const { return key.spell (root) + qualitySuffix (quality); }

//==============================================================================
const char* functionName (HarmonicFunction f)
{
    switch (f)
    {
        case HarmonicFunction::Tonic:       return "Tonic";
        case HarmonicFunction::Subdominant: return "Subdominant";
        case HarmonicFunction::Dominant:    return "Dominant";
        case HarmonicFunction::Borrowed:    return "Borrowed";
        case HarmonicFunction::Secondary:   return "Secondary";
        case HarmonicFunction::Chromatic:   break;
    }
    return "Chromatic";
}

bool isDiatonic (const Chord& chord, const Key& key)
{
    for (int p : chord.pitchClasses())
        if (! key.contains (p))
            return false;
    return true;
}

//==============================================================================
// TODO (core-theory agent): implement per the contract in Theory.h
HarmonicFunction harmonicFunction (const Chord&, const Key&) { return HarmonicFunction::Tonic; }
std::string romanNumeral (const Chord&, const Key&, bool) { return "I"; }

} // namespace cb
