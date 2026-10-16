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

namespace
{
    // Letter-per-degree spelling of pitch class p in the 7-note scale `scalePcs` whose degree 0 uses letter
    // index `tonicLetter` (0 = C .. 6 = B). Returns "" when p is not in the scale or would need a double accidental.
    std::string spellWithLetters (int tonicLetter, const std::array<int, 7>& scalePcs, int p)
    {
        static const char letters[] = { 'C', 'D', 'E', 'F', 'G', 'A', 'B' };
        static const int letterPcs[] = { 0, 2, 4, 5, 7, 9, 11 };

        for (int degree = 0; degree < 7; ++degree)
        {
            if (scalePcs[(size_t) degree] != p)
                continue;

            const int letterIndex = (tonicLetter + degree) % 7;
            int diff = p - letterPcs[letterIndex];
            while (diff > 6)  diff -= 12;
            while (diff < -6) diff += 12;

            if (diff == 0)  return std::string (1, letters[letterIndex]);
            if (diff == 1)  return std::string (1, letters[letterIndex]) + "#";
            if (diff == -1) return std::string (1, letters[letterIndex]) + "b";
            return {};
        }
        return {};
    }

    int letterIndexOf (char letter)
    {
        static const char letters[] = { 'C', 'D', 'E', 'F', 'G', 'A', 'B' };
        for (int i = 0; i < 7; ++i)
            if (letters[i] == letter)
                return i;
        return 0;
    }
} // namespace

std::string Key::spell (int pitchClass) const
{
    const int p = pc (pitchClass);
    const int tonicLetter = letterIndexOf (tonicName()[0]);

    if (contains (p))
    {
        auto s = spellWithLetters (tonicLetter, pitchClasses(), p);
        return s.empty() ? pitchClassName (p, prefersFlats()) : s;
    }

    // Out-of-scale tone. Extension of the contract: when the tone belongs to the parallel key (the usual
    // source of borrowed chords), spell it as the parallel scale would, using this key's tonic letter, so
    // that G Major's bVI is "Eb" (not "D#") and A Minor's Picardy third is "C#". Awkward enharmonics
    // (E#, B#, Cb, Fb) are avoided for out-of-scale tones.
    auto s = spellWithLetters (tonicLetter, parallel().pitchClasses(), p);
    if (! s.empty() && s != "E#" && s != "B#" && s != "Cb" && s != "Fb")
        return s;

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
namespace
{
    // Quality of the triad stacked in thirds on `degree` of the key's scale (Major/Minor/Diminished/Augmented).
    ChordQuality stackedTriadQuality (const Key& key, int degree)
    {
        const auto pcs = key.pitchClasses();
        const int d = ((degree % 7) + 7) % 7;
        const int root = pcs[(size_t) d];
        const int third = pc (pcs[(size_t) ((d + 2) % 7)] - root);
        const int fifth = pc (pcs[(size_t) ((d + 4) % 7)] - root);

        if (fifth == 6) return ChordQuality::Diminished;
        if (fifth == 8) return ChordQuality::Augmented;
        return third >= 4 ? ChordQuality::Major : ChordQuality::Minor;
    }

    bool isDominantShaped (ChordQuality q)
    {
        return q == ChordQuality::Major || q == ChordQuality::Dominant7 || q == ChordQuality::Dominant9;
    }

    bool isDiminishedFamily (ChordQuality q)
    {
        return q == ChordQuality::Diminished || q == ChordQuality::Diminished7 || q == ChordQuality::HalfDiminished7;
    }

    bool isLowerCaseQuality (ChordQuality q)
    {
        switch (q)
        {
            case ChordQuality::Minor:
            case ChordQuality::Diminished:
            case ChordQuality::Minor6:
            case ChordQuality::MinorAdd9:
            case ChordQuality::Minor7:
            case ChordQuality::HalfDiminished7:
            case ChordQuality::Diminished7:
            case ChordQuality::MinorMajor7:
            case ChordQuality::Minor9:
                return true;
            default:
                return false;
        }
    }

    // Numeral suffixes per the Theory.h contract. "\xC2\xB0" = U+00B0 (degree sign), "\xC3\xB8" = U+00F8 (o-slash).
    const char* numeralSuffix (ChordQuality q)
    {
        switch (q)
        {
            case ChordQuality::Diminished:      return "\xC2\xB0";
            case ChordQuality::Diminished7:     return "\xC2\xB0" "7";
            case ChordQuality::HalfDiminished7: return "\xC3\xB8" "7";
            case ChordQuality::Augmented:       return "+";
            case ChordQuality::Major7:          return "maj7";
            case ChordQuality::Minor7:          return "7";
            case ChordQuality::Dominant7:       return "7";
            case ChordQuality::Major6:          return "6";
            case ChordQuality::Minor6:          return "6";
            case ChordQuality::Add9:            return "add9";
            case ChordQuality::MinorAdd9:       return "add9";
            case ChordQuality::Sus2:            return "sus2";
            case ChordQuality::Sus4:            return "sus4";
            case ChordQuality::Dominant7Sus4:   return "7sus4";
            case ChordQuality::Major9:          return "maj9";
            case ChordQuality::Minor9:          return "9";
            case ChordQuality::Dominant9:       return "9";
            case ChordQuality::MinorMajor7:     return "(maj7)";
            default:                            return "";
        }
    }

    // Signed distance a - b folded into -5..+6.
    int signedDiff (int a, int b)
    {
        int d = pc (a - b);
        return d > 6 ? d - 12 : d;
    }

    // Scale degree (0..6) used for a root, plus an accidental (-1 = "b", 0 = none, +1 = "#").
    struct DegreeRef
    {
        int degree = 0;
        int accidental = 0;
    };

    DegreeRef locateRoot (int root, ChordQuality quality, const Key& key)
    {
        root = pc (root);
        const int inScale = key.degreeOf (root);
        if (inScale >= 0)
            return { inScale, 0 };

        const int below = key.degreeOf (root - 1);
        const int above = key.degreeOf (root + 1);

        // Diminished-family chords: "#" + the degree below (C major: F#dim -> #iv°).
        if (isDiminishedFamily (quality))
        {
            if (below >= 0) return { below, +1 };
            if (above >= 0) return { above, -1 };
            return {};
        }

        // Roots borrowed from the parallel key are named by their parallel degree, so that major-family keys get
        // "b" (C major: Eb -> bIII, Ab -> bVI, Bb -> bVII, identical to the "b + degree above" rule) and minor-family
        // keys get "#" for raised degrees (A minor: C#m -> #iii, F#m -> #vi) instead of misleading "bIV"/"bVII".
        const Key par = key.parallel();
        const int parDegree = par.degreeOf (root);
        if (parDegree >= 0)
        {
            const int diff = signedDiff (root, key.pitchClasses()[(size_t) parDegree]);
            if (diff == -1) return { parDegree, -1 };
            if (diff == +1) return { parDegree, +1 };
        }

        // Default: "b" + the degree above (C major: Db -> bII, Gb -> bV).
        if (above >= 0) return { above, -1 };
        if (below >= 0) return { below, +1 };
        return {};
    }

    std::string plainNumeral (const Chord& chord, const Key& key)
    {
        static const char* upper[] = { "I", "II", "III", "IV", "V", "VI", "VII" };
        static const char* lower[] = { "i", "ii", "iii", "iv", "v", "vi", "vii" };

        const auto ref = locateRoot (chord.root, chord.quality, key);
        const int d = std::clamp (ref.degree, 0, 6);

        std::string s = ref.accidental < 0 ? "b" : (ref.accidental > 0 ? "#" : "");
        s += isLowerCaseQuality (chord.quality) ? lower[d] : upper[d];
        s += numeralSuffix (chord.quality);
        return s;
    }

    // Dominant-shaped chord on the 5th degree of a key whose 5th is perfect. Contract: minor-family keys ("E in A minor
    // is V"). Extension: also Mixolydian, where D/D7 in G Mixolydian is the (major-key) V, not a chromatic chord.
    bool isNonDiatonicDominant (const Chord& chord, const Key& key)
    {
        return isDominantShaped (chord.quality)
            && key.degreeOf (chord.root) == 4
            && scaleInfo (key.scale).offsets[4] == 7;
    }

    // Degree (1..6) of the diatonic, non-diminished target a dominant-shaped chord points to (a fifth below), or -1.
    int secondaryTargetDegree (const Chord& chord, const Key& key)
    {
        if (! isDominantShaped (chord.quality))
            return -1;

        const int degree = key.degreeOf (pc (chord.root + 5));
        if (degree <= 0)
            return -1;

        if (stackedTriadQuality (key, degree) == ChordQuality::Diminished)
            return -1;

        return degree;
    }
} // namespace

HarmonicFunction harmonicFunction (const Chord& chord, const Key& key)
{
    if (isDiatonic (chord, key))
    {
        switch (key.degreeOf (chord.root))
        {
            case 0: case 2: case 5: return HarmonicFunction::Tonic;
            case 1: case 3:         return HarmonicFunction::Subdominant;
            default:                return HarmonicFunction::Dominant;
        }
    }

    if (isNonDiatonicDominant (chord, key))
        return HarmonicFunction::Dominant;

    if (isDiatonic (chord, key.parallel()))
        return HarmonicFunction::Borrowed;

    if (secondaryTargetDegree (chord, key) > 0)
        return HarmonicFunction::Secondary;

    return HarmonicFunction::Chromatic;
}

std::string romanNumeral (const Chord& chord, const Key& key, bool detectSecondary)
{
    if (detectSecondary && harmonicFunction (chord, key) == HarmonicFunction::Secondary)
    {
        const int targetDegree = secondaryTargetDegree (chord, key);
        const Chord target { key.pitchClasses()[(size_t) targetDegree], stackedTriadQuality (key, targetDegree) };

        std::string s = "V";
        if (chord.quality == ChordQuality::Dominant7) s += "7";
        if (chord.quality == ChordQuality::Dominant9) s += "9";
        return s + "/" + plainNumeral (target, key);
    }

    return plainNumeral (chord, key);
}

} // namespace cb
