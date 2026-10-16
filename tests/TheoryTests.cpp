#include "TestFramework.h"

#include "core/Palette.h"
#include "core/Suggest.h"
#include "core/Theory.h"

#include <algorithm>
#include <map>
#include <set>

// Tests for Theory / Palette / Suggest (owner: core-theory agent)

// Chord-tone spelling (Theory.cpp). Declared here as well because core-theory may not add declarations to Theory.h;
// once the integrator adds them there these become harmless redeclarations.
namespace cb
{
std::string spellChordTone (const Chord&, int interval, const Key&);
std::vector<std::string> chordToneNames (const Chord&, const Key&);
}

namespace
{
using cb::Chord;
using cb::ChordQuality;
using cb::HarmonicFunction;
using cb::Key;
using cb::ScaleType;
using Q = cb::ChordQuality;
using F = cb::HarmonicFunction;

const std::string deg = "\xC2\xB0";    // U+00B0 degree sign, as used by romanNumeral
const std::string half = "\xC3\xB8";   // U+00F8, half-diminished

enum Pc { C = 0, Cs = 1, Db = 1, D = 2, Ds = 3, Eb = 3, E = 4, F_ = 5, Fs = 6, Gb = 6, G = 7, Gs = 8, Ab = 8, A = 9, As = 10, Bb = 10, B = 11 };

Key key (int tonic, ScaleType s) { return { tonic, s }; }
Chord ch (int root, ChordQuality q) { return { root, q }; }
std::string rn (int root, ChordQuality q, const Key& k) { return cb::romanNumeral ({ root, q }, k); }

std::vector<std::string> triadNumerals (const Key& k, bool sevenths)
{
    std::vector<std::string> out;
    for (int d = 0; d < 7; ++d)
        out.push_back (cb::romanNumeral (cb::diatonicChord (k, d, sevenths), k));
    return out;
}

std::vector<std::string> triadNames (const Key& k, bool sevenths)
{
    std::vector<std::string> out;
    for (int d = 0; d < 7; ++d)
        out.push_back (cb::diatonicChord (k, d, sevenths).name (k));
    return out;
}

std::vector<std::string> spellScale (const Key& k)
{
    std::vector<std::string> out;
    for (int p : k.pitchClasses())
        out.push_back (k.spell (p));
    return out;
}

std::vector<Key> allKeys()
{
    std::vector<Key> keys;
    for (int s = 0; s < cb::numScaleTypes; ++s)
        for (int t = 0; t < 12; ++t)
            keys.push_back ({ t, (ScaleType) s });
    return keys;
}

const cb::PaletteSection& section (const std::vector<cb::PaletteSection>& p, cb::PaletteSectionId id)
{
    return p[(size_t) id];
}

bool sectionHas (const cb::PaletteSection& s, const Chord& c)
{
    for (auto& pad : s.chords)
        if (pad.chord == c)
            return true;
    return false;
}

// "V/x", "V7/x", "V9/x", "V13/x", "V7b9/x" (but not "V6/9" or "I6/9").
bool looksSecondary (const std::string& numeral)
{
    for (const char* prefix : { "V/", "V7/", "V9/", "V13/", "V7b9/" })
        if (numeral.rfind (prefix, 0) == 0)
            return true;
    return false;
}

std::vector<std::string> padNames (const cb::PaletteSection& s, const Key& k)
{
    std::vector<std::string> out;
    for (auto& pad : s.chords)
        out.push_back (pad.chord.name (k));
    return out;
}

// Best-scoring pad (over the whole palette) from `from`.
Chord bestNext (const Chord& from, const Key& k)
{
    Chord best {};
    float bestScore = -1.0f;
    for (auto& s : cb::buildPalette (k))
        for (auto& pad : s.chords)
        {
            const float v = cb::transitionScore (from, pad.chord, k);
            if (v > bestScore)
            {
                bestScore = v;
                best = pad.chord;
            }
        }
    return best;
}
} // namespace

//==============================================================================
// Basics
//==============================================================================
TEST_CASE ("theory: pc wraps negative and large values")
{
    CHECK_EQ (cb::pc (-1), 11);
    CHECK_EQ (cb::pc (12), 0);
    CHECK_EQ (cb::pc (-13), 11);
    CHECK_EQ (cb::pc (25), 1);
}

TEST_CASE ("theory: pitchClassName sharps and flats")
{
    CHECK_EQ (cb::pitchClassName (10, true), std::string ("Bb"));
    CHECK_EQ (cb::pitchClassName (10, false), std::string ("A#"));
    CHECK_EQ (cb::pitchClassName (1, true), std::string ("Db"));
    CHECK_EQ (cb::pitchClassName (1, false), std::string ("C#"));
    CHECK_EQ (cb::pitchClassName (-2, true), std::string ("Bb"));
    CHECK_EQ (cb::pitchClassName (4, true), std::string ("E"));
}

TEST_CASE ("theory: scale tables are well formed")
{
    for (int s = 0; s < cb::numScaleTypes; ++s)
    {
        const auto& info = cb::scaleInfo ((ScaleType) s);
        CHECK (info.name != nullptr);
        CHECK_EQ (info.offsets[0], 0);
        for (size_t i = 1; i < 7; ++i)
        {
            CHECK (info.offsets[i] > info.offsets[i - 1]);
            CHECK (info.offsets[i] < 12);
        }
        CHECK_EQ (info.isMinorFamily, info.offsets[2] == 3);
    }
    CHECK_EQ (std::string (cb::scaleInfo (ScaleType::HarmonicMinor).name), std::string ("Harmonic Minor"));
    CHECK_EQ (cb::scaleInfo (ScaleType::Dorian).offsets[5], 9);
    CHECK_EQ (cb::scaleInfo (ScaleType::Lydian).offsets[3], 6);
    CHECK_EQ (cb::scaleInfo (ScaleType::Locrian).offsets[4], 6);
}

TEST_CASE ("theory: quality tables match the header")
{
    CHECK_EQ (std::string (cb::qualitySuffix (Q::Major)), std::string (""));
    CHECK_EQ (std::string (cb::qualitySuffix (Q::MinorAdd9)), std::string ("m(add9)"));
    CHECK_EQ (std::string (cb::qualitySuffix (Q::HalfDiminished7)), std::string ("m7b5"));
    CHECK_EQ (std::string (cb::qualitySuffix (Q::MinorMajor7)), std::string ("m(maj7)"));
    CHECK_EQ (std::string (cb::qualitySuffix (Q::Dominant9)), std::string ("9"));
    CHECK_EQ (cb::qualityIntervals (Q::Dominant7Sus4), (std::vector<int> { 0, 5, 7, 10 }));
    CHECK_EQ (cb::qualityIntervals (Q::Major9), (std::vector<int> { 0, 4, 7, 11, 14 }));

    for (int q = 0; q < cb::numChordQualities; ++q)
    {
        const auto& iv = cb::qualityIntervals ((ChordQuality) q);
        REQUIRE (! iv.empty());
        CHECK_EQ (iv[0], 0);
        for (size_t i = 1; i < iv.size(); ++i)
            CHECK (iv[i] > iv[i - 1]);
    }
}

TEST_CASE ("theory: key degreeOf / contains / parallel")
{
    const auto c = key (C, ScaleType::Major);
    CHECK_EQ (c.degreeOf (G), 4);
    CHECK_EQ (c.degreeOf (Fs), -1);
    CHECK_EQ (c.degreeOf (G + 12), 4);
    CHECK (c.contains (B));
    CHECK (! c.contains (Bb));

    CHECK (key (A, ScaleType::NaturalMinor).parallel() == key (A, ScaleType::Major));
    CHECK (key (A, ScaleType::Dorian).parallel() == key (A, ScaleType::Major));
    CHECK (key (G, ScaleType::Mixolydian).parallel() == key (G, ScaleType::NaturalMinor));
    CHECK (key (F_, ScaleType::Lydian).parallel() == key (F_, ScaleType::NaturalMinor));
    CHECK (key (E, ScaleType::Phrygian).parallel() == key (E, ScaleType::Major));
}

TEST_CASE ("theory: tonic names, key names and flat preference")
{
    CHECK_EQ (key (1, ScaleType::Major).tonicName(), std::string ("Db"));
    CHECK_EQ (key (1, ScaleType::NaturalMinor).tonicName(), std::string ("C#"));
    CHECK_EQ (key (8, ScaleType::Mixolydian).tonicName(), std::string ("Ab"));
    CHECK_EQ (key (8, ScaleType::HarmonicMinor).tonicName(), std::string ("G#"));
    CHECK_EQ (key (6, ScaleType::Major).tonicName(), std::string ("F#"));
    CHECK_EQ (key (A, ScaleType::NaturalMinor).name(), std::string ("A Natural Minor"));
    CHECK_EQ (key (Eb, ScaleType::Major).name(), std::string ("Eb Major"));

    CHECK (key (C, ScaleType::Major).prefersFlats());
    CHECK (key (F_, ScaleType::Major).prefersFlats());
    CHECK (key (Eb, ScaleType::Major).prefersFlats());
    CHECK (! key (G, ScaleType::Major).prefersFlats());
    CHECK (! key (A, ScaleType::NaturalMinor).prefersFlats());
    CHECK (key (D, ScaleType::NaturalMinor).prefersFlats());
    CHECK (key (G, ScaleType::Dorian).prefersFlats());
    CHECK (! key (E, ScaleType::NaturalMinor).prefersFlats());
}

//==============================================================================
// Spelling
//==============================================================================
TEST_CASE ("spelling: F major uses Bb")
{
    CHECK_EQ (spellScale (key (F_, ScaleType::Major)), (std::vector<std::string> { "F", "G", "A", "Bb", "C", "D", "E" }));
}

TEST_CASE ("spelling: D major uses F# and C#")
{
    CHECK_EQ (spellScale (key (D, ScaleType::Major)), (std::vector<std::string> { "D", "E", "F#", "G", "A", "B", "C#" }));
}

TEST_CASE ("spelling: Eb major uses Ab")
{
    CHECK_EQ (spellScale (key (Eb, ScaleType::Major)), (std::vector<std::string> { "Eb", "F", "G", "Ab", "Bb", "C", "D" }));
}

TEST_CASE ("spelling: A harmonic minor uses G#")
{
    CHECK_EQ (spellScale (key (A, ScaleType::HarmonicMinor)), (std::vector<std::string> { "A", "B", "C", "D", "E", "F", "G#" }));
}

TEST_CASE ("spelling: Db major")
{
    CHECK_EQ (spellScale (key (Db, ScaleType::Major)), (std::vector<std::string> { "Db", "Eb", "F", "Gb", "Ab", "Bb", "C" }));
}

TEST_CASE ("spelling: more keys and modes")
{
    CHECK_EQ (spellScale (key (B, ScaleType::Major)), (std::vector<std::string> { "B", "C#", "D#", "E", "F#", "G#", "A#" }));
    CHECK_EQ (spellScale (key (Ab, ScaleType::Major)), (std::vector<std::string> { "Ab", "Bb", "C", "Db", "Eb", "F", "G" }));
    CHECK_EQ (spellScale (key (Fs, ScaleType::NaturalMinor)), (std::vector<std::string> { "F#", "G#", "A", "B", "C#", "D", "E" }));
    CHECK_EQ (spellScale (key (G, ScaleType::NaturalMinor)), (std::vector<std::string> { "G", "A", "Bb", "C", "D", "Eb", "F" }));
    CHECK_EQ (spellScale (key (D, ScaleType::Dorian)), (std::vector<std::string> { "D", "E", "F", "G", "A", "B", "C" }));
    CHECK_EQ (spellScale (key (E, ScaleType::Phrygian)), (std::vector<std::string> { "E", "F", "G", "A", "B", "C", "D" }));
    CHECK_EQ (spellScale (key (F_, ScaleType::Lydian)), (std::vector<std::string> { "F", "G", "A", "B", "C", "D", "E" }));
    CHECK_EQ (spellScale (key (A, ScaleType::MelodicMinor)), (std::vector<std::string> { "A", "B", "C", "D", "E", "F#", "G#" }));
    CHECK_EQ (spellScale (key (Cs, ScaleType::HarmonicMinor)), (std::vector<std::string> { "C#", "D#", "E", "F#", "G#", "A", "B#" }));
}

TEST_CASE ("spelling: every scale tone gets its own letter (no double accidentals needed)")
{
    for (auto& k : allKeys())
    {
        std::set<char> letters;
        for (auto& s : spellScale (k))
        {
            REQUIRE (! s.empty());
            CHECK (s.size() <= 2);
            letters.insert (s[0]);
        }
        // Keys that would need a double accidental fall back to plain names, so allow one collision at most.
        CHECK (letters.size() >= 6);
    }
}

TEST_CASE ("spelling: out-of-scale tones follow the parallel key")
{
    const auto c = key (C, ScaleType::Major);
    CHECK_EQ (c.spell (Bb), std::string ("Bb"));
    CHECK_EQ (c.spell (Ab), std::string ("Ab"));
    CHECK_EQ (c.spell (Eb), std::string ("Eb"));
    CHECK_EQ (c.spell (Db), std::string ("Db"));

    const auto g = key (G, ScaleType::Major);   // borrowed bVI / bIII must not read as D# / A#
    CHECK_EQ (g.spell (Eb), std::string ("Eb"));
    CHECK_EQ (g.spell (Bb), std::string ("Bb"));
    CHECK_EQ (key (D, ScaleType::Major).spell (Bb), std::string ("Bb"));

    const auto a = key (A, ScaleType::NaturalMinor);
    CHECK_EQ (a.spell (Cs), std::string ("C#"));
    CHECK_EQ (a.spell (Fs), std::string ("F#"));
    CHECK_EQ (a.spell (Gs), std::string ("G#"));
    CHECK_EQ (key (C, ScaleType::NaturalMinor).spell (E), std::string ("E"));
}

TEST_CASE ("spelling: chord names")
{
    const auto c = key (C, ScaleType::Major);
    CHECK_EQ (ch (A, Q::Minor7).name (c), std::string ("Am7"));
    CHECK_EQ (ch (Bb, Q::Major7).name (key (F_, ScaleType::Major)), std::string ("Bbmaj7"));
    CHECK_EQ (ch (Fs, Q::Diminished).name (key (G, ScaleType::Major)), std::string ("F#dim"));
    CHECK_EQ (ch (Gs, Q::Diminished7).name (key (A, ScaleType::HarmonicMinor)), std::string ("G#dim7"));
    CHECK_EQ (ch (A, Q::MinorAdd9).name (key (A, ScaleType::NaturalMinor)), std::string ("Am(add9)"));
    CHECK_EQ (ch (Eb, Q::Major).name (key (G, ScaleType::Major)), std::string ("Eb"));
    CHECK_EQ (ch (C, Q::Dominant7Sus4).name (c), std::string ("C7sus4"));
}

//==============================================================================
// Roman numerals
//==============================================================================
TEST_CASE ("roman: C major triads")
{
    CHECK_EQ (triadNumerals (key (C, ScaleType::Major), false),
              (std::vector<std::string> { "I", "ii", "iii", "IV", "V", "vi", "vii" + deg }));
}

TEST_CASE ("roman: C major sevenths")
{
    CHECK_EQ (triadNumerals (key (C, ScaleType::Major), true),
              (std::vector<std::string> { "Imaj7", "ii7", "iii7", "IVmaj7", "V7", "vi7", "vii" + half + "7" }));
}

TEST_CASE ("roman: major key numerals are the same in every tonic")
{
    for (int t = 0; t < 12; ++t)
    {
        CHECK_EQ (triadNumerals (key (t, ScaleType::Major), false),
                  (std::vector<std::string> { "I", "ii", "iii", "IV", "V", "vi", "vii" + deg }));
        CHECK_EQ (triadNumerals (key (t, ScaleType::NaturalMinor), true),
                  (std::vector<std::string> { "i7", "ii" + half + "7", "IIImaj7", "iv7", "v7", "VImaj7", "VII7" }));
    }
}

TEST_CASE ("roman: natural minor triads and sevenths")
{
    const auto a = key (A, ScaleType::NaturalMinor);
    CHECK_EQ (triadNumerals (a, false), (std::vector<std::string> { "i", "ii" + deg, "III", "iv", "v", "VI", "VII" }));
    CHECK_EQ (triadNumerals (a, true),
              (std::vector<std::string> { "i7", "ii" + half + "7", "IIImaj7", "iv7", "v7", "VImaj7", "VII7" }));
    CHECK_EQ (rn (C, Q::Major, a), std::string ("III"));   // mode-relative
}

TEST_CASE ("roman: harmonic minor triads and sevenths")
{
    const auto a = key (A, ScaleType::HarmonicMinor);
    CHECK_EQ (triadNumerals (a, false), (std::vector<std::string> { "i", "ii" + deg, "III+", "iv", "V", "VI", "vii" + deg }));
    CHECK_EQ (triadNumerals (a, true),
              (std::vector<std::string> { "i(maj7)", "ii" + half + "7", "III+", "iv7", "V7", "VImaj7", "vii" + deg + "7" }));
}

TEST_CASE ("roman: dorian triads and sevenths")
{
    const auto d = key (D, ScaleType::Dorian);
    CHECK_EQ (triadNumerals (d, false), (std::vector<std::string> { "i", "ii", "III", "IV", "v", "vi" + deg, "VII" }));
    CHECK_EQ (triadNumerals (d, true),
              (std::vector<std::string> { "i7", "ii7", "IIImaj7", "IV7", "v7", "vi" + half + "7", "VIImaj7" }));
}

TEST_CASE ("roman: mixolydian triads and sevenths")
{
    const auto g = key (G, ScaleType::Mixolydian);
    CHECK_EQ (triadNumerals (g, false), (std::vector<std::string> { "I", "ii", "iii" + deg, "IV", "v", "vi", "VII" }));
    CHECK_EQ (triadNumerals (g, true),
              (std::vector<std::string> { "I7", "ii7", "iii" + half + "7", "IVmaj7", "v7", "vi7", "VIImaj7" }));
}

TEST_CASE ("roman: melodic minor, phrygian, lydian, locrian triads")
{
    CHECK_EQ (triadNumerals (key (A, ScaleType::MelodicMinor), false),
              (std::vector<std::string> { "i", "ii", "III+", "IV", "V", "vi" + deg, "vii" + deg }));
    CHECK_EQ (triadNumerals (key (E, ScaleType::Phrygian), false),
              (std::vector<std::string> { "i", "II", "III", "iv", "v" + deg, "VI", "vii" }));
    CHECK_EQ (triadNumerals (key (F_, ScaleType::Lydian), false),
              (std::vector<std::string> { "I", "II", "iii", "iv" + deg, "V", "vi", "vii" }));
    CHECK_EQ (triadNumerals (key (B, ScaleType::Locrian), false),
              (std::vector<std::string> { "i" + deg, "II", "iii", "iv", "V", "VI", "vii" }));
}

TEST_CASE ("roman: every quality suffix and case rule")
{
    const auto c = key (C, ScaleType::Major);
    CHECK_EQ (rn (C, Q::Sus2, c), std::string ("Isus2"));
    CHECK_EQ (rn (C, Q::Sus4, c), std::string ("Isus4"));
    CHECK_EQ (rn (C, Q::Major6, c), std::string ("I6"));
    CHECK_EQ (rn (A, Q::Minor6, c), std::string ("vi6"));
    CHECK_EQ (rn (C, Q::Add9, c), std::string ("Iadd9"));
    CHECK_EQ (rn (A, Q::MinorAdd9, c), std::string ("viadd9"));
    CHECK_EQ (rn (G, Q::Dominant7Sus4, c), std::string ("V7sus4"));
    CHECK_EQ (rn (F_, Q::Major9, c), std::string ("IVmaj9"));
    CHECK_EQ (rn (D, Q::Minor9, c), std::string ("ii9"));
    CHECK_EQ (rn (G, Q::Dominant9, c), std::string ("V9"));
    CHECK_EQ (rn (C, Q::Augmented, c), std::string ("I+"));
    CHECK_EQ (rn (B, Q::Diminished7, c), std::string ("vii" + deg + "7"));
    CHECK_EQ (rn (A, Q::MinorMajor7, key (A, ScaleType::NaturalMinor)), std::string ("i(maj7)"));
    CHECK_EQ (rn (B, Q::HalfDiminished7, c), std::string ("vii" + half + "7"));
}

TEST_CASE ("roman: borrowed chords in C major")
{
    const auto c = key (C, ScaleType::Major);
    CHECK_EQ (rn (Bb, Q::Major, c), std::string ("bVII"));
    CHECK_EQ (rn (Ab, Q::Major, c), std::string ("bVI"));
    CHECK_EQ (rn (Eb, Q::Major, c), std::string ("bIII"));
    CHECK_EQ (rn (F_, Q::Minor, c), std::string ("iv"));
    CHECK_EQ (rn (F_, Q::Minor7, c), std::string ("iv7"));
    CHECK_EQ (rn (Ab, Q::Major7, c), std::string ("bVImaj7"));
    CHECK_EQ (rn (G, Q::Minor, c), std::string ("v"));
    CHECK_EQ (rn (C, Q::Minor, c), std::string ("i"));
    CHECK_EQ (rn (D, Q::Diminished, c), std::string ("ii" + deg));
    CHECK_EQ (rn (Bb, Q::Dominant7, c), std::string ("bVII7"));
}

TEST_CASE ("roman: chromatic roots use b + degree above, diminished uses # + degree below")
{
    const auto c = key (C, ScaleType::Major);
    CHECK_EQ (rn (Db, Q::Major, c), std::string ("bII"));
    CHECK_EQ (rn (Gb, Q::Major, c), std::string ("bV"));
    CHECK_EQ (rn (Fs, Q::Diminished, c), std::string ("#iv" + deg));
    CHECK_EQ (rn (Cs, Q::Diminished7, c), std::string ("#i" + deg + "7"));
    CHECK_EQ (rn (Ds, Q::HalfDiminished7, c), std::string ("#ii" + half + "7"));
    CHECK_EQ (rn (Fs, Q::Minor, c), std::string ("bv"));
}

TEST_CASE ("roman: borrowed chords in A minor")
{
    const auto a = key (A, ScaleType::NaturalMinor);
    CHECK_EQ (rn (D, Q::Major, a), std::string ("IV"));
    CHECK_EQ (rn (A, Q::Major, a), std::string ("I"));
    CHECK_EQ (rn (E, Q::Major, a), std::string ("V"));
    CHECK_EQ (rn (E, Q::Dominant7, a), std::string ("V7"));
    CHECK_EQ (rn (B, Q::Minor, a), std::string ("ii"));
    CHECK_EQ (rn (Cs, Q::Minor, a), std::string ("#iii"));
    CHECK_EQ (rn (Fs, Q::Minor, a), std::string ("#vi"));
    CHECK_EQ (rn (Gs, Q::Diminished, a), std::string ("#vii" + deg));
    CHECK_EQ (rn (Bb, Q::Major, a), std::string ("bII"));   // Neapolitan
}

TEST_CASE ("roman: secondary dominants in C major")
{
    const auto c = key (C, ScaleType::Major);
    CHECK_EQ (rn (A, Q::Dominant7, c), std::string ("V7/ii"));
    CHECK_EQ (rn (B, Q::Dominant7, c), std::string ("V7/iii"));
    CHECK_EQ (rn (C, Q::Dominant7, c), std::string ("V7/IV"));
    CHECK_EQ (rn (D, Q::Dominant7, c), std::string ("V7/V"));
    CHECK_EQ (rn (E, Q::Dominant7, c), std::string ("V7/vi"));
    CHECK_EQ (rn (E, Q::Major, c), std::string ("V/vi"));
    CHECK_EQ (rn (D, Q::Major, c), std::string ("V/V"));
    CHECK_EQ (rn (A, Q::Major, c), std::string ("V/ii"));
    CHECK_EQ (rn (D, Q::Dominant9, c), std::string ("V9/V"));
    CHECK_EQ (rn (G, Q::Dominant7, c), std::string ("V7"));   // diatonic: not V7/I
    CHECK_EQ (rn (Fs, Q::Dominant7, c), std::string ("bV7")); // target vii° is diminished
}

TEST_CASE ("roman: secondary dominants in minor keys, and the minor-key V")
{
    const auto a = key (A, ScaleType::NaturalMinor);
    CHECK_EQ (rn (A, Q::Dominant7, a), std::string ("V7/iv"));
    CHECK_EQ (rn (B, Q::Dominant7, a), std::string ("V7/v"));
    CHECK_EQ (rn (C, Q::Dominant7, a), std::string ("V7/VI"));
    CHECK_EQ (rn (D, Q::Dominant7, a), std::string ("V7/VII"));
    CHECK_EQ (rn (E, Q::Major, a), std::string ("V"));
    CHECK_EQ (rn (E, Q::Dominant7, a), std::string ("V7"));
    CHECK_EQ (rn (B, Q::Dominant7, key (A, ScaleType::HarmonicMinor)), std::string ("V7/V"));
}

TEST_CASE ("roman: detectSecondary=false gives the plain numeral")
{
    const auto c = key (C, ScaleType::Major);
    CHECK_EQ (cb::romanNumeral (ch (D, Q::Dominant7), c, false), std::string ("II7"));
    CHECK_EQ (cb::romanNumeral (ch (E, Q::Major), c, false), std::string ("III"));
    CHECK_EQ (cb::romanNumeral (ch (Bb, Q::Major), c, false), std::string ("bVII"));
}

//==============================================================================
// Harmonic function
//==============================================================================
TEST_CASE ("function: names")
{
    CHECK_EQ (std::string (cb::functionName (F::Tonic)), std::string ("Tonic"));
    CHECK_EQ (std::string (cb::functionName (F::Subdominant)), std::string ("Subdominant"));
    CHECK_EQ (std::string (cb::functionName (F::Dominant)), std::string ("Dominant"));
    CHECK_EQ (std::string (cb::functionName (F::Borrowed)), std::string ("Borrowed"));
    CHECK_EQ (std::string (cb::functionName (F::Secondary)), std::string ("Secondary"));
    CHECK_EQ (std::string (cb::functionName (F::Chromatic)), std::string ("Chromatic"));
}

TEST_CASE ("function: isDiatonic")
{
    const auto c = key (C, ScaleType::Major);
    CHECK (cb::isDiatonic (ch (F_, Q::Major9), c));
    CHECK (cb::isDiatonic (ch (G, Q::Dominant7Sus4), c));
    CHECK (! cb::isDiatonic (ch (F_, Q::Minor), c));
    CHECK (! cb::isDiatonic (ch (G, Q::Major7), c));
    CHECK (cb::isDiatonic (ch (Gs, Q::Diminished7), key (A, ScaleType::HarmonicMinor)));
}

TEST_CASE ("function: diatonic degrees in every key")
{
    const F expected[7] = { F::Tonic, F::Subdominant, F::Tonic, F::Subdominant, F::Dominant, F::Tonic, F::Dominant };
    for (auto& k : allKeys())
        for (int d = 0; d < 7; ++d)
        {
            CHECK_EQ (cb::harmonicFunction (cb::diatonicChord (k, d, false), k), expected[d]);
            CHECK_EQ (cb::harmonicFunction (cb::diatonicChord (k, d, true), k), expected[d]);
        }
}

TEST_CASE ("function: borrowed, secondary and chromatic in C major")
{
    const auto c = key (C, ScaleType::Major);
    CHECK_EQ (cb::harmonicFunction (ch (F_, Q::Minor), c), F::Borrowed);
    CHECK_EQ (cb::harmonicFunction (ch (Bb, Q::Major), c), F::Borrowed);
    CHECK_EQ (cb::harmonicFunction (ch (Ab, Q::Major), c), F::Borrowed);
    CHECK_EQ (cb::harmonicFunction (ch (Eb, Q::Major), c), F::Borrowed);
    CHECK_EQ (cb::harmonicFunction (ch (D, Q::HalfDiminished7), c), F::Borrowed);
    CHECK_EQ (cb::harmonicFunction (ch (D, Q::Dominant7), c), F::Secondary);
    CHECK_EQ (cb::harmonicFunction (ch (A, Q::Dominant7), c), F::Secondary);
    CHECK_EQ (cb::harmonicFunction (ch (E, Q::Major), c), F::Secondary);
    CHECK_EQ (cb::harmonicFunction (ch (C, Q::Dominant7), c), F::Secondary);
    CHECK_EQ (cb::harmonicFunction (ch (G, Q::Dominant7), c), F::Dominant);
    CHECK_EQ (cb::harmonicFunction (ch (Fs, Q::Dominant7), c), F::Chromatic);   // V7/vii°: target is diminished
    CHECK_EQ (cb::harmonicFunction (ch (Db, Q::Major), c), F::Chromatic);
    CHECK_EQ (cb::harmonicFunction (ch (Fs, Q::Minor), c), F::Chromatic);
}

TEST_CASE ("function: minor-key V is Dominant, not secondary or borrowed")
{
    for (int t = 0; t < 12; ++t)
        for (auto s : { ScaleType::NaturalMinor, ScaleType::Dorian, ScaleType::Phrygian, ScaleType::HarmonicMinor })
        {
            const auto k = key (t, s);
            CHECK_EQ (cb::harmonicFunction (ch (t + 7, Q::Major), k), F::Dominant);
            CHECK_EQ (cb::harmonicFunction (ch (t + 7, Q::Dominant7), k), F::Dominant);
        }

    const auto a = key (A, ScaleType::NaturalMinor);
    CHECK_EQ (cb::harmonicFunction (ch (D, Q::Major), a), F::Borrowed);
    CHECK_EQ (cb::harmonicFunction (ch (A, Q::Major), a), F::Borrowed);
    CHECK_EQ (cb::harmonicFunction (ch (B, Q::Minor), a), F::Borrowed);
    CHECK_EQ (cb::harmonicFunction (ch (A, Q::Dominant7), a), F::Secondary);
    CHECK_EQ (cb::harmonicFunction (ch (Bb, Q::Major), a), F::Chromatic);
}

TEST_CASE ("function: roman numeral and function agree on secondary labels")
{
    for (auto& k : allKeys())
        for (int r = 0; r < 12; ++r)
            for (int q = 0; q < cb::numChordQualities; ++q)
            {
                const Chord c { r, (ChordQuality) q };
                const auto numeral = cb::romanNumeral (c, k);
                REQUIRE (! numeral.empty());
                // Round 2: "6/9" also contains a slash, so look for the secondary label shape "V<suffix>/x".
                CHECK_EQ (looksSecondary (numeral), cb::harmonicFunction (c, k) == F::Secondary);
            }
}

//==============================================================================
// Palette
//==============================================================================
TEST_CASE ("palette: diatonicChord triads and sevenths")
{
    const auto c = key (C, ScaleType::Major);
    CHECK (cb::diatonicChord (c, 4, true) == ch (G, Q::Dominant7));
    CHECK (cb::diatonicChord (c, 6, false) == ch (B, Q::Diminished));
    CHECK (cb::diatonicChord (c, 7, false) == ch (C, Q::Major));    // wraps
    CHECK (cb::diatonicChord (c, -1, false) == ch (B, Q::Diminished));

    CHECK_EQ (triadNames (key (A, ScaleType::HarmonicMinor), false),
              (std::vector<std::string> { "Am", "Bdim", "Caug", "Dm", "E", "F", "G#dim" }));
    CHECK_EQ (triadNames (key (A, ScaleType::HarmonicMinor), true),
              (std::vector<std::string> { "Am(maj7)", "Bm7b5", "Caug", "Dm7", "E7", "Fmaj7", "G#dim7" }));
    CHECK_EQ (triadNames (key (A, ScaleType::MelodicMinor), true),
              (std::vector<std::string> { "Am(maj7)", "Bm7", "Caug", "D7", "E7", "F#m7b5", "G#m7b5" }));
    CHECK_EQ (triadNames (key (Eb, ScaleType::Major), true),
              (std::vector<std::string> { "Ebmaj7", "Fm7", "Gm7", "Abmaj7", "Bb7", "Cm7", "Dm7b5" }));
}

TEST_CASE ("palette: structure holds for all 12 tonics x 9 scales")
{
    static const char* titles[] = { "Triads", "Sevenths", "Color", "Borrowed", "Secondary" };

    for (auto& k : allKeys())
    {
        const auto p = cb::buildPalette (k);
        REQUIRE (p.size() == (size_t) cb::numPaletteSections);

        for (int i = 0; i < cb::numPaletteSections; ++i)
        {
            CHECK_EQ ((int) p[(size_t) i].id, i);
            CHECK_EQ (p[(size_t) i].title, std::string (titles[i]));
            CHECK (p[(size_t) i].chords.size() <= (size_t) cb::kMaxPadsPerSection);

            const auto& pads = p[(size_t) i].chords;
            for (size_t a = 0; a < pads.size(); ++a)
                for (size_t b = a + 1; b < pads.size(); ++b)
                    CHECK (pads[a].chord != pads[b].chord);
        }

        CHECK_EQ (p[0].chords.size(), (size_t) 7);
        CHECK_EQ (p[1].chords.size(), (size_t) 7);
        CHECK (p[2].chords.size() >= 4);     // every mode has a usable colour row
        CHECK (p[3].chords.size() >= 4);
    }
}

TEST_CASE ("palette: triads and sevenths are the diatonic chords in degree order")
{
    for (auto& k : allKeys())
    {
        const auto p = cb::buildPalette (k);
        const auto pcs = k.pitchClasses();
        for (int d = 0; d < 7; ++d)
        {
            const auto& t = p[0].chords[(size_t) d].chord;
            const auto& s = p[1].chords[(size_t) d].chord;
            CHECK (t == cb::diatonicChord (k, d, false));
            CHECK (s == cb::diatonicChord (k, d, true));
            CHECK_EQ (t.root, pcs[(size_t) d]);
            CHECK_EQ (s.root, pcs[(size_t) d]);
            CHECK (cb::isDiatonic (t, k));
            CHECK (cb::isDiatonic (s, k));
            CHECK_EQ (t.numTones(), 3);
        }
    }
}

TEST_CASE ("palette: colour pads are diatonic, new, and in degree order")
{
    for (auto& k : allKeys())
    {
        const auto p = cb::buildPalette (k);
        int lastDegree = -1;
        for (auto& pad : p[2].chords)
        {
            CHECK (cb::isDiatonic (pad.chord, k));
            CHECK (! sectionHas (p[0], pad.chord));
            CHECK (! sectionHas (p[1], pad.chord));
            const int d = k.degreeOf (pad.chord.root);
            CHECK (d >= lastDegree);
            lastDegree = d;
        }
    }
}

// Round 2 (B13, intentional change): minor-family scales other than Natural Minor also borrow from the natural minor.
TEST_CASE ("palette: the colour row spreads over its roots")
{
    for (auto& k : allKeys())
    {
        const auto pal = cb::buildPalette (k);
        const auto& colour = pal[(size_t) cb::PaletteSectionId::Color].chords;
        std::map<int, int> perRoot;
        for (auto& pad : colour)
            ++perRoot[cb::pc (pad.chord.root)];
        for (auto& kv : perRoot)
            CHECK (kv.second <= 4);   // Lydian's curated tonic colours (maj7#11 add9 6/9 maj9) are the most on one root
        if (k.scale == ScaleType::Locrian)
        {
            CHECK (colour.size() >= 6);
            for (auto& kv : perRoot)
                CHECK (kv.second <= 2);   // B Locrian used to show five F chords
        }
    }
}

TEST_CASE ("palette: borrowed pads come from the parallel key or, in altered minor scales, the natural minor")
{
    for (auto& k : allKeys())
    {
        const bool alteredMinor = cb::scaleInfo (k.scale).isMinorFamily && k.scale != ScaleType::NaturalMinor;
        const Key natural { k.tonic, ScaleType::NaturalMinor };
        const auto p = cb::buildPalette (k);
        for (auto& pad : p[3].chords)
        {
            CHECK (! cb::isDiatonic (pad.chord, k));
            CHECK (cb::isDiatonic (pad.chord, k.parallel()) || (alteredMinor && cb::isDiatonic (pad.chord, natural)));
            CHECK (pad.function == F::Borrowed || pad.function == F::Dominant);
        }
    }
}

// Round 2 (I19, intentional change): minor-family keys also offer V7b9/x right after V7/x.
TEST_CASE ("palette: secondary pads are V7 (or V7b9 in minor keys) of diatonic targets in degree order")
{
    for (auto& k : allKeys())
    {
        const bool minorKey = cb::scaleInfo (k.scale).isMinorFamily;
        const auto p = cb::buildPalette (k);
        int lastTarget = 0;
        Chord last {};
        for (auto& pad : p[4].chords)
        {
            CHECK_EQ (pad.function, F::Secondary);
            const int target = k.degreeOf (pad.chord.root + 5);
            const auto tq = cb::diatonicChord (k, target, false).quality;
            CHECK (tq != Q::Diminished);
            CHECK (tq != Q::Augmented);
            for (int s = 0; s < 4; ++s)
                CHECK (! sectionHas (p[(size_t) s], pad.chord));

            if (pad.chord.quality == Q::Dominant7Flat9)
            {
                CHECK (minorKey);
                CHECK (last == ch (pad.chord.root, Q::Dominant7));     // directly after its plain V7
                CHECK (tq == Q::Minor || k.contains (pad.chord.root + 1));
                CHECK (pad.roman.rfind ("V7b9/", 0) == 0);
                CHECK_EQ (target, lastTarget);
            }
            else
            {
                CHECK (pad.chord.quality == Q::Dominant7);
                CHECK (pad.roman.rfind ("V7/", 0) == 0);
                CHECK (target > lastTarget);
            }
            lastTarget = target;
            last = pad.chord;
        }
        CHECK (p[4].chords.size() <= 6);
    }
}

TEST_CASE ("palette: every possible V7/x is offered before any V7b9 variant")
{
    for (auto& k : allKeys())
    {
        const auto p = cb::buildPalette (k);
        for (int target = 1; target < 7; ++target)
        {
            const auto t = cb::diatonicChord (k, target, false);
            if (t.quality == Q::Diminished || t.quality == Q::Augmented)
                continue;
            const Chord v7 { cb::pc (t.root + 7), Q::Dominant7 };
            if (cb::harmonicFunction (v7, k) != F::Secondary)
                continue;
            bool earlier = false;
            for (int s = 0; s < 4; ++s)
                earlier = earlier || sectionHas (p[(size_t) s], v7);
            CHECK (earlier || sectionHas (p[4], v7));
        }
    }
}

TEST_CASE ("palette: roman, function and hint are filled consistently")
{
    for (auto& k : allKeys())
        for (auto& s : cb::buildPalette (k))
            for (auto& pad : s.chords)
            {
                CHECK_EQ (pad.roman, cb::romanNumeral (pad.chord, k));
                CHECK_EQ (pad.function, cb::harmonicFunction (pad.chord, k));
                REQUIRE (pad.hint.size() > 10);
                CHECK (pad.hint.size() < 110);
                CHECK (pad.hint.back() == '.');
                CHECK (pad.hint.find ("  ") == std::string::npos);
            }
}

TEST_CASE ("palette: deterministic")
{
    for (auto& k : allKeys())
    {
        const auto a = cb::buildPalette (k);
        const auto b = cb::buildPalette (k);
        REQUIRE (a.size() == b.size());
        for (size_t i = 0; i < a.size(); ++i)
        {
            REQUIRE (a[i].chords.size() == b[i].chords.size());
            CHECK_EQ (a[i].title, b[i].title);
            for (size_t j = 0; j < a[i].chords.size(); ++j)
            {
                CHECK (a[i].chords[j].chord == b[i].chords[j].chord);
                CHECK_EQ (a[i].chords[j].roman, b[i].chords[j].roman);
                CHECK_EQ (a[i].chords[j].hint, b[i].chords[j].hint);
            }
        }
    }
}

TEST_CASE ("palette: C major contents")
{
    const auto c = key (C, ScaleType::Major);
    const auto p = cb::buildPalette (c);

    std::vector<std::string> names;
    for (auto& pad : p[0].chords) names.push_back (pad.chord.name (c));
    CHECK_EQ (names, (std::vector<std::string> { "C", "Dm", "Em", "F", "G", "Am", "Bdim" }));

    names.clear();
    for (auto& pad : p[1].chords) names.push_back (pad.chord.name (c));
    CHECK_EQ (names, (std::vector<std::string> { "Cmaj7", "Dm7", "Em7", "Fmaj7", "G7", "Am7", "Bm7b5" }));

    // Round 2 (I16 / I19, intentional change): data-driven colours plus 6/9, the Lydian IVmaj7#11 and V13.
    CHECK_EQ (padNames (p[2], c), (std::vector<std::string> { "Cadd9", "C6/9", "Cmaj9", "Dm9", "Fadd9", "F6", "Fmaj7#11", "G13" }));
    names.clear();
    for (auto& pad : p[2].chords) names.push_back (pad.roman);
    CHECK_EQ (names, (std::vector<std::string> { "Iadd9", "I6/9", "Imaj9", "ii9", "IVadd9", "IV6", "IVmaj7#11", "V13" }));

    // Round 2 (I23, intentional change): bVI, bVII, iv, bIII, then iv7, bVImaj7, v, i7.
    CHECK_EQ (padNames (p[3], c), (std::vector<std::string> { "Ab", "Bb", "Fm", "Eb", "Fm7", "Abmaj7", "Gm", "Cm7" }));
    names.clear();
    for (auto& pad : p[3].chords) names.push_back (pad.roman);
    CHECK_EQ (names, (std::vector<std::string> { "bVI", "bVII", "iv", "bIII", "iv7", "bVImaj7", "v", "i7" }));

    names.clear();
    for (auto& pad : p[4].chords) names.push_back (pad.chord.name (c) + " " + pad.roman);
    CHECK_EQ (names, (std::vector<std::string> { "A7 V7/ii", "B7 V7/iii", "C7 V7/IV", "D7 V7/V", "E7 V7/vi" }));

    CHECK_EQ (p[0].chords[0].hint, std::string ("Home base - everything resolves here."));
    CHECK (p[3].chords[0].hint.rfind ("Borrowed from C minor:", 0) == 0);
    CHECK (p[4].chords[0].hint.rfind ("Pulls strongly to Dm (ii)", 0) == 0);
}

TEST_CASE ("palette: A minor contents")
{
    const auto a = key (A, ScaleType::NaturalMinor);
    const auto p = cb::buildPalette (a);

    // Round 2 (I16 / I19, intentional change): VII(add9), VI(add9), VII6, III6, iv9 lead the data; plus i11 and VImaj7#11.
    CHECK_EQ (padNames (p[2], a), (std::vector<std::string> { "Am11", "C6", "Dm9", "Fadd9", "Fmaj7#11", "Gadd9", "G6", "Gsus4" }));
    std::vector<std::string> names;
    for (auto& pad : p[2].chords) names.push_back (pad.roman);
    CHECK_EQ (names, (std::vector<std::string> { "i11", "III6", "iv9", "VIadd9", "VImaj7#11", "VIIadd9", "VII6", "VIIsus4" }));

    CHECK (p[3].chords[0].chord == ch (E, Q::Major));    // the major V comes first in minor
    for (auto c : { ch (D, Q::Major), ch (A, Q::Major), ch (E, Q::Dominant7), ch (B, Q::Minor) })
        CHECK (sectionHas (p[3], c));
    CHECK (! sectionHas (p[3], ch (C, Q::Major)));

    // Round 2 (I19, intentional change): V7b9 of the minor targets iv and v (whose b9 C is in the key).
    names.clear();
    for (auto& pad : p[4].chords) names.push_back (pad.chord.name (a) + " " + pad.roman);
    CHECK_EQ (names, (std::vector<std::string> { "A7 V7/iv", "A7b9 V7b9/iv", "B7 V7/v", "B7b9 V7b9/v", "C7 V7/VI", "D7 V7/VII" }));
    CHECK (p[4].chords[1].hint.rfind ("A darker, dramatic pull into Dm (iv)", 0) == 0);
}

TEST_CASE ("palette: major keys keep plain secondary dominants only")
{
    for (int t = 0; t < 12; ++t)
        for (auto s : { ScaleType::Major, ScaleType::Lydian, ScaleType::Mixolydian })
        {
            const auto pal = cb::buildPalette (key (t, s));   // keep the palette alive for the loop
            for (auto& pad : pal[4].chords)
                CHECK (pad.chord.quality == Q::Dominant7);
        }
}

TEST_CASE ("palette: new extended colours show up where they are diatonic")
{
    const auto c = key (C, ScaleType::Major);
    auto has = [] (const Key& k, int section, const Chord& chord) { return sectionHas (cb::buildPalette (k)[(size_t) section], chord); };

    CHECK (has (key (C, ScaleType::Lydian), 2, ch (C, Q::Major7Sharp11)));       // Lydian's signature tonic
    CHECK (has (key (C, ScaleType::Mixolydian), 2, ch (C, Q::Dominant13)));      // Mixolydian I13
    CHECK (has (key (A, ScaleType::HarmonicMinor), 2, ch (E, Q::Dominant7Flat9)));   // harmonic minor V7b9
    CHECK (has (key (A, ScaleType::Dorian), 2, ch (A, Q::Minor11)));
    CHECK (has (key (A, ScaleType::NaturalMinor), 2, ch (F_, Q::Major7Sharp11)));  // the Lydian VI of natural minor

    // Every key of the two main families gets at least one of the round-2 extended colours.
    for (int t = 0; t < 12; ++t)
        for (auto s : { ScaleType::Major, ScaleType::NaturalMinor, ScaleType::HarmonicMinor, ScaleType::Dorian,
                        ScaleType::Lydian, ScaleType::Mixolydian })
        {
            int extended = 0;
            const auto pal = cb::buildPalette (key (t, s));
            for (auto& pad : pal[2].chords)
                extended += (int) pad.chord.quality >= (int) Q::Minor11 ? 1 : 0;
            CHECK (extended >= 1);
        }

    // Hints for the new chords are friendly sentences naming the chord type.
    const auto p = cb::buildPalette (c);
    for (auto& pad : p[2].chords)
    {
        if (pad.chord.quality == Q::Major7Sharp11) CHECK_EQ (pad.hint, std::string ("Dreamy, floating Lydian maj7#11 - sets up G nicely."));
        if (pad.chord.quality == Q::Dominant13)    CHECK_EQ (pad.hint, std::string ("Rich, soulful 13th - a smooth, jazzy pull back to C."));
        if (pad.chord.quality == Q::Major69)       CHECK_EQ (pad.hint, std::string ("Smooth, open 6/9 - a richer way to land home."));
    }
    const auto ph = cb::buildPalette (key (A, ScaleType::HarmonicMinor));
    for (auto& pad : ph[2].chords)
        if (pad.chord.quality == Q::Dominant7Flat9)
            CHECK_EQ (pad.hint, std::string ("Dark, dramatic 7b9 - the most intense pull back to Am."));
    const auto pn = cb::buildPalette (key (A, ScaleType::NaturalMinor));
    for (auto& pad : pn[2].chords)
        if (pad.chord.quality == Q::Minor11)
            CHECK_EQ (pad.hint, std::string ("Lush, neo-soul m11 - a richer way to land home."));
}

TEST_CASE ("palette: borrowed names are spelled sensibly in sharp keys")
{
    const auto g = key (G, ScaleType::Major);
    const auto p = cb::buildPalette (g);
    std::set<std::string> names;
    for (auto& pad : p[3].chords) names.insert (pad.chord.name (g));
    CHECK (names.count ("Cm") == 1);
    CHECK (names.count ("F") == 1);
    CHECK (names.count ("Eb") == 1);
    CHECK (names.count ("Bb") == 1);
    CHECK (names.count ("D#") == 0);
}

TEST_CASE ("palette: harmonic minor and modes")
{
    const auto ah = key (A, ScaleType::HarmonicMinor);
    const auto p = cb::buildPalette (ah);
    CHECK (! sectionHas (p[3], ch (E, Q::Major)));        // already diatonic
    CHECK (sectionHas (p[3], ch (D, Q::Major)));
    CHECK (! sectionHas (p[4], ch (G, Q::Dominant7)));    // V7/III+ is not offered

    const auto dd = cb::buildPalette (key (D, ScaleType::Dorian));
    CHECK (sectionHas (dd[3], ch (A, Q::Major)));         // the major V
    CHECK (sectionHas (dd[2], ch (G, Q::Dominant9)));     // Dorian IV9

    const auto gm = cb::buildPalette (key (G, ScaleType::Mixolydian));
    CHECK (! sectionHas (gm[3], ch (F_, Q::Major)));      // bVII is diatonic in Mixolydian
    CHECK (sectionHas (gm[3], ch (C, Q::Minor)));
}

//==============================================================================
// Round 2: new qualities (Minor11, Major69, Dominant13, Major7Sharp11, Dominant7Flat9)
//==============================================================================
TEST_CASE ("round2: new quality suffixes and intervals")
{
    CHECK_EQ (std::string (cb::qualitySuffix (Q::Minor11)), std::string ("m11"));
    CHECK_EQ (std::string (cb::qualitySuffix (Q::Major69)), std::string ("6/9"));
    CHECK_EQ (std::string (cb::qualitySuffix (Q::Dominant13)), std::string ("13"));
    CHECK_EQ (std::string (cb::qualitySuffix (Q::Major7Sharp11)), std::string ("maj7#11"));
    CHECK_EQ (std::string (cb::qualitySuffix (Q::Dominant7Flat9)), std::string ("7b9"));
    CHECK_EQ (cb::qualityIntervals (Q::Minor11), (std::vector<int> { 0, 3, 7, 10, 14, 17 }));
    CHECK_EQ (cb::qualityIntervals (Q::Dominant13), (std::vector<int> { 0, 4, 7, 10, 14, 21 }));

    const auto c = key (C, ScaleType::Major);
    CHECK_EQ (ch (D, Q::Minor11).name (c), std::string ("Dm11"));
    CHECK_EQ (ch (C, Q::Major69).name (c), std::string ("C6/9"));
    CHECK_EQ (ch (G, Q::Dominant13).name (c), std::string ("G13"));
    CHECK_EQ (ch (F_, Q::Major7Sharp11).name (c), std::string ("Fmaj7#11"));
    CHECK_EQ (ch (E, Q::Dominant7Flat9).name (key (A, ScaleType::HarmonicMinor)), std::string ("E7b9"));
}

TEST_CASE ("round2: numerals of the new qualities follow the case rules")
{
    const auto c = key (C, ScaleType::Major);
    CHECK_EQ (rn (D, Q::Minor11, c), std::string ("ii11"));
    CHECK_EQ (rn (A, Q::Minor11, c), std::string ("vi11"));
    CHECK_EQ (rn (C, Q::Major69, c), std::string ("I6/9"));
    CHECK_EQ (rn (F_, Q::Major69, c), std::string ("IV6/9"));
    CHECK_EQ (rn (G, Q::Dominant13, c), std::string ("V13"));
    CHECK_EQ (rn (F_, Q::Major7Sharp11, c), std::string ("IVmaj7#11"));
    CHECK_EQ (rn (G, Q::Dominant7Flat9, c), std::string ("V7b9"));
    CHECK_EQ (rn (Ab, Q::Major69, c), std::string ("bVI6/9"));

    const auto a = key (A, ScaleType::NaturalMinor);
    CHECK_EQ (rn (A, Q::Minor11, a), std::string ("i11"));
    CHECK_EQ (rn (D, Q::Minor11, a), std::string ("iv11"));
    CHECK_EQ (rn (F_, Q::Major7Sharp11, a), std::string ("VImaj7#11"));
    CHECK_EQ (rn (E, Q::Dominant7Flat9, a), std::string ("V7b9"));
    CHECK_EQ (rn (E, Q::Dominant13, a), std::string ("V13"));
    CHECK_EQ (rn (E, Q::Dominant7Flat9, key (A, ScaleType::HarmonicMinor)), std::string ("V7b9"));
}

TEST_CASE ("round2: 13 and 7b9 chords behave like dominant 7ths")
{
    const auto c = key (C, ScaleType::Major);
    CHECK_EQ (rn (D, Q::Dominant13, c), std::string ("V13/V"));
    CHECK_EQ (rn (A, Q::Dominant7Flat9, c), std::string ("V7b9/ii"));
    CHECK_EQ (rn (E, Q::Dominant7Flat9, c), std::string ("V7b9/vi"));
    CHECK_EQ (rn (C, Q::Dominant13, c), std::string ("V13/IV"));
    CHECK_EQ (cb::harmonicFunction (ch (D, Q::Dominant13), c), F::Secondary);
    CHECK_EQ (cb::harmonicFunction (ch (A, Q::Dominant7Flat9), c), F::Secondary);
    CHECK_EQ (cb::harmonicFunction (ch (G, Q::Dominant13), c), F::Dominant);       // diatonic
    CHECK_EQ (cb::harmonicFunction (ch (G, Q::Dominant7Flat9), c), F::Dominant);   // the minor-key V7b9 in major
    CHECK_EQ (cb::harmonicFunction (ch (Fs, Q::Dominant7Flat9), c), F::Chromatic); // target vii° is diminished
    CHECK_EQ (cb::romanNumeral (ch (D, Q::Dominant7Flat9), c, false), std::string ("II7b9"));

    const auto a = key (A, ScaleType::NaturalMinor);
    CHECK_EQ (cb::harmonicFunction (ch (E, Q::Dominant7Flat9), a), F::Dominant);
    CHECK_EQ (cb::harmonicFunction (ch (E, Q::Dominant13), a), F::Dominant);
    CHECK_EQ (rn (A, Q::Dominant7Flat9, a), std::string ("V7b9/iv"));
    CHECK_EQ (rn (B, Q::Dominant7Flat9, a), std::string ("V7b9/v"));
    CHECK_EQ (rn (D, Q::Dominant13, a), std::string ("V13/VII"));

    for (int t = 0; t < 12; ++t)
        for (auto s : { ScaleType::NaturalMinor, ScaleType::Dorian, ScaleType::Phrygian, ScaleType::HarmonicMinor })
        {
            CHECK_EQ (cb::harmonicFunction (ch (t + 7, Q::Dominant13), key (t, s)), F::Dominant);
            CHECK_EQ (cb::harmonicFunction (ch (t + 7, Q::Dominant7Flat9), key (t, s)), F::Dominant);
        }
}

TEST_CASE ("round2: every quality is handled by numerals, names and functions in every key")
{
    for (auto& k : allKeys())
        for (int r = 0; r < 12; ++r)
            for (int q = 0; q < cb::numChordQualities; ++q)
            {
                const Chord c { r, (ChordQuality) q };
                const auto numeral = cb::romanNumeral (c, k);
                const auto plain = cb::romanNumeral (c, k, false);
                REQUIRE (! numeral.empty());
                REQUIRE (! plain.empty());
                CHECK (! looksSecondary (plain));
                CHECK (c.name (k).size() >= 1);
                const auto names = cb::chordToneNames (c, k);
                CHECK_EQ (names.size(), (size_t) c.numTones());
                const auto f = cb::harmonicFunction (c, k);
                CHECK ((int) f >= 0 && (int) f <= (int) F::Chromatic);
            }
}

//==============================================================================
// Round 2: B13 - harmonic / melodic minor (and the other altered minor scales) borrow from natural minor
//==============================================================================
TEST_CASE ("b13: harmonic minor labels C, G and Em as borrowed natural-minor chords")
{
    const auto a = key (A, ScaleType::HarmonicMinor);
    CHECK_EQ (rn (C, Q::Major, a), std::string ("III"));
    CHECK_EQ (rn (G, Q::Major, a), std::string ("VII"));       // natural-minor numeral (beside G#dim = vii°)
    CHECK_EQ (rn (E, Q::Minor, a), std::string ("v"));
    CHECK_EQ (rn (G, Q::Dominant7, a), std::string ("VII7"));
    CHECK_EQ (rn (A, Q::Minor7, a), std::string ("i7"));
    CHECK_EQ (rn (C, Q::Major7, a), std::string ("IIImaj7"));
    for (auto c : { ch (C, Q::Major), ch (G, Q::Major), ch (E, Q::Minor), ch (G, Q::Dominant7), ch (A, Q::Minor7),
                    ch (C, Q::Major7), ch (E, Q::Minor7) })
        CHECK_EQ (cb::harmonicFunction (c, a), F::Borrowed);

    // The progression Am C G F reads the same in natural and harmonic minor.
    const auto an = key (A, ScaleType::NaturalMinor);
    for (auto c : { ch (A, Q::Minor), ch (C, Q::Major), ch (G, Q::Major), ch (F_, Q::Major) })
        CHECK_EQ (cb::romanNumeral (c, a), cb::romanNumeral (c, an));

    // Real secondary dominants still read as such.
    CHECK_EQ (rn (C, Q::Dominant7, a), std::string ("V7/VI"));
    CHECK_EQ (rn (A, Q::Dominant7, a), std::string ("V7/iv"));
    CHECK_EQ (rn (B, Q::Dominant7, a), std::string ("V7/V"));
}

TEST_CASE ("b13: augmented III+ is never a secondary-dominant target")
{
    for (int t = 0; t < 12; ++t)
        for (auto s : { ScaleType::HarmonicMinor, ScaleType::MelodicMinor })
        {
            const auto k = key (t, s);
            const int augRoot = k.pitchClasses()[2];
            for (auto q : { Q::Major, Q::Dominant7, Q::Dominant9, Q::Dominant13, Q::Dominant7Flat9 })
            {
                const Chord v { augRoot + 7, q };
                CHECK (cb::harmonicFunction (v, k) != F::Secondary);
                CHECK (! looksSecondary (cb::romanNumeral (v, k)));
            }
        }
}

TEST_CASE ("b13: melodic minor, dorian and phrygian borrow from natural minor")
{
    const auto m = key (A, ScaleType::MelodicMinor);
    CHECK_EQ (rn (F_, Q::Major, m), std::string ("VI"));
    CHECK_EQ (rn (G, Q::Major, m), std::string ("VII"));
    CHECK_EQ (rn (D, Q::Minor, m), std::string ("iv"));
    CHECK_EQ (rn (C, Q::Major, m), std::string ("III"));
    for (auto c : { ch (F_, Q::Major), ch (G, Q::Major), ch (D, Q::Minor), ch (C, Q::Major), ch (E, Q::Minor) })
        CHECK_EQ (cb::harmonicFunction (c, m), F::Borrowed);

    const auto d = key (D, ScaleType::Dorian);
    CHECK_EQ (cb::harmonicFunction (ch (Bb, Q::Major), d), F::Borrowed);   // was Chromatic
    CHECK_EQ (rn (Bb, Q::Major, d), std::string ("bVI"));                   // modes keep mode-relative accidentals
    CHECK_EQ (cb::harmonicFunction (ch (G, Q::Minor), d), F::Borrowed);

    const auto e = key (E, ScaleType::Phrygian);
    CHECK_EQ (cb::harmonicFunction (ch (B, Q::Minor), e), F::Borrowed);
    CHECK_EQ (cb::harmonicFunction (ch (D, Q::Major), e), F::Borrowed);      // the natural-minor VII
    CHECK_EQ (rn (D, Q::Major, e), std::string ("VII"));

    // Natural Minor itself and the major family are unchanged: natural-minor-only chords are not borrowed in A minor.
    CHECK_EQ (cb::harmonicFunction (ch (Bb, Q::Major), key (A, ScaleType::NaturalMinor)), F::Chromatic);
}

TEST_CASE ("b13: harmonic and melodic minor palettes offer the natural-minor III, VII and v")
{
    const auto a = key (A, ScaleType::HarmonicMinor);
    const auto p = cb::buildPalette (a);
    CHECK_EQ (padNames (p[3], a), (std::vector<std::string> { "G", "C", "Am7", "Em", "Em7", "Cmaj7", "D", "A" }));
    CHECK (p[3].chords[0].hint.rfind ("Borrowed from A natural minor:", 0) == 0);
    CHECK (p[3].chords[6].hint.rfind ("Borrowed from A major:", 0) == 0);
    for (auto& pad : p[3].chords)
        CHECK_EQ (pad.function, F::Borrowed);

    const auto m = key (A, ScaleType::MelodicMinor);
    const auto pm = cb::buildPalette (m);
    for (auto c : { ch (G, Q::Major), ch (F_, Q::Major), ch (C, Q::Major), ch (E, Q::Minor), ch (D, Q::Minor) })
        CHECK (sectionHas (pm[3], c));

    // Every harmonic / melodic minor key: VII and III of the natural minor are one click away.
    for (int t = 0; t < 12; ++t)
        for (auto s : { ScaleType::HarmonicMinor, ScaleType::MelodicMinor })
        {
            const auto k = key (t, s);
            const auto pal = cb::buildPalette (k);
            CHECK (sectionHas (pal[3], ch (cb::pc (t + 10), Q::Major)));
            CHECK (sectionHas (pal[3], ch (cb::pc (t + 3), Q::Major)));
            CHECK (sectionHas (pal[3], ch (cb::pc (t + 7), Q::Minor)));
        }

    // Dorian gets the natural-minor bVI next to its major V.
    const auto dp = cb::buildPalette (key (D, ScaleType::Dorian));
    CHECK (sectionHas (dp[3], ch (Bb, Q::Major)));
    CHECK (dp[3].chords[0].chord == ch (A, Q::Major));
}

//==============================================================================
// Round 2: B12 - chord-relative spelling
//==============================================================================
TEST_CASE ("b12: secondary dominants spell their raised tones with sharps")
{
    const auto c = key (C, ScaleType::Major);
    using V = std::vector<std::string>;
    CHECK_EQ (cb::chordToneNames (ch (D, Q::Dominant7), c), (V { "D", "F#", "A", "C" }));
    CHECK_EQ (cb::chordToneNames (ch (E, Q::Dominant7), c), (V { "E", "G#", "B", "D" }));
    CHECK_EQ (cb::chordToneNames (ch (B, Q::Dominant7), c), (V { "B", "D#", "F#", "A" }));
    CHECK_EQ (cb::chordToneNames (ch (A, Q::Dominant7), c), (V { "A", "C#", "E", "G" }));
    CHECK_EQ (cb::chordToneNames (ch (C, Q::Dominant7), c), (V { "C", "E", "G", "Bb" }));
    CHECK_EQ (cb::chordToneNames (ch (C, Q::Dominant7), key (A, ScaleType::NaturalMinor)), (V { "C", "E", "G", "Bb" }));
    CHECK_EQ (cb::chordToneNames (ch (B, Q::Dominant7), key (G, ScaleType::Major)), (V { "B", "D#", "F#", "A" }));
    CHECK_EQ (cb::chordToneNames (ch (A, Q::Dominant7Flat9), key (A, ScaleType::NaturalMinor)), (V { "A", "C#", "E", "G", "Bb" }));

    // Slash-bass names (ProgressionLane): D7/F#, E7/G#.
    CHECK_EQ (cb::spellChordTone (ch (D, Q::Dominant7), 4, c), std::string ("F#"));
    CHECK_EQ (cb::spellChordTone (ch (E, Q::Dominant7), 4, c), std::string ("G#"));
}

TEST_CASE ("b12: chord-relative spelling of borrowed, altered and extended chords")
{
    const auto c = key (C, ScaleType::Major);
    using V = std::vector<std::string>;
    CHECK_EQ (cb::chordToneNames (ch (Ab, Q::Major), c), (V { "Ab", "C", "Eb" }));
    CHECK_EQ (cb::chordToneNames (ch (F_, Q::Minor), c), (V { "F", "Ab", "C" }));
    CHECK_EQ (cb::chordToneNames (ch (B, Q::Diminished7), c), (V { "B", "D", "F", "Ab" }));
    CHECK_EQ (cb::chordToneNames (ch (C, Q::Augmented), c), (V { "C", "E", "G#" }));
    CHECK_EQ (cb::chordToneNames (ch (C, Q::Major7Sharp11), c), (V { "C", "E", "G", "B", "F#" }));   // #11, not Gb
    CHECK_EQ (cb::chordToneNames (ch (F_, Q::Major7Sharp11), c), (V { "F", "A", "C", "E", "B" }));
    CHECK_EQ (cb::chordToneNames (ch (D, Q::Minor11), c), (V { "D", "F", "A", "C", "E", "G" }));
    CHECK_EQ (cb::chordToneNames (ch (G, Q::Dominant13), c), (V { "G", "B", "D", "F", "A", "E" }));
    CHECK_EQ (cb::chordToneNames (ch (C, Q::Major69), c), (V { "C", "E", "G", "A", "D" }));
    CHECK_EQ (cb::chordToneNames (ch (G, Q::Dominant7Flat9), c), (V { "G", "B", "D", "F", "Ab" }));
    CHECK_EQ (cb::chordToneNames (ch (Fs, Q::HalfDiminished7), c), (V { "F#", "A", "C", "E" }));
    CHECK_EQ (cb::chordToneNames (ch (Ab, Q::Minor), key (Eb, ScaleType::Major)), (V { "Ab", "Cb", "Eb" }));
    CHECK_EQ (cb::chordToneNames (ch (Gs, Q::Diminished7), key (A, ScaleType::HarmonicMinor)), (V { "G#", "B", "D", "F" }));
}

TEST_CASE ("b12: chord roots outside key and parallel key follow their numeral")
{
    const auto c = key (C, ScaleType::Major);
    CHECK_EQ (ch (Fs, Q::Diminished).name (c), std::string ("F#dim"));          // #iv°
    CHECK_EQ (ch (Fs, Q::HalfDiminished7).name (c), std::string ("F#m7b5"));     // was "Gbm7b5"
    CHECK_EQ (ch (Cs, Q::Diminished7).name (c), std::string ("C#dim7"));         // #i°7
    CHECK_EQ (ch (Db, Q::Major).name (c), std::string ("Db"));                   // bII
    CHECK_EQ (ch (Gb, Q::Major).name (c), std::string ("Gb"));                   // bV
    CHECK_EQ (ch (Bb, Q::Major).name (key (A, ScaleType::NaturalMinor)), std::string ("Bb"));   // Neapolitan bII, was "A#"
    CHECK_EQ (ch (Ab, Q::Major).name (key (G, ScaleType::Major)), std::string ("Ab"));          // bII, was "G#"
    CHECK_EQ (ch (G, Q::Major).name (key (A, ScaleType::HarmonicMinor)), std::string ("G"));
    CHECK_EQ (ch (Bb, Q::Major).name (key (D, ScaleType::Dorian)), std::string ("Bb"));
    // Unchanged: roots in the key or its parallel key keep Key::spell.
    CHECK_EQ (ch (Eb, Q::Major).name (key (G, ScaleType::Major)), std::string ("Eb"));
    CHECK_EQ (ch (Cs, Q::Minor).name (key (A, ScaleType::NaturalMinor)), std::string ("C#m"));

    for (auto& k : allKeys())
        for (int r = 0; r < 12; ++r)
            for (int q = 0; q < cb::numChordQualities; ++q)
            {
                const Chord ch2 { r, (ChordQuality) q };
                const auto tones = cb::chordToneNames (ch2, k);
                REQUIRE (! tones.empty());
                CHECK_EQ (ch2.name (k).rfind (tones[0], 0), (size_t) 0);   // the name starts with the spelled root
                for (auto& t : tones)
                {
                    CHECK (t.size() == 1 || t.size() == 2);                 // never a double accidental
                    CHECK (t.find ("##") == std::string::npos && t.find ("bb") == std::string::npos);
                }
            }
}

TEST_CASE ("b12: palette chords spell as stacked letters with single accidentals")
{
    for (auto& k : allKeys())
    {
        // Keys whose own scale needs a double accidental (G# Harmonic Minor ...) fall back to plain names.
        bool regular = true;
        for (int p : k.pitchClasses())
            regular = regular && k.spell (p).size() <= 2;
        std::set<char> scaleLetters;
        for (int p : k.pitchClasses())
            scaleLetters.insert (k.spell (p)[0]);
        regular = regular && scaleLetters.size() == 7;

        const auto pal = cb::buildPalette (k);
        for (auto& s : pal)
            for (auto& pad : s.chords)
            {
                const auto tones = cb::chordToneNames (pad.chord, k);
                const auto pcs = pad.chord.pitchClasses();

                // Triad tones (root, 3rd/sus, 5th) take distinct letters: no "C Eb D#" style spellings.
                std::set<char> triadLetters { tones[0][0], tones[1][0], tones[2][0] };
                CHECK_EQ (triadLetters.size(), (size_t) 3);

                // Diatonic chords of regular keys spell exactly like the key does.
                if (regular && s.id != cb::PaletteSectionId::Borrowed && s.id != cb::PaletteSectionId::Secondary)
                    for (size_t i = 0; i < pcs.size(); ++i)
                        CHECK_EQ (tones[i], k.spell (pcs[i]));
            }
    }
}

TEST_CASE ("spelling: the raised leading tone of a minor key keeps its 7th-degree letter")
{
    using V = std::vector<std::string>;
    const auto cSharpMinor = key (Cs, ScaleType::NaturalMinor);
    CHECK_EQ (ch (C, Q::Diminished).name (cSharpMinor), std::string ("B#dim"));   // was "Cdim" = C Eb Gb
    CHECK_EQ (cb::chordToneNames (ch (C, Q::Diminished), cSharpMinor), (V { "B#", "D#", "F#" }));
    CHECK_EQ (cb::chordToneNames (ch (F_, Q::Diminished), key (Fs, ScaleType::NaturalMinor)), (V { "E#", "G#", "B" }));
    CHECK_EQ (cb::chordToneNames (ch (Gs, Q::Dominant7), cSharpMinor), (V { "G#", "B#", "D#", "F#" }));
    CHECK_EQ (ch (C, Q::Diminished7).name (key (Cs, ScaleType::HarmonicMinor)), std::string ("B#dim7"));
    // Single out-of-key notes still avoid E# / B# (the keyboard guide's note names), and so do chords that would need
    // a double accidental on the leading-tone letter (E# major = E# G## B#).
    CHECK_EQ (cSharpMinor.spell (C), std::string ("C"));
    CHECK_EQ (ch (F_, Q::Major).name (key (Fs, ScaleType::NaturalMinor)), std::string ("F"));
    CHECK_EQ (ch (C, Q::Major).name (cSharpMinor), std::string ("C"));
    CHECK_EQ (ch (C, Q::Minor7).name (cSharpMinor), std::string ("Cm7"));
    CHECK_EQ (ch (F_, Q::HalfDiminished7).name (key (Fs, ScaleType::NaturalMinor)), std::string ("E#m7b5"));
    // Flat minor keys are unchanged.
    CHECK_EQ (ch (D, Q::Diminished).name (key (Eb, ScaleType::NaturalMinor)), std::string ("Ddim"));
    CHECK_EQ (ch (E, Q::Diminished).name (key (F_, ScaleType::NaturalMinor)), std::string ("Edim"));

    // Every minor-family key: the vii° chord on the raised leading tone spells the same in natural and harmonic minor
    // whenever its letter needs only a single accidental, and then its tones stack on letters a third apart.
    static const char letters[] = { 'C', 'D', 'E', 'F', 'G', 'A', 'B' };
    for (int t = 0; t < 12; ++t)
    {
        const auto nat = key (t, ScaleType::NaturalMinor);
        const auto harm = key (t, ScaleType::HarmonicMinor);
        const Chord vii = ch (t + 11, Q::Diminished);
        const char tonicLetter = nat.tonicName()[0];
        int li = 0;
        while (letters[li] != tonicLetter)
            ++li;
        const char expected = letters[(li + 6) % 7];
        const auto harmName = vii.name (harm);
        if (harmName[0] != expected)
            continue;   // would need a double accidental (G# minor's F##)
        CHECK_EQ (vii.name (nat), harmName);
        const auto tones = cb::chordToneNames (vii, nat);
        REQUIRE (tones.size() == 3);
        CHECK_EQ (tones[0][0], expected);
        CHECK_EQ (tones[1][0], letters[(li + 6 + 2) % 7]);
        CHECK_EQ (tones[2][0], letters[(li + 6 + 4) % 7]);
    }
}

//==============================================================================
// Suggestions
//==============================================================================
TEST_CASE ("suggest: zero for the same chord, range 0..1 everywhere")
{
    for (auto& k : allKeys())
    {
        const auto p = cb::buildPalette (k);
        std::vector<Chord> chords;
        for (auto& s : p)
            for (auto& pad : s.chords)
                chords.push_back (pad.chord);

        for (auto& from : chords)
            for (auto& to : chords)
            {
                const float v = cb::transitionScore (from, to, k);
                CHECK (v >= 0.0f && v <= 1.0f);
                if (from == to)
                    CHECK_EQ (v, 0.0f);
            }
    }
    CHECK_EQ (cb::transitionScore (ch (C + 12, Q::Major), ch (C, Q::Major), key (C, ScaleType::Major)), 0.0f);
}

TEST_CASE ("suggest: from V the tonic is the best next chord")
{
    const auto c = key (C, ScaleType::Major);
    CHECK (bestNext (ch (G, Q::Major), c) == ch (C, Q::Major));
    CHECK (bestNext (ch (G, Q::Dominant7), c) == ch (C, Q::Major));
    CHECK (cb::transitionScore (ch (G, Q::Major), ch (C, Q::Major), c) >= 0.9f);

    for (int t = 0; t < 12; ++t)
        for (auto s : { ScaleType::Major, ScaleType::HarmonicMinor, ScaleType::MelodicMinor, ScaleType::Lydian })
        {
            const auto k = key (t, s);
            CHECK (bestNext (cb::diatonicChord (k, 4, false), k) == cb::diatonicChord (k, 0, false));
        }

    const auto a = key (A, ScaleType::NaturalMinor);
    CHECK (bestNext (ch (E, Q::Major), a) == ch (A, Q::Minor));
}

TEST_CASE ("suggest: from ii the V is the best next chord")
{
    for (int t = 0; t < 12; ++t)
    {
        const auto k = key (t, ScaleType::Major);
        CHECK (bestNext (cb::diatonicChord (k, 1, false), k) == cb::diatonicChord (k, 4, false));
        CHECK (bestNext (cb::diatonicChord (k, 1, true), k) == cb::diatonicChord (k, 4, false));
    }
}

TEST_CASE ("suggest: secondary dominants resolve to their target")
{
    const auto c = key (C, ScaleType::Major);
    CHECK (cb::transitionScore (ch (D, Q::Dominant7), ch (G, Q::Major), c) >= 0.95f);
    CHECK (bestNext (ch (D, Q::Dominant7), c) == ch (G, Q::Major));
    CHECK (bestNext (ch (A, Q::Dominant7), c) == ch (D, Q::Minor));
    CHECK (bestNext (ch (E, Q::Dominant7), c) == ch (A, Q::Minor));

    for (auto& k : allKeys())
    {
        const auto pal = cb::buildPalette (k);   // round 2: was a range-for over a destroyed temporary
        for (auto& pad : pal[4].chords)
        {
            const auto target = cb::diatonicChord (k, k.degreeOf (pad.chord.root + 5), false);
            CHECK (cb::transitionScore (pad.chord, target, k) >= 0.9f);
        }
    }
}

TEST_CASE ("suggest: borrowed moves")
{
    const auto c = key (C, ScaleType::Major);
    const Chord bVI = ch (Ab, Q::Major), bVII = ch (Bb, Q::Major);
    CHECK (cb::transitionScore (bVI, bVII, c) >= 0.75f);
    CHECK (bestNext (bVI, c) == bVII);
    CHECK (cb::transitionScore (bVII, ch (C, Q::Major), c) >= 0.75f);
    CHECK (cb::transitionScore (ch (F_, Q::Minor), ch (C, Q::Major), c) >= 0.75f);
    CHECK (cb::transitionScore (ch (F_, Q::Major), ch (F_, Q::Minor), c) >= 0.7f);   // IV -> iv
    CHECK (cb::transitionScore (bVI, ch (G, Q::Major), c) >= 0.6f);
}

TEST_CASE ("suggest: from IV both V and I score high")
{
    const auto c = key (C, ScaleType::Major);
    const Chord iv = ch (F_, Q::Major);
    const float toV = cb::transitionScore (iv, ch (G, Q::Major), c);
    const float toI = cb::transitionScore (iv, ch (C, Q::Major), c);
    CHECK (toV >= 0.7f);
    CHECK (toI >= 0.7f);
    for (int d : { 1, 2, 5, 6 })
    {
        CHECK (toV > cb::transitionScore (iv, cb::diatonicChord (c, d, false), c));
        CHECK (toI > cb::transitionScore (iv, cb::diatonicChord (c, d, false), c));
    }
}

TEST_CASE ("suggest: common pop motions beat weak ones")
{
    const auto c = key (C, ScaleType::Major);
    const Chord I = ch (C, Q::Major), ii = ch (D, Q::Minor), iii = ch (E, Q::Minor), IV = ch (F_, Q::Major),
                V = ch (G, Q::Major), vi = ch (A, Q::Minor), vii = ch (B, Q::Diminished);
    CHECK (cb::transitionScore (I, IV, c) > cb::transitionScore (I, iii, c));
    CHECK (cb::transitionScore (I, vi, c) > cb::transitionScore (I, vii, c));
    CHECK (cb::transitionScore (vi, IV, c) > cb::transitionScore (vi, iii, c));
    CHECK (cb::transitionScore (iii, vi, c) > cb::transitionScore (iii, V, c));
    CHECK (cb::transitionScore (V, vi, c) > cb::transitionScore (V, ii, c));   // deceptive cadence
    CHECK (cb::transitionScore (vii, I, c) > cb::transitionScore (vii, IV, c));
    CHECK (cb::transitionScore (ii, V, c) > cb::transitionScore (ii, I, c));

    const auto a = key (A, ScaleType::NaturalMinor);
    CHECK (cb::transitionScore (ch (F_, Q::Major), ch (G, Q::Major), a) >= 0.75f);   // VI -> VII
    CHECK (cb::transitionScore (ch (A, Q::Minor), ch (F_, Q::Major), a) >= 0.75f);   // i -> VI
    CHECK (cb::transitionScore (ch (D, Q::Minor), ch (E, Q::Major), a) >= 0.6f);     // iv -> V
}

TEST_CASE ("suggest: scores spread across the range")
{
    for (auto& k : allKeys())
    {
        // Major / minor keys: from every triad a few strong picks, several middling, the rest low.
        // Modes: the best pick may sit lower (e.g. from Dorian's vi°), but there is always something worth playing.
        const bool mainKey = k.scale == ScaleType::Major || k.scale == ScaleType::NaturalMinor
                          || k.scale == ScaleType::HarmonicMinor;
        const auto p = cb::buildPalette (k);
        for (auto& fromPad : p[0].chords)
        {
            int high = 0, mid = 0, low = 0;
            float best = 0.0f;
            for (auto& s : p)
                for (auto& pad : s.chords)
                {
                    if (pad.chord == fromPad.chord) continue;
                    const float v = cb::transitionScore (fromPad.chord, pad.chord, k);
                    best = std::max (best, v);
                    if (v >= 0.75f) ++high;
                    else if (v >= 0.4f) ++mid;
                    else ++low;
                }
            CHECK (high <= 12);
            CHECK (low >= 4);
            CHECK (best >= 0.55f);
            if (mainKey)
            {
                CHECK (high >= 1);
                CHECK (mid >= 3);
            }
        }
    }

    // C major from the tonic: the classic I -> IV / V / vi trio leads.
    const auto c = key (C, ScaleType::Major);
    const Chord I = ch (C, Q::Major);
    for (auto to : { ch (F_, Q::Major), ch (G, Q::Major), ch (A, Q::Minor) })
        CHECK (cb::transitionScore (I, to, c) >= 0.75f);
    for (auto to : { ch (E, Q::Minor), ch (B, Q::Diminished), ch (C, Q::Major7), ch (Eb, Q::Major) })
        CHECK (cb::transitionScore (I, to, c) < 0.45f);
}

TEST_CASE ("suggest: deterministic and suggestionsFor matches the palette shape")
{
    const auto k = key (D, ScaleType::Major);
    const auto p = cb::buildPalette (k);
    const Chord from = ch (B, Q::Minor);
    const auto s1 = cb::suggestionsFor (from, p, k);
    const auto s2 = cb::suggestionsFor (from, p, k);
    REQUIRE (s1.size() == p.size());
    for (size_t i = 0; i < p.size(); ++i)
    {
        REQUIRE (s1[i].size() == p[i].chords.size());
        for (size_t j = 0; j < p[i].chords.size(); ++j)
        {
            CHECK_EQ (s1[i][j], s2[i][j]);
            CHECK_EQ (s1[i][j], cb::transitionScore (from, p[i].chords[j].chord, k));
        }
    }
}

TEST_CASE ("suggest: robust to odd input")
{
    const auto c = key (C, ScaleType::Major);
    for (int r = -14; r < 26; r += 3)
        for (int q = 0; q < cb::numChordQualities; ++q)
            for (int q2 = 0; q2 < cb::numChordQualities; q2 += 3)
            {
                const float v = cb::transitionScore ({ r, (ChordQuality) q }, { r + 5, (ChordQuality) q2 }, c);
                CHECK (v >= 0.0f && v <= 1.0f);
            }
    CHECK (cb::suggestionsFor (ch (C, Q::Major), {}, c).empty());
}
