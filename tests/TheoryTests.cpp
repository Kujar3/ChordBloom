#include "TestFramework.h"

#include "core/Palette.h"
#include "core/Suggest.h"
#include "core/Theory.h"

#include <algorithm>
#include <set>

// Tests for Theory / Palette / Suggest (owner: core-theory agent)

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
                const bool looksSecondary = numeral.find ('/') != std::string::npos;
                CHECK_EQ (looksSecondary, cb::harmonicFunction (c, k) == F::Secondary);
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

TEST_CASE ("palette: borrowed pads come from the parallel key only")
{
    for (auto& k : allKeys())
    {
        const auto p = cb::buildPalette (k);
        for (auto& pad : p[3].chords)
        {
            CHECK (! cb::isDiatonic (pad.chord, k));
            CHECK (cb::isDiatonic (pad.chord, k.parallel()));
            CHECK (pad.function == F::Borrowed || pad.function == F::Dominant);
        }
    }
}

TEST_CASE ("palette: secondary pads are V7 of diatonic targets in degree order")
{
    for (auto& k : allKeys())
    {
        const auto p = cb::buildPalette (k);
        int lastTarget = 0;
        for (auto& pad : p[4].chords)
        {
            CHECK (pad.chord.quality == Q::Dominant7);
            CHECK_EQ (pad.function, F::Secondary);
            const int target = k.degreeOf (pad.chord.root + 5);
            CHECK (target > lastTarget);
            lastTarget = target;
            const auto tq = cb::diatonicChord (k, target, false).quality;
            CHECK (tq != Q::Diminished);
            CHECK (pad.roman.rfind ("V7/", 0) == 0);
            for (int s = 0; s < 4; ++s)
                CHECK (! sectionHas (p[(size_t) s], pad.chord));
        }
        CHECK (p[4].chords.size() <= 6);
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

    for (auto c9 : { ch (C, Q::Add9), ch (C, Q::Sus2), ch (C, Q::Sus4), ch (D, Q::Minor9), ch (F_, Q::Major9),
                     ch (G, Q::Dominant7Sus4), ch (A, Q::Minor9) })
        CHECK (sectionHas (p[2], c9));
    CHECK_EQ (p[2].chords.size(), (size_t) 8);

    CHECK (p[3].chords[0].chord == ch (F_, Q::Minor));   // iv first: the most-used borrowed chord
    for (auto b : { ch (Bb, Q::Major), ch (Ab, Q::Major), ch (Eb, Q::Major), ch (G, Q::Minor), ch (F_, Q::Minor7), ch (Ab, Q::Major7) })
        CHECK (sectionHas (p[3], b));

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

    for (auto c : { ch (A, Q::MinorAdd9), ch (A, Q::Sus2), ch (A, Q::Sus4), ch (D, Q::Minor9), ch (F_, Q::Major9) })
        CHECK (sectionHas (p[2], c));

    CHECK (p[3].chords[0].chord == ch (E, Q::Major));    // the major V comes first in minor
    for (auto c : { ch (D, Q::Major), ch (A, Q::Major), ch (E, Q::Dominant7), ch (B, Q::Minor) })
        CHECK (sectionHas (p[3], c));
    CHECK (! sectionHas (p[3], ch (C, Q::Major)));

    std::vector<std::string> names;
    for (auto& pad : p[4].chords) names.push_back (pad.chord.name (a) + " " + pad.roman);
    CHECK_EQ (names, (std::vector<std::string> { "A7 V7/iv", "B7 V7/v", "C7 V7/VI", "D7 V7/VII" }));   // G7 is VII7
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
        for (auto& pad : cb::buildPalette (k)[4].chords)
        {
            const auto target = cb::diatonicChord (k, k.degreeOf (pad.chord.root + 5), false);
            CHECK (cb::transitionScore (pad.chord, target, k) >= 0.9f);
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
