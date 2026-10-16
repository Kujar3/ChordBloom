#include "TestFramework.h"

#include "core/Generate.h"
#include "core/Suggest.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

// Tests for Generate (presets + spark) and Suggest calibration (owner: core-suggest agent)

namespace
{
using cb::Chord;
using cb::Key;
using cb::Mood;
using cb::ScaleType;
using Q = cb::ChordQuality;

Chord ch (int root, Q q) { return { cb::pc (root), q }; }

std::vector<Key> allKeys()
{
    std::vector<Key> keys;
    for (int s = 0; s < cb::numScaleTypes; ++s)
        for (int t = 0; t < 12; ++t)
            keys.push_back ({ t, (ScaleType) s });
    return keys;
}

std::vector<Chord> paletteChords (const std::vector<cb::PaletteSection>& p)
{
    std::vector<Chord> out;
    for (auto& s : p)
        for (auto& pad : s.chords)
            if (std::find (out.begin(), out.end(), pad.chord) == out.end())
                out.push_back (pad.chord);
    return out;
}

bool contains (const std::vector<Chord>& v, const Chord& c) { return std::find (v.begin(), v.end(), c) != v.end(); }

// Distinct destination roots from `from`, best first (each root scored by its best pad in the palette).
std::vector<int> rankedRoots (const Chord& from, const Key& k)
{
    std::vector<std::pair<float, int>> best;   // (score, root)
    for (auto& c : paletteChords (cb::buildPalette (k)))
    {
        if (c.root == cb::pc (from.root))
            continue;
        const float v = cb::transitionScore (from, c, k);
        auto it = std::find_if (best.begin(), best.end(), [&] (const std::pair<float, int>& b) { return b.second == c.root; });
        if (it == best.end())
            best.push_back ({ v, c.root });
        else
            it->first = std::max (it->first, v);
    }
    std::stable_sort (best.begin(), best.end(), [] (const std::pair<float, int>& a, const std::pair<float, int>& b) { return a.first > b.first; });

    std::vector<int> roots;
    for (auto& b : best)
        roots.push_back (b.second);
    return roots;
}

int topRoot (const Chord& from, const Key& k)
{
    const auto r = rankedRoots (from, k);
    return r.empty() ? -1 : r[0];
}

bool topTwoAre (const Chord& from, const Key& k, int rootA, int rootB)
{
    const auto r = rankedRoots (from, k);
    if (r.size() < 2)
        return false;
    const std::set<int> got { r[0], r[1] }, want { cb::pc (rootA), cb::pc (rootB) };
    return got == want;
}

int offsetOf (const Chord& c, const Key& k) { return cb::pc (c.root - k.tonic); }

bool isJazzy (Q q)
{
    switch (q)
    {
        case Q::Major7: case Q::Minor7: case Q::Dominant7: case Q::HalfDiminished7: case Q::Major9: case Q::Minor9:
        case Q::Dominant9: case Q::Minor11: case Q::Dominant13: case Q::Major7Sharp11: case Q::Dominant7Flat9:
        case Q::MinorMajor7: case Q::Major6: case Q::Minor6: case Q::Major69: case Q::Dominant7Sus4: case Q::Diminished7:
            return true;
        default:
            return false;
    }
}

bool isDreamyColour (Q q)
{
    switch (q)
    {
        case Q::Major7: case Q::Major9: case Q::Add9: case Q::Major6: case Q::Major69: case Q::Major7Sharp11:
        case Q::Minor7: case Q::Minor9: case Q::MinorAdd9: case Q::Minor11: case Q::Sus2: case Q::Minor6:
            return true;
        default:
            return false;
    }
}

cb::SparkSettings spark (Mood m, int n = 4, int beats = 4)
{
    cb::SparkSettings s;
    s.mood = m;
    s.numChords = n;
    s.beatsPerChord = beats;
    return s;
}

std::string describe (const cb::Progression& p, const Key& k)
{
    std::string s;
    for (auto& slot : p.slots)
        s += slot.chord.name (k) + " ";
    return s;
}
} // namespace

//==============================================================================
// Suggest calibration (out/midi_stats.md)
//==============================================================================
TEST_CASE ("suggest calibration: minor keys follow the data")
{
    for (int t = 0; t < 12; ++t)
    {
        const Key k { t, ScaleType::NaturalMinor };
        const Chord i = cb::diatonicChord (k, 0, false), III = cb::diatonicChord (k, 2, false),
                    iv = cb::diatonicChord (k, 3, false), v = cb::diatonicChord (k, 4, false),
                    VI = cb::diatonicChord (k, 5, false), VII = cb::diatonicChord (k, 6, false);

        // i -> VI / VII first, then III; the modal v ahead of iv (data: VI 20%, VII 17%, III 12%, v 8%, iv 4%)
        CHECK (topTwoAre (i, k, VI.root, VII.root));
        const auto fromI = rankedRoots (i, k);
        REQUIRE (fromI.size() >= 3);
        CHECK_EQ (fromI[2], III.root);
        CHECK (cb::transitionScore (i, v, k) > cb::transitionScore (i, iv, k));

        CHECK_EQ (topRoot (VII, k), i.root);              // VII -> i 40%
        CHECK_EQ (topRoot (VI, k), VII.root);             // VI -> VII 22%
        CHECK_EQ (topRoot (III, k), VI.root);             // III -> VI 30%
        CHECK (topTwoAre (v, k, VI.root, i.root));        // v -> VI 32% / i 27%
        CHECK_EQ (topRoot (iv, k), i.root);               // iv -> i 29%
        CHECK_EQ (rankedRoots (iv, k)[1], VI.root);
        CHECK_EQ (topRoot (ch (t + 7, Q::Major), k), i.root);   // V -> i 55%

        CHECK (cb::transitionScore (VII, i, k) >= 0.75f);
        CHECK (cb::transitionScore (VI, VII, k) >= 0.75f);
        CHECK (cb::transitionScore (III, VI, k) >= 0.75f);
    }
}

TEST_CASE ("suggest calibration: major keys follow the data")
{
    for (int t = 0; t < 12; ++t)
    {
        const Key k { t, ScaleType::Major };
        const Chord I = cb::diatonicChord (k, 0, false), IV = cb::diatonicChord (k, 3, false),
                    V = cb::diatonicChord (k, 4, false), vi = cb::diatonicChord (k, 5, false);

        const auto fromI = rankedRoots (I, k);
        REQUIRE (fromI.size() >= 3);
        CHECK_EQ (fromI[0], V.root);                       // I -> V 29%
        CHECK (std::set<int> (fromI.begin(), fromI.begin() + 3) == (std::set<int> { V.root, IV.root, vi.root }));

        const auto fromV = rankedRoots (V, k);
        CHECK_EQ (fromV[0], I.root);                       // V -> I (cadence) ...
        CHECK_EQ (fromV[1], vi.root);                      // ... and V -> vi 40%

        CHECK_EQ (topRoot (IV, k), I.root);                // IV -> I 48%
        CHECK (topTwoAre (IV, k, I.root, V.root));
        CHECK_EQ (topRoot (vi, k), IV.root);               // vi -> IV 31%
        CHECK (topTwoAre (vi, k, IV.root, V.root));

        // borrowed bVI -> bVII -> I, iv -> I
        const Chord bVI = ch (t + 8, Q::Major), bVII = ch (t + 10, Q::Major), iv = ch (t + 5, Q::Minor);
        CHECK_EQ (topRoot (bVI, k), bVII.root);
        CHECK_EQ (topRoot (bVII, k), I.root);
        CHECK_EQ (topRoot (iv, k), I.root);
        CHECK (cb::transitionScore (bVI, bVII, k) >= 0.75f);
        CHECK (cb::transitionScore (bVII, I, k) >= 0.75f);
        CHECK (cb::transitionScore (iv, I, k) >= 0.75f);
    }
}

TEST_CASE ("suggest calibration: modal signature moves score high")
{
    for (int t = 0; t < 12; ++t)
    {
        const Key mixo { t, ScaleType::Mixolydian };
        const Chord I = ch (t, Q::Major), bVII = ch (t + 10, Q::Major), IV = ch (t + 5, Q::Major);
        CHECK (cb::transitionScore (I, bVII, mixo) >= 0.75f);
        const auto fromI = rankedRoots (I, mixo);
        CHECK (fromI[0] == bVII.root || fromI[1] == bVII.root);
        CHECK (topTwoAre (bVII, mixo, IV.root, I.root));

        const Key dorian { t, ScaleType::Dorian };
        CHECK_EQ (topRoot (ch (t, Q::Minor), dorian), cb::pc (t + 5));                    // i -> IV
        CHECK (cb::transitionScore (ch (t, Q::Minor), ch (t + 5, Q::Major), dorian) >= 0.75f);
        CHECK_EQ (topRoot (ch (t + 5, Q::Major), dorian), cb::pc (t));                    // IV -> i

        const Key phrygian { t, ScaleType::Phrygian };
        CHECK_EQ (topRoot (ch (t, Q::Minor), phrygian), cb::pc (t + 1));                  // i -> bII
        CHECK_EQ (topRoot (ch (t + 1, Q::Major), phrygian), cb::pc (t));                  // bII -> i
        CHECK (cb::transitionScore (ch (t + 1, Q::Major), ch (t, Q::Minor), phrygian) >= 0.75f);

        const Key lydian { t, ScaleType::Lydian };
        const auto fromLydI = rankedRoots (ch (t, Q::Major), lydian);
        CHECK (fromLydI[0] == cb::pc (t + 2) || fromLydI[1] == cb::pc (t + 2));          // I -> II
        CHECK (cb::transitionScore (ch (t, Q::Major), ch (t + 2, Q::Major), lydian) >= 0.75f);
        CHECK_EQ (topRoot (ch (t + 2, Q::Major), lydian), cb::pc (t));                    // II -> I

        const Key harmonic { t, ScaleType::HarmonicMinor };
        CHECK_EQ (topRoot (ch (t + 8, Q::Major), harmonic), cb::pc (t + 7));              // VI -> V (Andalusian)
        CHECK_EQ (topRoot (ch (t + 7, Q::Major), harmonic), cb::pc (t));                  // V -> i
        // Phrygian / harmonic minor: the old tritone and diminished picks no longer beat the cadence.
        CHECK (cb::transitionScore (ch (t + 1, Q::Major), ch (t, Q::Minor), phrygian)
               > cb::transitionScore (ch (t + 1, Q::Major), ch (t + 7, Q::Diminished), phrygian));
        CHECK (cb::transitionScore (ch (t + 8, Q::Major), ch (t + 7, Q::Major), harmonic)
               > cb::transitionScore (ch (t + 8, Q::Major), ch (t + 11, Q::Diminished), harmonic));
    }
}

TEST_CASE ("suggest calibration: colour chords inherit their base chord")
{
    const Key c { 0, ScaleType::Major };
    const Key a { 9, ScaleType::NaturalMinor };

    // An Am9 after F scores like Am after F, just below it.
    const float plain = cb::transitionScore (ch (5, Q::Major), ch (9, Q::Minor), c);
    const float ninth = cb::transitionScore (ch (5, Q::Major), ch (9, Q::Minor9), c);
    CHECK (ninth < plain);
    CHECK (ninth >= 0.85f * plain);
    CHECK (cb::transitionScore (ch (9, Q::Minor), ch (5, Q::Major7), a) >= 0.9f * cb::transitionScore (ch (9, Q::Minor), ch (5, Q::Major), a));

    // From a colour chord the plain-chord motion carries over.
    CHECK (std::abs (cb::transitionScore (ch (5, Q::Major9), ch (7, Q::Major), c) - cb::transitionScore (ch (5, Q::Major), ch (7, Q::Major), c)) < 0.03f);
    CHECK (std::abs (cb::transitionScore (ch (5, Q::Major7Sharp11), ch (0, Q::Major), c) - cb::transitionScore (ch (5, Q::Major), ch (0, Q::Major), c)) < 0.03f);

    // Colour -> colour moves the data favours get a small bonus.
    CHECK (cb::transitionScore (ch (5, Q::Major7), ch (4, Q::Minor7), a) > cb::transitionScore (ch (5, Q::Major), ch (4, Q::Minor), a));   // VImaj7 -> v7
    CHECK (cb::transitionScore (ch (5, Q::Major7), ch (4, Q::Minor7), a) >= 0.75f);
    CHECK (cb::transitionScore (ch (0, Q::Major7), ch (2, Q::Minor7), a) > cb::transitionScore (ch (0, Q::Major), ch (2, Q::Minor), a));   // IIImaj7 -> iv7
    CHECK (cb::transitionScore (ch (9, Q::Minor7), ch (5, Q::Major7), c) > cb::transitionScore (ch (9, Q::Minor7), ch (5, Q::Major), c) - 0.001f);

    // Colour -> its plain triad is a natural move (data: i7 -> i 17%), plain -> colour stays modest.
    CHECK (cb::transitionScore (ch (9, Q::Minor7), ch (9, Q::Minor), a) >= 0.6f);
    CHECK (cb::transitionScore (ch (7, Q::Add9), ch (7, Q::Major), a) >= 0.6f);
    CHECK (cb::transitionScore (ch (9, Q::Minor), ch (9, Q::Minor7), a) < 0.45f);

    // Dominant colours still resolve: G13 / G7b9 -> C like G -> C.
    CHECK (cb::transitionScore (ch (7, Q::Dominant13), ch (0, Q::Major), c) >= 0.9f);
    CHECK (cb::transitionScore (ch (4, Q::Dominant7Flat9), ch (9, Q::Minor), a) >= 0.9f);
}

TEST_CASE ("suggest calibration: few strong picks, by root")
{
    for (auto scale : { ScaleType::Major, ScaleType::NaturalMinor, ScaleType::HarmonicMinor })
        for (int t = 0; t < 12; ++t)
        {
            const Key k { t, scale };
            const auto p = cb::buildPalette (k);
            const auto chords = paletteChords (p);
            for (auto& fromPad : p[0].chords)
            {
                std::set<int> strongRoots;
                int strongPads = 0;
                for (auto& c : chords)
                {
                    if (c == fromPad.chord)
                        continue;
                    if (cb::transitionScore (fromPad.chord, c, k) >= 0.75f)
                    {
                        ++strongPads;
                        strongRoots.insert (c.root);
                    }
                }
                CHECK (strongRoots.size() >= 1 && strongRoots.size() <= 4);
                CHECK (strongPads <= 12);
            }
        }
}

//==============================================================================
// openingScore
//==============================================================================
TEST_CASE ("opening: tonic leads, data openers follow")
{
    for (auto& k : allKeys())
    {
        const auto chords = paletteChords (cb::buildPalette (k));
        const Chord tonic = cb::diatonicChord (k, 0, false);
        const float tonicScore = cb::openingScore (tonic, k);
        for (auto& c : chords)
        {
            const float v = cb::openingScore (c, k);
            CHECK (v >= 0.0f && v <= 1.0f);
            CHECK (v <= tonicScore);
            CHECK_EQ (v, cb::openingScore (c, k));
            if (cb::harmonicFunction (c, k) == cb::HarmonicFunction::Secondary)
                CHECK (v < 0.2f);
        }
        if (k.scale != ScaleType::Locrian)
            CHECK_EQ (tonicScore, 1.0f);
    }

    for (int t = 0; t < 12; ++t)
    {
        const Key a { t, ScaleType::NaturalMinor };
        const float VI = cb::openingScore (ch (t + 8, Q::Major), a);
        CHECK (VI > cb::openingScore (ch (t + 5, Q::Minor), a));    // > iv
        CHECK (VI > cb::openingScore (ch (t + 3, Q::Major), a));    // > III
        CHECK (VI > cb::openingScore (ch (t + 10, Q::Major), a));   // > VII
        CHECK (cb::openingScore (ch (t + 8, Q::Major7), a) >= VI);   // data: VImaj7 opens more often than VI
        CHECK (cb::openingScore (ch (t, Q::Minor7), a) > VI);        // i7 is the second-commonest opener

        const Key c { t, ScaleType::Major };
        const float V = cb::openingScore (ch (t + 7, Q::Major), c);
        CHECK (cb::openingScore (ch (t + 9, Q::Minor), c) > V);     // vi
        CHECK (cb::openingScore (ch (t + 5, Q::Major), c) > V);     // IV
        CHECK (cb::openingScore (ch (t + 8, Q::Major), c) > V);     // borrowed bVI
        CHECK (cb::openingScore (ch (t, Q::Major7), c) > cb::openingScore (ch (t + 9, Q::Minor), c));
        CHECK (cb::openingScore (ch (t + 11, Q::Diminished), c) < 0.2f);
    }
}

//==============================================================================
// Presets
//==============================================================================
TEST_CASE ("presets: valid for every key")
{
    for (auto& k : allKeys())
    {
        const auto presets = cb::presetsFor (k);
        const auto chords = paletteChords (cb::buildPalette (k));

        CHECK (presets.size() <= 20);
        CHECK (presets.size() >= 12);

        std::set<std::string> names;
        for (size_t i = 0; i < presets.size(); ++i)
        {
            const auto& p = presets[i];
            CHECK (! p.name.empty());
            CHECK (! p.description.empty());
            CHECK ((int) p.mood >= 0 && (int) p.mood < cb::numMoods);
            CHECK (names.insert (p.name).second);
            REQUIRE (! p.progression.empty());
            CHECK ((int) p.progression.slots.size() <= cb::kMaxSlots);

            std::string summary;
            for (auto& slot : p.progression.slots)
            {
                CHECK (contains (chords, slot.chord));
                CHECK (slot.lengthBeats >= cb::kMinSlotBeats && slot.lengthBeats <= cb::kMaxSlotBeats);
                CHECK_EQ (slot.inversion, -1);
                CHECK_EQ (slot.octaveShift, 0);
                summary += (summary.empty() ? "" : " - ") + cb::romanNumeral (slot.chord, k);
            }
            CHECK_EQ (p.romanSummary, summary);

            for (size_t j = 0; j < i; ++j)
                CHECK (presets[j].progression != p.progression);
        }

        // Deterministic
        const auto again = cb::presetsFor (k);
        REQUIRE (again.size() == presets.size());
        for (size_t i = 0; i < presets.size(); ++i)
        {
            CHECK_EQ (again[i].name, presets[i].name);
            CHECK (again[i].progression == presets[i].progression);
        }
    }
}

TEST_CASE ("presets: the well-known loops are there")
{
    auto has = [] (const Key& k, std::vector<Chord> want)
    {
        for (auto& p : cb::presetsFor (k))
        {
            if (p.progression.slots.size() != want.size())
                continue;
            bool same = true;
            for (size_t i = 0; i < want.size(); ++i)
                same = same && p.progression.slots[i].chord == want[i];
            if (same)
                return true;
        }
        return false;
    };

    for (int t = 0; t < 12; ++t)
    {
        const Key c { t, ScaleType::Major };
        const Chord I = ch (t, Q::Major), ii7 = ch (t + 2, Q::Minor7), IV = ch (t + 5, Q::Major), V = ch (t + 7, Q::Major),
                    vi = ch (t + 9, Q::Minor), bVI = ch (t + 8, Q::Major), bVII = ch (t + 10, Q::Major);
        CHECK (has (c, { I, V, vi, IV }));
        CHECK (has (c, { vi, IV, I, V }));
        CHECK (has (c, { I, vi, IV, V }));
        CHECK (has (c, { I, IV, vi, V }));
        CHECK (has (c, { I, bVII, IV, I }));
        CHECK (has (c, { I, bVI, bVII, I }));
        CHECK (has (c, { ii7, ch (t + 7, Q::Dominant7), ch (t, Q::Major7) }));
        CHECK (has (c, { I, V, vi, ch (t + 4, Q::Minor), IV, I, IV, V }));
        const auto majorPresets = cb::presetsFor (c);
        CHECK_EQ (majorPresets[0].romanSummary, std::string ("I - V - vi - IV"));

        const Key a { t, ScaleType::NaturalMinor };
        const Chord i = ch (t, Q::Minor), III = ch (t + 3, Q::Major), iv = ch (t + 5, Q::Minor), v = ch (t + 7, Q::Minor),
                    VI = ch (t + 8, Q::Major), VII = ch (t + 10, Q::Major), Vmaj = ch (t + 7, Q::Major);
        CHECK (has (a, { i, VI, III, VII }));
        CHECK (has (a, { i, VII, VI, VII }));
        CHECK (has (a, { VI, VII, i, i }));
        CHECK (has (a, { i, iv, VI, v }));
        CHECK (has (a, { i, VI, iv, Vmaj }));
        CHECK (has (a, { i, VII, VI, Vmaj }));
        CHECK (has (a, { ch (t, Q::Minor7), ch (t + 5, Q::Minor7), ch (t + 8, Q::Major7), ch (t + 7, Q::Minor7) }));
        CHECK (has (a, { i, VI, VII, i }));
        const auto minorPresets = cb::presetsFor (a);
        CHECK_EQ (minorPresets[0].romanSummary, std::string ("i - VI - III - VII"));

        // Every mood is represented in the main keys.
        for (auto& k : { c, a })
        {
            std::set<int> moods;
            for (auto& p : cb::presetsFor (k))
                moods.insert ((int) p.mood);
            CHECK ((int) moods.size() == cb::numMoods);
        }

        // Modes lead with their signature moves.
        CHECK (has ({ t, ScaleType::Mixolydian }, { I, bVII, IV, I }));
        CHECK (has ({ t, ScaleType::Dorian }, { ch (t, Q::Minor7), ch (t + 5, Q::Dominant7) }));
        auto firstHas = [] (const Key& k, const Chord& c)
        {
            const auto presets = cb::presetsFor (k);
            if (presets.empty())
                return false;
            for (auto& slot : presets[0].progression.slots)
                if (slot.chord == c)
                    return true;
            return false;
        };
        CHECK (firstHas ({ t, ScaleType::Phrygian }, ch (t + 1, Q::Major)));     // bII
        CHECK (firstHas ({ t, ScaleType::Lydian }, ch (t + 2, Q::Major)));       // II
        CHECK (firstHas ({ t, ScaleType::Mixolydian }, ch (t + 10, Q::Major)));  // bVII
        CHECK (firstHas ({ t, ScaleType::Dorian }, ch (t + 5, Q::Dominant7)));   // IV7
    }
}

//==============================================================================
// Spark
//==============================================================================
TEST_CASE ("spark: deterministic")
{
    for (auto& k : { Key { 0, ScaleType::Major }, Key { 9, ScaleType::NaturalMinor }, Key { 2, ScaleType::Dorian }, Key { 6, ScaleType::HarmonicMinor } })
    {
        const auto p = cb::buildPalette (k);
        for (int m = 0; m < cb::numMoods; ++m)
            for (std::uint32_t seed : { 0u, 1u, 2u, 77u, 123456789u, 0xFFFFFFFFu })
            {
                const auto a = cb::sparkProgression (k, p, seed, spark ((Mood) m, 5));
                const auto b = cb::sparkProgression (k, p, seed, spark ((Mood) m, 5));
                CHECK (a == b);
                CHECK_EQ ((int) a.slots.size(), 5);
            }
    }
}

TEST_CASE ("spark: different seeds give different progressions")
{
    for (auto& k : { Key { 0, ScaleType::Major }, Key { 9, ScaleType::NaturalMinor }, Key { 4, ScaleType::Phrygian } })
    {
        const auto p = cb::buildPalette (k);
        for (int m = 0; m < cb::numMoods; ++m)
        {
            std::set<std::string> distinct;
            int neighbourDiffs = 0;
            cb::Progression prev;
            for (std::uint32_t seed = 1; seed <= 32; ++seed)
            {
                const auto prog = cb::sparkProgression (k, p, seed, spark ((Mood) m));
                distinct.insert (describe (prog, k));
                if (seed > 1 && prog != prev)
                    ++neighbourDiffs;
                prev = prog;
            }
            CHECK (distinct.size() >= 16);
            CHECK (neighbourDiffs >= 26);
        }
    }
}

TEST_CASE ("spark: clamps length and beats")
{
    const Key k { 0, ScaleType::Major };
    const auto p = cb::buildPalette (k);
    const std::pair<int, int> lengths[] = { { -3, 2 }, { 0, 2 }, { 1, 2 }, { 2, 2 }, { 3, 3 }, { 8, 8 }, { 9, 8 }, { 50, 8 } };
    const std::pair<int, int> beats[] = { { -1, 1 }, { 0, 1 }, { 1, 1 }, { 7, 7 }, { 16, 16 }, { 99, 16 } };
    for (auto& l : lengths)
        for (auto& b : beats)
        {
            const auto prog = cb::sparkProgression (k, p, 42u, spark (Mood::Uplifting, l.first, b.first));
            CHECK_EQ ((int) prog.slots.size(), l.second);
            for (auto& slot : prog.slots)
            {
                CHECK_EQ (slot.lengthBeats, b.second);
                CHECK_EQ (slot.inversion, -1);
                CHECK_EQ (slot.octaveShift, 0);
            }
        }

    // Out-of-range moods are clamped, not undefined.
    auto s = spark (Mood::Dark);
    s.mood = (Mood) 99;
    CHECK_EQ ((int) cb::sparkProgression (k, p, 3u, s).slots.size(), 4);
}

TEST_CASE ("spark: palette chords only, no immediate repeats, loop closes on a new root")
{
    for (auto& k : allKeys())
    {
        const auto p = cb::buildPalette (k);
        const auto chords = paletteChords (p);
        for (int m = 0; m < cb::numMoods; ++m)
            for (int n : { 2, 4, 8 })
                for (std::uint32_t seed = 1; seed <= 6; ++seed)
                {
                    const auto prog = cb::sparkProgression (k, p, seed * 7919u + (std::uint32_t) k.tonic, spark ((Mood) m, n));
                    REQUIRE ((int) prog.slots.size() == n);
                    for (size_t i = 0; i < prog.slots.size(); ++i)
                    {
                        CHECK (contains (chords, prog.slots[i].chord));
                        if (i > 0)
                        {
                            CHECK (prog.slots[i].chord != prog.slots[i - 1].chord);
                            CHECK (prog.slots[i].chord.root != prog.slots[i - 1].chord.root);
                        }
                    }
                    CHECK (prog.slots.back().chord.root != prog.slots.front().chord.root);
                }
    }
}

TEST_CASE ("spark: short loops use fresh roots, long loops spread out")
{
    for (auto& k : { Key { 0, ScaleType::Major }, Key { 9, ScaleType::NaturalMinor }, Key { 5, ScaleType::Major },
                     Key { 1, ScaleType::NaturalMinor }, Key { 9, ScaleType::HarmonicMinor }, Key { 2, ScaleType::Dorian } })
    {
        const auto p = cb::buildPalette (k);
        for (int m = 0; m < cb::numMoods; ++m)
        {
            int distinctLoops = 0, wideLongLoops = 0;
            for (std::uint32_t seed = 1; seed <= 40; ++seed)
            {
                std::set<int> roots;
                for (auto& slot : cb::sparkProgression (k, p, seed, spark ((Mood) m, 4)).slots)
                    roots.insert (slot.chord.root);
                distinctLoops += roots.size() == 4 ? 1 : 0;

                std::set<int> longRoots;
                for (auto& slot : cb::sparkProgression (k, p, seed, spark ((Mood) m, 8)).slots)
                    longRoots.insert (slot.chord.root);
                wideLongLoops += longRoots.size() >= 4 ? 1 : 0;
            }
            // Dark may fall back onto the tonic mid-loop (i - bII - i - VI).
            CHECK (distinctLoops >= ((Mood) m == Mood::Dark ? 24 : 38));
            CHECK (wideLongLoops >= 36);
        }
    }
}

TEST_CASE ("spark: empty and tiny palettes")
{
    const Key k { 0, ScaleType::Major };
    CHECK (cb::sparkProgression (k, {}, 1u, spark (Mood::Epic)).empty());

    std::vector<cb::PaletteSection> empty (5);
    CHECK (cb::sparkProgression (k, empty, 1u, spark (Mood::Epic)).empty());

    std::vector<cb::PaletteSection> one (1);
    one[0].chords.push_back ({ ch (0, Q::Major), "I", cb::HarmonicFunction::Tonic, "" });
    const auto single = cb::sparkProgression (k, one, 1u, spark (Mood::Epic, 4));
    REQUIRE (single.slots.size() >= 1);
    CHECK (single.slots[0].chord == ch (0, Q::Major));

    std::vector<cb::PaletteSection> two (1);
    two[0].chords.push_back ({ ch (0, Q::Major), "I", cb::HarmonicFunction::Tonic, "" });
    two[0].chords.push_back ({ ch (0, Q::Major7), "Imaj7", cb::HarmonicFunction::Tonic, "" });
    for (std::uint32_t seed = 1; seed <= 8; ++seed)
    {
        const auto prog = cb::sparkProgression (k, two, seed, spark ((Mood) (seed % 6), 4));
        REQUIRE (prog.slots.size() == 4);
        for (size_t i = 1; i < prog.slots.size(); ++i)
            CHECK (prog.slots[i].chord != prog.slots[i - 1].chord);
    }
}

TEST_CASE ("spark: moods have their character")
{
    const Key c { 0, ScaleType::Major };
    const Key a { 9, ScaleType::NaturalMinor };

    struct Tally
    {
        int chords = 0, hits = 0, progs = 0, progHits = 0, openers = 0;
    };

    auto run = [] (const Key& k, Mood m, auto chordTest, auto progTest, auto openerTest)
    {
        const auto p = cb::buildPalette (k);
        Tally t;
        for (std::uint32_t seed = 1; seed <= 60; ++seed)
        {
            const auto prog = cb::sparkProgression (k, p, seed, spark (m));
            ++t.progs;
            t.progHits += progTest (prog) ? 1 : 0;
            t.openers += openerTest (prog.slots.front().chord) ? 1 : 0;
            for (auto& slot : prog.slots)
            {
                ++t.chords;
                t.hits += chordTest (slot.chord) ? 1 : 0;
            }
        }
        return t;
    };
    auto any = [] (const cb::Progression&) { return true; };
    auto anyChord = [] (const Chord&) { return true; };
    auto triad = [] (const Chord& x) { return x.quality == Q::Major || x.quality == Q::Minor; };
    auto offsetIn = [] (const Key& k, std::set<int> offs) { return [k, offs] (const Chord& x) { return offs.count (offsetOf (x, k)) > 0; }; };

    // Uplifting: mostly plain triads, opening on I / IV / vi.
    auto up = run (c, Mood::Uplifting, triad, any, offsetIn (c, { 0, 5, 9 }));
    CHECK (up.hits * 10 >= up.chords * 7);
    CHECK (up.openers * 10 >= up.progs * 8);

    // Lo-fi: 7ths / 9ths / 6ths.
    for (auto& k : { c, a })
    {
        auto lofi = run (k, Mood::LoFi, [] (const Chord& x) { return isJazzy (x.quality); }, any, anyChord);
        CHECK (lofi.hits * 10 >= lofi.chords * 7);
    }

    // Dreamy: maj7 / add9 / maj9 / 6 / 6-9 / minor-7th colour.
    for (auto& k : { c, a })
    {
        auto dreamy = run (k, Mood::Dreamy, [] (const Chord& x) { return isDreamyColour (x.quality); }, any, anyChord);
        CHECK (dreamy.hits * 10 >= dreamy.chords * 7);
    }

    // Epic (minor): the i - VI - III - VII family.
    auto epic = run (a, Mood::Epic, offsetIn (a, { 0, 8, 3, 10 }), any, offsetIn (a, { 0, 8 }));
    CHECK (epic.hits * 10 >= epic.chords * 7);
    CHECK (epic.openers * 10 >= epic.progs * 8);

    // Dark (minor): i / bVI / bVII / iv / v / bII.
    auto dark = run (a, Mood::Dark, offsetIn (a, { 0, 8, 10, 5, 7, 1 }), any, offsetIn (a, { 0, 8 }));
    CHECK (dark.hits * 10 >= dark.chords * 8);
    CHECK (dark.openers * 10 >= dark.progs * 8);

    // Dark (major): leans on the borrowed minor chords.
    auto darkMajor = run (c, Mood::Dark, anyChord,
                          [&c] (const cb::Progression& prog)
                          {
                              for (auto& slot : prog.slots)
                                  if (cb::harmonicFunction (slot.chord, c) == cb::HarmonicFunction::Borrowed)
                                      return true;
                              return false;
                          },
                          anyChord);
    CHECK (darkMajor.progHits * 2 >= darkMajor.progs);

    // Emotional: vi / i centred.
    auto emoMajor = run (c, Mood::Emotional, anyChord,
                         [&c] (const cb::Progression& prog)
                         {
                             for (auto& slot : prog.slots)
                                 if (offsetOf (slot.chord, c) == 9)
                                     return true;
                             return false;
                         },
                         offsetIn (c, { 9, 0, 5 }));
    CHECK (emoMajor.progHits * 10 >= emoMajor.progs * 6);
    CHECK (emoMajor.openers * 10 >= emoMajor.progs * 8);

    auto emoMinor = run (a, Mood::Emotional, anyChord, any, offsetIn (a, { 0, 8 }));
    CHECK (emoMinor.openers * 10 >= emoMinor.progs * 8);
}

TEST_CASE ("spark: Locrian uses its own diminished tonic")
{
    int progs = 0, withHome = 0, openOnHome = 0;
    for (int t = 0; t < 12; ++t)
    {
        const Key k { t, ScaleType::Locrian };
        const auto p = cb::buildPalette (k);
        for (int m = 0; m < cb::numMoods; ++m)
            for (std::uint32_t seed = 1; seed <= 40; ++seed)
            {
                const auto prog = cb::sparkProgression (k, p, seed, spark ((Mood) m));
                REQUIRE (! prog.empty());
                ++progs;
                bool home = false;
                for (auto& slot : prog.slots)
                    home = home || (offsetOf (slot.chord, k) == 0 && cb::isDiatonic (slot.chord, k));
                withHome += home ? 1 : 0;
                openOnHome += offsetOf (prog.slots.front().chord, k) == 0 && cb::isDiatonic (prog.slots.front().chord, k) ? 1 : 0;
            }
    }
    // Before the fix: the diatonic i° / iø7 opened 0 of 200 progressions and appeared in at most 8%.
    CHECK (withHome * 10 >= progs * 4);
    CHECK (openOnHome * 10 >= progs * 2);
}

TEST_CASE ("presets: Locrian presets mostly sit on the diminished home")
{
    for (int t = 0; t < 12; ++t)
    {
        const Key k { t, ScaleType::Locrian };
        const auto presets = cb::presetsFor (k);
        CHECK (presets.size() >= 12 && presets.size() <= 20);
        size_t onHome = 0, borrowedTonic = 0;
        for (auto& p : presets)
        {
            bool home = false, borrowed = false;
            for (auto& slot : p.progression.slots)
            {
                home = home || (offsetOf (slot.chord, k) == 0 && cb::isDiatonic (slot.chord, k));
                borrowed = borrowed || (offsetOf (slot.chord, k) == 0 && ! cb::isDiatonic (slot.chord, k));
            }
            onHome += home ? 1 : 0;
            borrowedTonic += borrowed ? 1 : 0;
            if (borrowed)
                CHECK (p.description.find ("(borrowed minor i)") != std::string::npos);
        }
        CHECK (onHome * 2 > presets.size());          // was 5 of 20
        CHECK (borrowedTonic <= 6);
    }
}
