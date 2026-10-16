#pragma once

// Chordbloom core music theory. Pure C++17, no JUCE. May allocate: never call from the audio thread.

#include <array>
#include <string>
#include <vector>

namespace cb
{

//==============================================================================
// Pitch classes: 0 = C, 1 = C#/Db, ... 11 = B.
inline int pc (int n) { return ((n % 12) + 12) % 12; }

// Plain spelling without key context: pitchClassName (10, true) == "Bb", pitchClassName (10, false) == "A#".
std::string pitchClassName (int pitchClass, bool preferFlats);

//==============================================================================
enum class ScaleType
{
    Major = 0,
    NaturalMinor,
    HarmonicMinor,
    MelodicMinor,   // ascending (jazz) melodic minor
    Dorian,
    Phrygian,
    Lydian,
    Mixolydian,
    Locrian,
    NumScales
};

constexpr int numScaleTypes = (int) ScaleType::NumScales;

struct ScaleInfo
{
    const char* name;            // "Major", "Natural Minor", "Harmonic Minor", "Melodic Minor", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Locrian"
    std::array<int, 7> offsets;  // semitones above tonic; offsets[0] == 0, strictly ascending, all < 12
    bool isMinorFamily;          // true when degree 3 is a minor third (Natural/Harmonic/Melodic Minor, Dorian, Phrygian, Locrian)
};

const ScaleInfo& scaleInfo (ScaleType);

//==============================================================================
struct Key
{
    int tonic = 0;                        // pitch class 0..11
    ScaleType scale = ScaleType::Major;

    std::array<int, 7> pitchClasses() const;  // degree (0..6) -> pitch class
    int degreeOf (int pitchClass) const;      // 0..6, or -1 when not in the scale
    bool contains (int pitchClass) const;

    // The parallel key used for borrowing: minor-family scales -> Major on the same tonic,
    // major-family scales (Major, Lydian, Mixolydian) -> Natural Minor on the same tonic.
    Key parallel() const;

    // Spelling of the tonic: major family uses {C, Db, D, Eb, E, F, F#, G, Ab, A, Bb, B},
    // minor family uses {C, C#, D, Eb, E, F, F#, G, G#, A, Bb, B}.
    std::string tonicName() const;

    // Key-aware note spelling. Scale tones get one distinct letter per degree starting from the tonic's letter
    // (A Harmonic Minor degree 7 -> "G#", F Major degree 4 -> "Bb"). If that would need a double accidental,
    // or the pitch class is not in the scale, falls back to pitchClassName (pitchClass, prefersFlats()).
    std::string spell (int pitchClass) const;

    // True when out-of-scale notes should be spelled with flats. True for keys whose tonicName contains 'b',
    // for C/F major-family keys and for minor-family keys on C, D, G, F, Bb, Eb (flat-side signatures).
    bool prefersFlats() const;

    std::string name() const;                 // e.g. "A Natural Minor", "Eb Major"

    bool operator== (const Key& o) const { return tonic == o.tonic && scale == o.scale; }
    bool operator!= (const Key& o) const { return ! (*this == o); }
};

//==============================================================================
enum class ChordQuality
{
    Major = 0,        // 0 4 7          suffix ""
    Minor,            // 0 3 7          "m"
    Diminished,       // 0 3 6          "dim"
    Augmented,        // 0 4 8          "aug"
    Sus2,             // 0 2 7          "sus2"
    Sus4,             // 0 5 7          "sus4"
    Major6,           // 0 4 7 9        "6"
    Minor6,           // 0 3 7 9        "m6"
    Add9,             // 0 4 7 14       "add9"
    MinorAdd9,        // 0 3 7 14       "m(add9)"
    Major7,           // 0 4 7 11       "maj7"
    Minor7,           // 0 3 7 10       "m7"
    Dominant7,        // 0 4 7 10       "7"
    HalfDiminished7,  // 0 3 6 10       "m7b5"
    Diminished7,      // 0 3 6 9        "dim7"
    MinorMajor7,      // 0 3 7 11       "m(maj7)"
    Dominant7Sus4,    // 0 5 7 10       "7sus4"
    Major9,           // 0 4 7 11 14    "maj9"
    Minor9,           // 0 3 7 10 14    "m9"
    Dominant9,        // 0 4 7 10 14    "9"
    // Extended colours (appended in round 2; enum values are stored in plugin state, so only ever append)
    Minor11,          // 0 3 7 10 14 17 "m11"
    Major69,          // 0 4 7 9 14     "6/9"
    Dominant13,       // 0 4 7 10 14 21 "13"
    Major7Sharp11,    // 0 4 7 11 18    "maj7#11"
    Dominant7Flat9,   // 0 4 7 10 13    "7b9"
    NumQualities
};

constexpr int numChordQualities = (int) ChordQuality::NumQualities;

const char* qualitySuffix (ChordQuality);           // as listed above
const std::vector<int>& qualityIntervals (ChordQuality);  // as listed above (ascending, first is 0, may exceed 12)

//==============================================================================
struct Chord
{
    int root = 0;                                // pitch class 0..11
    ChordQuality quality = ChordQuality::Major;

    std::vector<int> intervals() const { return qualityIntervals (quality); }
    std::vector<int> pitchClasses() const;       // pc (root + interval) in interval order
    int numTones() const { return (int) qualityIntervals (quality).size(); }

    std::string name (const Key& key) const;     // key.spell (root) + qualitySuffix, e.g. "Am7", "Bbmaj7", "F#dim"
                                                 // (roots outside both the key and its parallel key are spelled
                                                 // like their roman numeral: C major F#dim = #iv°, not "Gbdim";
                                                 // A minor Bb = bII, not "A#")

    bool operator== (const Chord& o) const { return root == o.root && quality == o.quality; }
    bool operator!= (const Chord& o) const { return ! (*this == o); }
};

//==============================================================================
enum class HarmonicFunction
{
    Tonic,        // diatonic degrees 1, 3, 6
    Subdominant,  // diatonic degrees 2, 4
    Dominant,     // diatonic degrees 5, 7 (also a major/dominant-7 chord on degree 5 in minor keys; 9, 13 and 7b9
                  // chords count as dominant 7ths everywhere)
    Borrowed,     // not diatonic, but every tone is in key.parallel(); for minor-family scales other than Natural
                  // Minor also every tone in the natural minor on the same tonic (A Harmonic Minor: G, C, Em, Am7)
    Secondary,    // secondary dominant (V, V7, V9, V13 or V7b9 of a diatonic, non-diminished, non-augmented,
                  // non-tonic chord)
    Chromatic     // anything else
};

const char* functionName (HarmonicFunction);   // "Tonic", "Subdominant", "Dominant", "Borrowed", "Secondary", "Chromatic"

// True when every tone of the chord is in the key.
bool isDiatonic (const Chord&, const Key&);

HarmonicFunction harmonicFunction (const Chord&, const Key&);

// Roman numeral for any chord in a key.
//  - Degree numeral from the root's scale degree (mode-relative: A minor's C chord is "III").
//  - Upper case for Major/Augmented/Sus/6/Add9/Major7/Dominant chords, lower case for Minor/Diminished/HalfDim chords.
//  - Suffixes: Diminished "°", Diminished7 "°7", HalfDiminished7 "ø7", Augmented "+", Major7 "maj7", Minor7 "7",
//    Dominant7 "7", Major6/Minor6 "6", Add9/MinorAdd9 "add9", Sus2 "sus2", Sus4 "sus4", Dominant7Sus4 "7sus4",
//    Major9 "maj9", Minor9/Dominant9 "9", MinorMajor7 "(maj7)", Minor11 "11" (lower case), Major69 "6/9",
//    Dominant13 "13", Major7Sharp11 "maj7#11", Dominant7Flat9 "7b9" (C major: ii11, I6/9, V13, IVmaj7#11).
//  - Roots outside the scale get an accidental relative to an adjacent scale degree: by default "b" + the degree
//    above (C major: Bb -> "bVII", Ab -> "bVI", Eb -> "bIII", Db -> "bII", Gb -> "bV"); diminished-family chords
//    (Diminished, Diminished7, HalfDiminished7) instead use "#" + the degree below (C major: F#dim -> "#iv°").
//    Roots from the parallel key use their parallel degree (A minor: C#m -> "#iii"). In Harmonic / Melodic Minor,
//    natural-minor roots keep their natural-minor numeral (A Harmonic Minor: G -> "VII" beside G#dim "vii°").
//  - When detectSecondary and harmonicFunction == Secondary: "V/x", "V7/x", "V9/x", "V13/x" or "V7b9/x" where x is
//    the target's diatonic numeral (C major: D7 -> "V7/V", E -> "V/vi"; A minor: A7b9 -> "V7b9/iv").
std::string romanNumeral (const Chord&, const Key&, bool detectSecondary = true);

// Chord-relative tone spelling (round 2, B12).
// The root is spelled as in Chord::name; every other tone takes the letter its interval implies above the root's
// letter (3rd -> +2 letters, b5/5/#5 -> +4, #11 -> +3, bb7 of dim7 -> +6 ...) with a single accidental, falling back
// to Key::spell when that would need a double accidental. C major: D7 = D F# A C, E7 = E G# B D, B7 = B D# F# A,
// Ab = Ab C Eb; A minor: C7 = C E G Bb; G major: B7 = B D# F# A. Use it for keyboard note names and slash-bass names
// (ProgressionLane: spellChordTone (chord, chord.intervals()[inversion], key)).
std::string spellChordTone (const Chord&, int interval, const Key&);
std::vector<std::string> chordToneNames (const Chord&, const Key&);   // in interval order

} // namespace cb
