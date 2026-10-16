#include "TestFramework.h"

#include "core/Pattern.h"
#include "core/Progression.h"
#include "core/Voicing.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

// Tests for Voicing / Pattern / Progression (owner: core-voicing agent)

using namespace cb;

namespace
{
constexpr double kTol = 1.0e-9;

const VoicingStyle kStyles[] = { VoicingStyle::Close, VoicingStyle::Open, VoicingStyle::Spread };
const int kCentres[] = { 48, 60, 72 };

Chord ch (int root, ChordQuality q) { return Chord { root, q }; }

VoicingSettings vset (VoicingStyle style, int centre = 60, bool voiceLeading = true)
{
    VoicingSettings s;
    s.style = style;
    s.centerNote = centre;
    s.voiceLeading = voiceLeading;
    return s;
}

bool ascendingUnique (const std::vector<int>& v)
{
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i] <= v[i - 1])
            return false;
    return true;
}

bool inMidiRange (const std::vector<int>& v)
{
    for (int n : v)
        if (n < 21 || n > 108)
            return false;
    return true;
}

std::set<int> pcSet (const std::vector<int>& v)
{
    std::set<int> s;
    for (int n : v)
        s.insert (pc (n));
    return s;
}

std::set<int> chordPcSet (const Chord& c)
{
    const auto p = c.pitchClasses();
    return std::set<int> (p.begin(), p.end());
}

double mean (const std::vector<int>& v)
{
    double s = 0.0;
    for (int n : v)
        s += n;
    return v.empty() ? 0.0 : s / (double) v.size();
}

std::vector<int> relative (const std::vector<int>& v)
{
    std::vector<int> r;
    for (int n : v)
        r.push_back (n - v.front());
    return r;
}

std::vector<int> shifted (std::vector<int> v, int semis)
{
    for (auto& n : v)
        n += semis;
    return v;
}

// Sum of |movement|, sorted pairing (equal sizes only).
int movement (const std::vector<int>& a, const std::vector<int>& b)
{
    int m = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        m += std::abs (a[i] - b[i]);
    return m;
}

int totalMovement (const std::vector<std::vector<int>>& vs)
{
    int m = 0;
    for (size_t i = 1; i < vs.size(); ++i)
        m += movement (vs[i - 1], vs[i]);
    return m;
}

bool hasNinth (ChordQuality q)
{
    for (int i : qualityIntervals (q))
        if (i >= 12)
            return true;
    return false;
}

int ninthIndex (ChordQuality q)
{
    const auto& iv = qualityIntervals (q);
    for (int i = 0; i < (int) iv.size(); ++i)
        if (iv[(size_t) i] >= 12)
            return i;
    return -1;
}

// Deterministic pseudo-random chord sequence for sweeps.
struct Lcg
{
    uint32_t state = 12345u;
    int next (int mod)
    {
        state = state * 1664525u + 1013904223u;
        return (int) ((state >> 8) % (uint32_t) mod);
    }
};

Progression makeProgression (const std::vector<Chord>& chords, const std::vector<int>& lengths = {})
{
    Progression p;
    for (size_t i = 0; i < chords.size(); ++i)
    {
        ProgressionSlot s;
        s.chord = chords[i];
        s.lengthBeats = i < lengths.size() ? lengths[i] : 4;
        p.slots.push_back (s);
    }
    return p;
}

RenderSettings rset (RhythmPattern pattern, RateDivision rate = RateDivision::Eighth, BassMode bass = BassMode::Off)
{
    RenderSettings s;
    s.pattern = pattern;
    s.rate = rate;
    s.bass = bass;
    s.velocity = 0.75f;
    s.gate = 0.9f;
    s.swing = 0.0f;
    s.humanize = 0.0f;
    return s;
}

std::vector<NoteEvent> channel (const std::vector<NoteEvent>& events, int ch)
{
    std::vector<NoteEvent> out;
    for (auto& e : events)
        if (e.channel == ch)
            out.push_back (e);
    return out;
}

bool near (double a, double b, double tol = 1.0e-9) { return std::abs (a - b) <= tol; }

const std::vector<int> kCMajor { 60, 64, 67 };
const std::vector<int> kCMaj7 { 60, 64, 67, 71 };

// One slot of C major with the given voicing.
std::vector<NoteEvent> renderOne (const RenderSettings& s, const std::vector<int>& voicing = kCMajor, int beats = 4,
                                  const Chord& chord = Chord { 0, ChordQuality::Major })
{
    return renderProgression (makeProgression ({ chord }, { beats }), { voicing }, s);
}
} // namespace

//==============================================================================
// Voicing
//==============================================================================
TEST_CASE ("voicing: style names")
{
    CHECK (std::strcmp (voicingStyleName (VoicingStyle::Close), "Close") == 0);
    CHECK (std::strcmp (voicingStyleName (VoicingStyle::Open), "Open") == 0);
    CHECK (std::strcmp (voicingStyleName (VoicingStyle::Spread), "Spread") == 0);
}

TEST_CASE ("voicing: invariants for every quality x style x inversion x centre")
{
    int checked = 0;
    for (int q = 0; q < numChordQualities; ++q)
        for (int root = 0; root < 12; ++root)
            for (auto style : kStyles)
                for (int inv = -1; inv <= 4; ++inv)
                    for (int centre : kCentres)
                    {
                        const Chord c = ch (root, (ChordQuality) q);
                        const auto v = voiceChord (c, vset (style, centre), inv, 0, {});
                        REQUIRE (! v.empty());
                        CHECK (ascendingUnique (v));
                        CHECK (inMidiRange (v));
                        CHECK (pcSet (v) == chordPcSet (c));
                        CHECK ((int) v.size() >= c.numTones());
                        CHECK (std::abs (mean (v) - centre) <= 6.0 + kTol);
                        ++checked;
                    }
    CHECK_EQ (checked, numChordQualities * 12 * 3 * 6 * 3);
}

TEST_CASE ("voicing: explicit inversions put the right chord tone in the bass")
{
    for (int q = 0; q < numChordQualities; ++q)
        for (int root = 0; root < 12; ++root)
            for (auto style : kStyles)
                for (int inv = 0; inv <= 4; ++inv)
                    for (int centre : kCentres)
                    {
                        const Chord c = ch (root, (ChordQuality) q);
                        const auto v = voiceChord (c, vset (style, centre), inv, 0, {});
                        REQUIRE (! v.empty());
                        const int expectedTone = std::min (inv, c.numTones() - 1);
                        CHECK_EQ (pc (v.front()), c.pitchClasses()[(size_t) expectedTone]);
                    }
}

TEST_CASE ("voicing: auto without a previous voicing is root position nearest the centre")
{
    for (int q = 0; q < numChordQualities; ++q)
        for (auto style : kStyles)
            for (int centre : kCentres)
            {
                const Chord c = ch (7, (ChordQuality) q);
                const auto autoV = voiceChord (c, vset (style, centre), -1, 0, {});
                const auto rootV = voiceChord (c, vset (style, centre), 0, 0, {});
                CHECK_EQ (autoV, rootV);
                CHECK_EQ (pc (autoV.front()), 7);
            }
}

TEST_CASE ("voicing: inversion is clamped to the last chord tone")
{
    const auto s = vset (VoicingStyle::Close);
    const Chord c = ch (0, ChordQuality::Major);
    CHECK_EQ (voiceChord (c, s, 3, 0, {}), voiceChord (c, s, 2, 0, {}));
    CHECK_EQ (voiceChord (c, s, 4, 0, {}), voiceChord (c, s, 2, 0, {}));
    CHECK_EQ (voiceChord (c, s, 99, 0, {}), voiceChord (c, s, 2, 0, {}));

    const Chord m7 = ch (2, ChordQuality::Minor7);
    CHECK_EQ (voiceChord (m7, s, 4, 0, {}), voiceChord (m7, s, 3, 0, {}));

    const Chord maj9 = ch (0, ChordQuality::Major9);
    CHECK_EQ (voiceChord (maj9, s, 7, 0, {}), voiceChord (maj9, s, 4, 0, {}));
}

TEST_CASE ("voicing: close C major is C4 E4 G4 at centre 60, inversions packed")
{
    const auto s = vset (VoicingStyle::Close, 60);
    const Chord c = ch (0, ChordQuality::Major);
    CHECK_EQ (voiceChord (c, s, 0, 0, {}), (std::vector<int> { 60, 64, 67 }));
    CHECK_EQ (relative (voiceChord (c, s, 1, 0, {})), (std::vector<int> { 0, 3, 8 }));   // E G C
    CHECK_EQ (relative (voiceChord (c, s, 2, 0, {})), (std::vector<int> { 0, 5, 9 }));   // G C E
    CHECK_EQ (voiceChord (c, vset (VoicingStyle::Close, 72), 0, 0, {}), (std::vector<int> { 72, 76, 79 }));
    CHECK_EQ (voiceChord (c, vset (VoicingStyle::Close, 48), 0, 0, {}), (std::vector<int> { 48, 52, 55 }));
}

TEST_CASE ("voicing: close triads, sixths and sevenths stay within an octave")
{
    for (int q = 0; q < numChordQualities; ++q)
    {
        if (hasNinth ((ChordQuality) q))
            continue;
        for (int root = 0; root < 12; ++root)
            for (int inv = 0; inv < 4; ++inv)
            {
                const auto v = voiceChord (ch (root, (ChordQuality) q), vset (VoicingStyle::Close), inv, 0, {});
                CHECK (v.back() - v.front() < 12);
                CHECK_EQ ((int) v.size(), ch (root, (ChordQuality) q).numTones());
            }
    }
}

TEST_CASE ("voicing: close add9 keeps the 9th above the 3rd (C E G D, not C D E G)")
{
    const auto s = vset (VoicingStyle::Close, 60);
    CHECK_EQ (relative (voiceChord (ch (0, ChordQuality::Add9), s, 0, 0, {})), (std::vector<int> { 0, 4, 7, 14 }));
    CHECK_EQ (relative (voiceChord (ch (0, ChordQuality::MinorAdd9), s, 0, 0, {})), (std::vector<int> { 0, 3, 7, 14 }));
    CHECK_EQ (relative (voiceChord (ch (0, ChordQuality::Add9), s, 1, 0, {})), (std::vector<int> { 0, 3, 8, 10 }));   // E G C D
    CHECK_EQ (relative (voiceChord (ch (0, ChordQuality::Add9), s, 2, 0, {})), (std::vector<int> { 0, 5, 9, 19 }));   // G C E D
}

TEST_CASE ("voicing: close 9th chords stack root-3-5-7-9")
{
    const auto s = vset (VoicingStyle::Close, 60);
    CHECK_EQ (relative (voiceChord (ch (0, ChordQuality::Major9), s, 0, 0, {})), (std::vector<int> { 0, 4, 7, 11, 14 }));
    CHECK_EQ (relative (voiceChord (ch (0, ChordQuality::Dominant9), s, 0, 0, {})), (std::vector<int> { 0, 4, 7, 10, 14 }));
    CHECK_EQ (relative (voiceChord (ch (0, ChordQuality::Minor9), s, 0, 0, {})), (std::vector<int> { 0, 3, 7, 10, 14 }));
}

TEST_CASE ("voicing: the 9th sits above the 3rd in every style unless it is the bass")
{
    for (int q = 0; q < numChordQualities; ++q)
    {
        if (! hasNinth ((ChordQuality) q))
            continue;

        const int ni = ninthIndex ((ChordQuality) q);
        for (int root = 0; root < 12; ++root)
            for (auto style : kStyles)
                for (int inv = -1; inv <= 4; ++inv)
                {
                    if (inv == ni || (inv > ni && ni == ch (root, (ChordQuality) q).numTones() - 1))
                        continue;   // explicit 9th-in-bass (or clamped to it)

                    for (int centre : kCentres)
                    {
                        const Chord c = ch (root, (ChordQuality) q);
                        const auto v = voiceChord (c, vset (style, centre), inv, 0, {});
                        const int thirdPc = c.pitchClasses()[1];
                        const int ninthPc = c.pitchClasses()[(size_t) ni];
                        int lowestThird = 1000, lowestNinth = 1000;
                        for (int n : v)
                        {
                            if (pc (n) == thirdPc) lowestThird = std::min (lowestThird, n);
                            if (pc (n) == ninthPc) lowestNinth = std::min (lowestNinth, n);
                        }
                        CHECK (lowestNinth > lowestThird);
                        // never root + whole tone at the bottom
                        CHECK (! (pc (v[0]) == c.root && v[1] - v[0] == 2));
                    }
                }
    }
}

TEST_CASE ("voicing: 9th in the bass on explicit request")
{
    const auto s = vset (VoicingStyle::Close, 60);
    const auto add9 = voiceChord (ch (0, ChordQuality::Add9), s, 3, 0, {});
    CHECK_EQ (pc (add9.front()), 2);
    CHECK_EQ (relative (add9), (std::vector<int> { 0, 2, 5, 10 }));   // D E G C

    const auto maj9 = voiceChord (ch (0, ChordQuality::Major9), s, 4, 0, {});
    CHECK_EQ (pc (maj9.front()), 2);
    CHECK_EQ (relative (maj9), (std::vector<int> { 0, 2, 5, 9, 10 }));   // D E G B C

    const auto open = voiceChord (ch (0, ChordQuality::Major9), vset (VoicingStyle::Open), 4, 0, {});
    CHECK_EQ (relative (open), (std::vector<int> { 0, 5, 10, 14, 21 }));   // D G C E B
}

TEST_CASE ("voicing: open is drop-2 of a close voicing")
{
    for (int q = 0; q < numChordQualities; ++q)
    {
        if (hasNinth ((ChordQuality) q))
            continue;
        for (int root = 0; root < 12; ++root)
            for (int inv = 0; inv < 4; ++inv)
                for (int centre : kCentres)
                {
                    const Chord c = ch (root, (ChordQuality) q);
                    auto v = voiceChord (c, vset (VoicingStyle::Open, centre), inv, 0, {});
                    REQUIRE (v.size() >= 3);
                    // Raising the dropped (lowest) voice back up an octave must give a close voicing.
                    v[0] += 12;
                    std::sort (v.begin(), v.end());
                    CHECK (ascendingUnique (v));
                    CHECK (v.back() - v.front() < 12);
                    // ...in which the raised voice is the second highest.
                    CHECK_EQ (pc (v[v.size() - 2]), c.pitchClasses()[(size_t) std::min (inv, c.numTones() - 1)]);
                }
    }

    const auto s = vset (VoicingStyle::Open, 60);
    CHECK_EQ (voiceChord (ch (0, ChordQuality::Major), s, 0, 0, {}), (std::vector<int> { 48, 55, 64 }));   // C3 G3 E4
    CHECK_EQ (relative (voiceChord (ch (0, ChordQuality::Major7), s, 0, 0, {})), (std::vector<int> { 0, 7, 11, 16 }));   // C G B E
    CHECK_EQ (relative (voiceChord (ch (0, ChordQuality::Dominant7), s, 1, 0, {})), (std::vector<int> { 0, 6, 8, 15 })); // E Bb C G
}

TEST_CASE ("voicing: open 9th chords put drop-2 under the 9th")
{
    const auto s = vset (VoicingStyle::Open, 60);
    CHECK_EQ (relative (voiceChord (ch (0, ChordQuality::Major9), s, 0, 0, {})), (std::vector<int> { 0, 7, 11, 16, 26 }));   // C G B E D
    CHECK_EQ (relative (voiceChord (ch (0, ChordQuality::Add9), s, 0, 0, {})), (std::vector<int> { 0, 7, 16, 26 }));         // C G E D
}

TEST_CASE ("voicing: spread is wide with the root low")
{
    for (int q = 0; q < numChordQualities; ++q)
        for (int root = 0; root < 12; ++root)
            for (int centre : kCentres)
            {
                const Chord c = ch (root, (ChordQuality) q);
                const auto v = voiceChord (c, vset (VoicingStyle::Spread, centre), -1, 0, {});
                CHECK_EQ (pc (v.front()), root);
                const int span = v.back() - v.front();
                CHECK (span >= 19);
                // Very low spreads may lift a crowded top note an octave (low-register rule), so allow a bit more there.
                CHECK (span <= (centre >= 60 ? 31 : 38));
                CHECK (v[1] - v[0] >= 5);   // open at the bottom
            }

    const auto s = vset (VoicingStyle::Spread, 60);
    CHECK_EQ (voiceChord (ch (0, ChordQuality::Major), s, 0, 0, {}), (std::vector<int> { 48, 55, 64, 72 }));   // C3 G3 E4 C5
    CHECK_EQ (relative (voiceChord (ch (0, ChordQuality::Major7), s, 0, 0, {})), (std::vector<int> { 0, 7, 16, 23 }));     // C G E B
    CHECK_EQ (relative (voiceChord (ch (0, ChordQuality::Major9), s, 0, 0, {})), (std::vector<int> { 0, 7, 16, 23, 26 })); // C G E B D
}

TEST_CASE ("voicing: spread never has intervals smaller than a 4th below MIDI 52")
{
    for (int q = 0; q < numChordQualities; ++q)
        for (int root = 0; root < 12; ++root)
            for (int inv = -1; inv <= 4; ++inv)
                for (int centre = 30; centre <= 64; centre += 2)
                {
                    const auto v = voiceChord (ch (root, (ChordQuality) q), vset (VoicingStyle::Spread, centre), inv, 0, {});
                    CHECK (ascendingUnique (v));
                    CHECK (inMidiRange (v));
                    CHECK (pcSet (v) == chordPcSet (ch (root, (ChordQuality) q)));
                    for (size_t i = 1; i < v.size(); ++i)
                        if (v[i - 1] < 52)
                            CHECK (v[i] - v[i - 1] >= 5);
                }
}

TEST_CASE ("voicing: octaveShift moves the voicing by whole octaves")
{
    for (int q = 0; q < numChordQualities; ++q)
        for (auto style : kStyles)
            for (int inv = -1; inv <= 3; ++inv)
                for (int shift = -2; shift <= 2; ++shift)
                {
                    // The low-register rule may legitimately reshape low spreads, so Spread is checked from a
                    // register where it never applies.
                    const bool spread = style == VoicingStyle::Spread;
                    if (spread && (shift < 0 || shift > 1))
                        continue;
                    const int centre = spread ? 68 : 60;
                    const Chord c = ch (9, (ChordQuality) q);
                    const auto base = voiceChord (c, vset (style, centre), inv, 0, {});
                    const auto moved = voiceChord (c, vset (style, centre), inv, shift, {});
                    CHECK_EQ (moved, shifted (base, 12 * shift));
                }

    // Spread shifted down is still the same chord, an octave or two lower on average.
    const Chord c = ch (0, ChordQuality::Major7);
    const auto down = voiceChord (c, vset (VoicingStyle::Spread, 60), 0, -1, {});
    CHECK (pcSet (down) == chordPcSet (c));
    CHECK (std::abs (mean (down) - 48.0) <= 6.0 + kTol);
}

TEST_CASE ("voicing: notes stay within 21..108 at extreme centres and shifts")
{
    Lcg rng;
    for (int centre : { 0, 12, 24, 96, 108, 127 })
        for (auto style : kStyles)
            for (int shift = -2; shift <= 2; ++shift)
            {
                std::vector<int> prev;
                for (int i = 0; i < 24; ++i)
                {
                    const Chord c = ch (rng.next (12), (ChordQuality) rng.next (numChordQualities));
                    const int inv = rng.next (6) - 1;
                    const auto v = voiceChord (c, vset (style, centre), inv, shift, prev);
                    REQUIRE (! v.empty());
                    CHECK (inMidiRange (v));
                    CHECK (ascendingUnique (v));
                    CHECK (pcSet (v) == chordPcSet (c));
                    prev = v;
                }
            }
}

TEST_CASE ("voicing: voice leading C -> Am -> F -> G (close)")
{
    const auto s = vset (VoicingStyle::Close, 60);
    const auto c = voiceChord (ch (0, ChordQuality::Major), s, -1, 0, {});
    const auto am = voiceChord (ch (9, ChordQuality::Minor), s, -1, 0, c);
    const auto f = voiceChord (ch (5, ChordQuality::Major), s, -1, 0, am);
    const auto g = voiceChord (ch (7, ChordQuality::Major), s, -1, 0, f);
    CHECK_EQ (c, (std::vector<int> { 60, 64, 67 }));
    CHECK_EQ (am, (std::vector<int> { 60, 64, 69 }));
    CHECK_EQ (f, (std::vector<int> { 60, 65, 69 }));
    CHECK_EQ (g, (std::vector<int> { 59, 62, 67 }));   // tie with D G B broken towards the centre
}

TEST_CASE ("voicing: voice leading tie-break prefers the mean closest to the centre")
{
    // From C4 F4 A4 both B3 D4 G4 and D4 G4 B4 move 6 semitones; B3 D4 G4 is closer to 60.
    const auto g = voiceChord (ch (7, ChordQuality::Major), vset (VoicingStyle::Close, 60), -1, 0, { 60, 65, 69 });
    CHECK_EQ (g, (std::vector<int> { 59, 62, 67 }));
    // Centred higher, the same tie resolves the other way.
    const auto gHigh = voiceChord (ch (7, ChordQuality::Major), vset (VoicingStyle::Close, 66), -1, 0, { 60, 65, 69 });
    CHECK_EQ (gHigh, (std::vector<int> { 62, 67, 71 }));
}

TEST_CASE ("voicing: voice leading reduces movement for I-vi-IV-V")
{
    const std::vector<Chord> chords { ch (0, ChordQuality::Major), ch (9, ChordQuality::Minor),
                                      ch (5, ChordQuality::Major), ch (7, ChordQuality::Major) };
    for (auto style : kStyles)
        for (int centre : kCentres)
        {
            const auto p = makeProgression (chords);
            const auto led = voiceProgression (p, vset (style, centre, true));
            const auto rootPos = voiceProgression (p, vset (style, centre, false));
            CHECK (totalMovement (led) < totalMovement (rootPos));
        }

    const auto led = voiceProgression (makeProgression (chords), vset (VoicingStyle::Close, 60));
    CHECK_EQ (totalMovement (led), 9);
}

TEST_CASE ("voicing: voice leading reduces movement for ii-V-I")
{
    const std::vector<Chord> chords { ch (2, ChordQuality::Minor7), ch (7, ChordQuality::Dominant7), ch (0, ChordQuality::Major7) };
    for (auto style : kStyles)
        for (int centre : kCentres)
        {
            const auto p = makeProgression (chords);
            const auto led = voiceProgression (p, vset (style, centre, true));
            const auto rootPos = voiceProgression (p, vset (style, centre, false));
            CHECK (totalMovement (led) < totalMovement (rootPos));
        }

    const auto led = voiceProgression (makeProgression (chords), vset (VoicingStyle::Close, 60));
    CHECK_EQ (led[1], (std::vector<int> { 50, 53, 55, 59 }));   // G7 as D F G B
    CHECK_EQ (led[2], (std::vector<int> { 48, 52, 55, 59 }));   // Cmaj7 as C E G B
    CHECK_EQ (totalMovement (led), 6);
}

TEST_CASE ("voicing: voice leading picks the least movement among in-window candidates")
{
    for (auto style : { VoicingStyle::Close, VoicingStyle::Open })
        for (int centre : { 60, 72 })
            for (bool sevenths : { false, true })
            {
                const ChordQuality triads[] = { ChordQuality::Major, ChordQuality::Minor, ChordQuality::Diminished, ChordQuality::Sus4 };
                const ChordQuality fours[] = { ChordQuality::Major7, ChordQuality::Minor7, ChordQuality::Dominant7, ChordQuality::Major6 };
                Lcg rng;
                const auto s = vset (style, centre);
                std::vector<int> prev = voiceChord (ch (0, sevenths ? fours[0] : triads[0]), s, -1, 0, {});

                for (int i = 0; i < 40; ++i)
                {
                    const Chord c = ch (rng.next (12), sevenths ? fours[rng.next (4)] : triads[rng.next (4)]);
                    const auto res = voiceChord (c, s, -1, 0, prev);
                    CHECK (std::abs (mean (res) - centre) <= 7.0 + kTol);
                    const int cost = movement (res, prev);

                    for (int inv = 0; inv < c.numTones(); ++inv)
                    {
                        const auto base = voiceChord (c, s, inv, 0, {});
                        for (int o = -3; o <= 3; ++o)
                        {
                            const auto cand = shifted (base, 12 * o);
                            if (! inMidiRange (cand) || std::abs (mean (cand) - centre) > 7.0 + kTol)
                                continue;
                            CHECK (cost <= movement (cand, prev));
                        }
                    }
                    prev = res;
                }
            }
}

TEST_CASE ("voicing: voice leading never drifts over 64 chords")
{
    // Circle of fourths is the classic drift trap: always moving "down a step" would walk off the keyboard.
    const ChordQuality qs[] = { ChordQuality::Major, ChordQuality::Minor7, ChordQuality::Dominant7,
                                ChordQuality::Major9, ChordQuality::Add9, ChordQuality::Sus4, ChordQuality::Minor };
    for (auto style : kStyles)
        for (int centre : kCentres)
        {
            Progression p;
            for (int i = 0; i < 64; ++i)
            {
                ProgressionSlot s;
                s.chord = ch ((i * 5) % 12, qs[i % 7]);
                p.slots.push_back (s);
            }
            const auto vs = voiceProgression (p, vset (style, centre));
            REQUIRE (vs.size() == 64);
            double lo = 1000.0, hi = -1000.0;
            for (auto& v : vs)
            {
                CHECK (std::abs (mean (v) - centre) <= 7.0 + kTol);
                lo = std::min (lo, mean (v));
                hi = std::max (hi, mean (v));
            }
            CHECK (hi - lo <= 14.0 + kTol);
        }

    // Descending chromatic too.
    std::vector<int> prev;
    for (int i = 0; i < 64; ++i)
    {
        prev = voiceChord (ch (pc (-i), ChordQuality::Major7), vset (VoicingStyle::Close, 60), -1, 0, prev);
        CHECK (std::abs (mean (prev) - 60.0) <= 7.0 + kTol);
    }
}

TEST_CASE ("voicing: voice leading off or explicit inversion ignores the previous voicing")
{
    const std::vector<int> prev { 59, 62, 67 };
    for (auto style : kStyles)
    {
        const Chord c = ch (5, ChordQuality::Major);
        CHECK_EQ (voiceChord (c, vset (style, 60, false), -1, 0, prev), voiceChord (c, vset (style, 60), 0, 0, {}));
        for (int inv = 0; inv < 3; ++inv)
            CHECK_EQ (voiceChord (c, vset (style, 60), inv, 0, prev), voiceChord (c, vset (style, 60), inv, 0, {}));
    }
}

TEST_CASE ("voicing: voice leading between chords of different sizes")
{
    const std::vector<Chord> chords { ch (0, ChordQuality::Major), ch (0, ChordQuality::Major9), ch (7, ChordQuality::Dominant7),
                                      ch (9, ChordQuality::Minor), ch (5, ChordQuality::Add9), ch (0, ChordQuality::Major) };
    for (auto style : kStyles)
    {
        const auto vs = voiceProgression (makeProgression (chords), vset (style, 60));
        REQUIRE (vs.size() == chords.size());
        for (size_t i = 0; i < vs.size(); ++i)
        {
            CHECK (ascendingUnique (vs[i]));
            CHECK (pcSet (vs[i]) == chordPcSet (chords[i]));
            CHECK (std::abs (mean (vs[i]) - 60.0) <= 7.0 + kTol);
        }
        // C -> Cmaj9 shares C E G: the common tones are kept where they are.
        if (style == VoicingStyle::Close)
            for (int common : vs[0])
                CHECK (std::find (vs[1].begin(), vs[1].end(), common) != vs[1].end());
    }
}

TEST_CASE ("voicing: voice leading never puts the 9th in the bass automatically")
{
    Lcg rng;
    for (auto style : kStyles)
    {
        std::vector<int> prev;
        for (int i = 0; i < 60; ++i)
        {
            const ChordQuality q = (ChordQuality) (8 + rng.next (2) * 9 + rng.next (2));   // Add9/MinorAdd9/Major9/Minor9
            const Chord c = ch (rng.next (12), q);
            prev = voiceChord (c, vset (style, 60), -1, 0, prev);
            CHECK (pc (prev.front()) != pc (c.root + 2));
        }
    }
}

TEST_CASE ("voicing: deterministic and root normalised")
{
    const std::vector<int> prev { 57, 60, 64, 67 };
    for (auto style : kStyles)
    {
        const auto a = voiceChord (ch (2, ChordQuality::Dominant9), vset (style), -1, 0, prev);
        const auto b = voiceChord (ch (2, ChordQuality::Dominant9), vset (style), -1, 0, prev);
        CHECK_EQ (a, b);
        CHECK_EQ (voiceChord (ch (12, ChordQuality::Major), vset (style), 0, 0, {}), voiceChord (ch (0, ChordQuality::Major), vset (style), 0, 0, {}));
        CHECK_EQ (voiceChord (ch (-1, ChordQuality::Minor), vset (style), 0, 0, {}), voiceChord (ch (11, ChordQuality::Minor), vset (style), 0, 0, {}));
    }
    // unsorted previous voicings are fine
    CHECK_EQ (voiceChord (ch (9, ChordQuality::Minor), vset (VoicingStyle::Close), -1, 0, { 67, 60, 64 }),
              (std::vector<int> { 60, 64, 69 }));
}

TEST_CASE ("voicing: voiceProgression chains voiceChord and honours slot inversion / octave")
{
    Progression p = makeProgression ({ ch (0, ChordQuality::Major), ch (9, ChordQuality::Minor7), ch (5, ChordQuality::Major7), ch (7, ChordQuality::Dominant7) });
    p.slots[1].inversion = 1;
    p.slots[2].octaveShift = 1;

    for (auto style : kStyles)
    {
        const auto s = vset (style, 60);
        const auto vs = voiceProgression (p, s);
        REQUIRE (vs.size() == 4);

        std::vector<int> prev;
        for (size_t i = 0; i < 4; ++i)
        {
            const auto expected = voiceChord (p.slots[i].chord, s, p.slots[i].inversion, p.slots[i].octaveShift, prev);
            CHECK_EQ (vs[i], expected);
            prev = expected;
        }

        CHECK_EQ (pc (vs[1].front()), 0);   // Am7 first inversion: C in the bass
        CHECK (std::abs (mean (vs[2]) - 72.0) <= 7.0 + kTol);   // octave up
    }

    CHECK (voiceProgression (Progression {}, vset (VoicingStyle::Close)).empty());
}

//==============================================================================
// Progression
//==============================================================================
TEST_CASE ("progression: beats and slot lookup")
{
    const auto p = makeProgression ({ ch (0, ChordQuality::Major), ch (9, ChordQuality::Minor), ch (5, ChordQuality::Major) }, { 4, 2, 3 });
    CHECK_EQ (p.totalBeats(), 9);
    CHECK_EQ (p.slotStartBeat (0), 0.0);
    CHECK_EQ (p.slotStartBeat (1), 4.0);
    CHECK_EQ (p.slotStartBeat (2), 6.0);
    CHECK_EQ (p.slotStartBeat (3), 9.0);
    CHECK_EQ (p.slotAtBeat (0.0), 0);
    CHECK_EQ (p.slotAtBeat (3.99), 0);
    CHECK_EQ (p.slotAtBeat (4.0), 1);
    CHECK_EQ (p.slotAtBeat (8.5), 2);
    CHECK_EQ (p.slotAtBeat (9.0), -1);
    CHECK_EQ (p.slotAtBeat (-0.1), -1);
    CHECK_EQ (Progression {}.slotAtBeat (0.0), -1);
    CHECK_EQ (Progression {}.totalBeats(), 0);
}

//==============================================================================
// Pattern
//==============================================================================
TEST_CASE ("pattern: names and rates")
{
    CHECK (std::strcmp (rhythmPatternName (RhythmPattern::ArpUpDown), "Arp Up/Down") == 0);
    CHECK (std::strcmp (rhythmPatternName (RhythmPattern::Broken), "Broken") == 0);
    CHECK (std::strcmp (bassModeName (BassMode::RootFifth), "Root + 5th") == 0);
    CHECK (std::strcmp (rateDivisionName (RateDivision::EighthTriplet), "1/8T") == 0);
    CHECK_EQ (rateInBeats (RateDivision::Quarter), 1.0);
    CHECK_EQ (rateInBeats (RateDivision::Eighth), 0.5);
    CHECK_EQ (rateInBeats (RateDivision::Sixteenth), 0.25);
    CHECK (near (rateInBeats (RateDivision::EighthTriplet), 1.0 / 3.0));
}

TEST_CASE ("pattern: empty progression renders nothing")
{
    for (int p = 0; p < numRhythmPatterns; ++p)
        for (int b = 0; b < numBassModes; ++b)
            CHECK (renderProgression (Progression {}, {}, rset ((RhythmPattern) p, RateDivision::Eighth, (BassMode) b)).empty());
}

TEST_CASE ("pattern: block holds the chord for gate * slot")
{
    const auto p = makeProgression ({ ch (0, ChordQuality::Major), ch (9, ChordQuality::Minor) }, { 4, 2 });
    const auto ev = renderProgression (p, { { 60, 64, 67 }, { 57, 60, 64 } }, rset (RhythmPattern::Block));
    REQUIRE (ev.size() == 6);
    for (int i = 0; i < 3; ++i)
    {
        CHECK_EQ (ev[(size_t) i].startBeat, 0.0);
        CHECK (near (ev[(size_t) i].lengthBeats, 3.6, 1e-6));
        CHECK_EQ (ev[(size_t) i].channel, 1);
        CHECK_EQ (ev[(size_t) i].velocity, 0.75f);
    }
    for (int i = 3; i < 6; ++i)
    {
        CHECK_EQ (ev[(size_t) i].startBeat, 4.0);
        CHECK (near (ev[(size_t) i].lengthBeats, 1.8, 1e-6));
    }
    CHECK_EQ (ev[0].note, 60);
    CHECK_EQ (ev[3].note, 57);
}

TEST_CASE ("pattern: strum staggers notes upward by 0.03 beats")
{
    const auto ev = renderOne (rset (RhythmPattern::Strum), kCMaj7);
    REQUIRE (ev.size() == 4);
    for (int i = 0; i < 4; ++i)
    {
        CHECK_EQ (ev[(size_t) i].note, kCMaj7[(size_t) i]);
        CHECK (near (ev[(size_t) i].startBeat, 0.03 * i));
        CHECK (near (ev[(size_t) i].lengthBeats, 3.6, 1e-6));
    }

    auto s = rset (RhythmPattern::Strum);
    s.gate = 1.0f;
    const auto full = renderOne (s, kCMaj7);
    REQUIRE (full.size() == 4);
    for (auto& e : full)
        CHECK (e.startBeat + e.lengthBeats <= 4.0 + kTol);   // clipped to the slot
    CHECK (near (full[3].lengthBeats, 4.0 - 0.09, 1e-9));
}

TEST_CASE ("pattern: pulse retriggers the chord on every rate step")
{
    struct Case { RateDivision rate; int steps; double step; };
    for (auto c : { Case { RateDivision::Quarter, 4, 1.0 }, Case { RateDivision::Eighth, 8, 0.5 },
                    Case { RateDivision::Sixteenth, 16, 0.25 }, Case { RateDivision::EighthTriplet, 12, 1.0 / 3.0 } })
    {
        const auto ev = renderOne (rset (RhythmPattern::Pulse, c.rate));
        REQUIRE ((int) ev.size() == c.steps * 3);
        for (int k = 0; k < c.steps; ++k)
            for (int i = 0; i < 3; ++i)
            {
                const auto& e = ev[(size_t) (k * 3 + i)];
                CHECK (near (e.startBeat, k * c.step));
                CHECK (near (e.lengthBeats, 0.9 * c.step, 1e-6));
                CHECK_EQ (e.note, kCMajor[(size_t) i]);
            }
    }
}

TEST_CASE ("pattern: offbeat hits the & of every beat")
{
    const auto ev = renderOne (rset (RhythmPattern::Offbeat, RateDivision::Sixteenth));
    REQUIRE (ev.size() == 12);
    for (int b = 0; b < 4; ++b)
        for (int i = 0; i < 3; ++i)
        {
            const auto& e = ev[(size_t) (b * 3 + i)];
            CHECK (near (e.startBeat, b + 0.5));
            CHECK (near (e.lengthBeats, 0.45, 1e-6));
        }
}

TEST_CASE ("pattern: arp up and arp down order")
{
    const auto up = renderOne (rset (RhythmPattern::ArpUp, RateDivision::Eighth), kCMaj7);
    REQUIRE (up.size() == 8);
    for (int k = 0; k < 8; ++k)
    {
        CHECK_EQ (up[(size_t) k].note, kCMaj7[(size_t) (k % 4)]);
        CHECK (near (up[(size_t) k].startBeat, 0.5 * k));
        CHECK (near (up[(size_t) k].lengthBeats, 0.45, 1e-6));
    }

    const auto down = renderOne (rset (RhythmPattern::ArpDown, RateDivision::Eighth), kCMaj7);
    REQUIRE (down.size() == 8);
    for (int k = 0; k < 8; ++k)
        CHECK_EQ (down[(size_t) k].note, kCMaj7[(size_t) (3 - k % 4)]);
}

TEST_CASE ("pattern: arp up/down turnaround does not repeat end notes")
{
    const auto ev = renderOne (rset (RhythmPattern::ArpUpDown, RateDivision::Sixteenth), kCMaj7);
    REQUIRE (ev.size() == 16);
    const int expected[] = { 0, 1, 2, 3, 2, 1, 0, 1, 2, 3, 2, 1, 0, 1, 2, 3 };
    for (int k = 0; k < 16; ++k)
    {
        CHECK_EQ (ev[(size_t) k].note, kCMaj7[(size_t) expected[k]]);
        CHECK (near (ev[(size_t) k].startBeat, 0.25 * k));
    }

    const auto three = renderOne (rset (RhythmPattern::ArpUpDown, RateDivision::Eighth), kCMajor);
    REQUIRE (three.size() == 8);
    const int exp3[] = { 0, 1, 2, 1, 0, 1, 2, 1 };
    for (int k = 0; k < 8; ++k)
        CHECK_EQ (three[(size_t) k].note, kCMajor[(size_t) exp3[k]]);
}

TEST_CASE ("pattern: arps with one or two notes")
{
    const auto two = renderOne (rset (RhythmPattern::ArpUpDown, RateDivision::Quarter), { 60, 67 });
    REQUIRE (two.size() == 4);
    CHECK_EQ (two[0].note, 60);
    CHECK_EQ (two[1].note, 67);
    CHECK_EQ (two[2].note, 60);
    CHECK_EQ (two[3].note, 67);

    for (auto p : { RhythmPattern::ArpUp, RhythmPattern::ArpDown, RhythmPattern::ArpUpDown })
    {
        const auto one = renderOne (rset (p, RateDivision::Quarter), { 62 });
        REQUIRE (one.size() == 4);
        for (auto& e : one)
            CHECK_EQ (e.note, 62);
    }
}

TEST_CASE ("pattern: arp, pulse and broken steps restart at every slot start")
{
    const auto p = makeProgression ({ ch (0, ChordQuality::Major7), ch (5, ChordQuality::Major7) }, { 3, 3 });
    const std::vector<std::vector<int>> v { { 60, 64, 67, 71 }, { 60, 64, 65, 69 } };

    const auto up = renderProgression (p, v, rset (RhythmPattern::ArpUp, RateDivision::Quarter));
    REQUIRE (up.size() == 6);
    CHECK_EQ (up[2].note, 67);   // slot 1 ends on its third note...
    CHECK_EQ (up[3].note, 60);   // ...slot 2 starts again from its lowest
    CHECK (near (up[3].startBeat, 3.0));

    const auto down = renderProgression (p, v, rset (RhythmPattern::ArpDown, RateDivision::Quarter));
    REQUIRE (down.size() == 6);
    CHECK_EQ (down[3].note, 69);

    // Odd-length slot with swing: slot 2's first step is an even step again (on the beat, not swung).
    auto s = rset (RhythmPattern::Pulse, RateDivision::Quarter);
    s.swing = 1.0f;
    const auto pulse = renderProgression (makeProgression ({ ch (0, ChordQuality::Major), ch (5, ChordQuality::Major) }, { 3, 3 }),
                                          { kCMajor, { 60, 65, 69 } }, s);
    bool slot2Start = false;
    for (auto& e : pulse)
        if (near (e.startBeat, 3.0))
            slot2Start = true;
    CHECK (slot2Start);
}

TEST_CASE ("pattern: broken plays the low note on the beat and the rest after it")
{
    {
        const auto ev = renderOne (rset (RhythmPattern::Broken, RateDivision::Eighth), kCMajor, 2);
        REQUIRE (ev.size() == 6);
        const double starts[] = { 0.0, 0.5, 0.5, 1.0, 1.5, 1.5 };
        const int notes[] = { 60, 64, 67, 60, 64, 67 };
        for (int i = 0; i < 6; ++i)
        {
            CHECK (near (ev[(size_t) i].startBeat, starts[i]));
            CHECK_EQ (ev[(size_t) i].note, notes[i]);
            CHECK (near (ev[(size_t) i].lengthBeats, 0.45, 1e-6));
        }
    }
    {
        const auto ev = renderOne (rset (RhythmPattern::Broken, RateDivision::Sixteenth), kCMajor, 1);
        REQUIRE (ev.size() == 7);
        CHECK_EQ (ev[0].note, 60);
        CHECK (near (ev[0].startBeat, 0.0));
        for (int j = 0; j < 3; ++j)
        {
            CHECK (near (ev[(size_t) (1 + 2 * j)].startBeat, 0.25 * (j + 1)));
            CHECK_EQ (ev[(size_t) (1 + 2 * j)].note, 64);
            CHECK_EQ (ev[(size_t) (2 + 2 * j)].note, 67);
        }
    }
    {
        const auto ev = renderOne (rset (RhythmPattern::Broken, RateDivision::EighthTriplet), kCMajor, 1);
        REQUIRE (ev.size() == 5);
        CHECK (near (ev[1].startBeat, 1.0 / 3.0));
        CHECK (near (ev[3].startBeat, 2.0 / 3.0));
    }
}

TEST_CASE ("pattern: broken at 1/4 subdivides the beat in eighths")
{
    const auto quarter = renderOne (rset (RhythmPattern::Broken, RateDivision::Quarter), kCMajor, 4);
    const auto eighth = renderOne (rset (RhythmPattern::Broken, RateDivision::Eighth), kCMajor, 4);
    REQUIRE (quarter.size() == 12);
    CHECK (quarter == eighth);
}

TEST_CASE ("pattern: gate scales note lengths and is clamped")
{
    auto s = rset (RhythmPattern::Pulse, RateDivision::Eighth);
    s.gate = 0.25f;
    for (auto& e : renderOne (s))
        CHECK (near (e.lengthBeats, 0.125, 1e-6));

    s.gate = 0.0f;   // clamped to 0.05
    for (auto& e : renderOne (s))
        CHECK (near (e.lengthBeats, 0.025, 1e-6));

    s.gate = 3.0f;   // clamped to 1
    for (auto& e : renderOne (s))
        CHECK (near (e.lengthBeats, 0.5, 1e-6));

    auto b = rset (RhythmPattern::Block, RateDivision::Eighth, BassMode::Root);
    b.gate = 0.5f;
    for (auto& e : renderOne (b, kCMajor, 2))
        CHECK (near (e.lengthBeats, 1.0, 1e-6));
}

TEST_CASE ("pattern: swing only moves second steps")
{
    struct Case { RhythmPattern pattern; RateDivision rate; double step; };
    for (auto c : { Case { RhythmPattern::Pulse, RateDivision::Eighth, 0.5 }, Case { RhythmPattern::ArpUp, RateDivision::Sixteenth, 0.25 },
                    Case { RhythmPattern::ArpUpDown, RateDivision::Quarter, 1.0 }, Case { RhythmPattern::Pulse, RateDivision::Sixteenth, 0.25 } })
        for (float swing : { 0.3f, 0.6f, 1.0f })
        {
            auto straight = rset (c.pattern, c.rate);
            auto swung = straight;
            swung.swing = swing;
            const auto a = renderOne (straight, kCMaj7);
            const auto b = renderOne (swung, kCMaj7);
            REQUIRE (a.size() == b.size());
            const double delay = swing * c.step / 3.0;

            for (size_t i = 0; i < a.size(); ++i)
            {
                const int k = (int) std::lround (a[i].startBeat / c.step);
                CHECK_EQ (a[i].note, b[i].note);
                if (k % 2 == 0)
                {
                    CHECK (near (b[i].startBeat, a[i].startBeat));
                    CHECK (near (b[i].lengthBeats, 0.9 * (c.step + delay), 1e-6));
                }
                else
                {
                    CHECK (near (b[i].startBeat, a[i].startBeat + delay, 1e-6));
                    CHECK (near (b[i].lengthBeats, 0.9 * (c.step - delay), 1e-6));
                }
            }
        }

    // swing 1 at 1/8 = triplet shuffle: the "&" lands on the third triplet
    auto s = rset (RhythmPattern::Pulse, RateDivision::Eighth);
    s.swing = 1.0f;
    const auto ev = renderOne (s);
    CHECK (near (ev[3].startBeat, 2.0 / 3.0, 1e-9));
}

TEST_CASE ("pattern: swing does not affect block, strum or triplets")
{
    for (auto p : { RhythmPattern::Block, RhythmPattern::Strum })
    {
        auto s = rset (p);
        const auto a = renderOne (s);
        s.swing = 1.0f;
        CHECK (renderOne (s) == a);
    }
    for (auto p : { RhythmPattern::Pulse, RhythmPattern::ArpUp, RhythmPattern::Broken })
    {
        auto s = rset (p, RateDivision::EighthTriplet, BassMode::Pulse);
        const auto a = renderOne (s);
        s.swing = 1.0f;
        CHECK (renderOne (s) == a);
    }
}

TEST_CASE ("pattern: offbeat swings on the 1/8 grid only")
{
    auto s = rset (RhythmPattern::Offbeat, RateDivision::Eighth);
    s.swing = 1.0f;
    const auto ev = renderOne (s);
    REQUIRE (ev.size() == 12);
    for (int b = 0; b < 4; ++b)
        CHECK (near (ev[(size_t) (b * 3)].startBeat, b + 0.5 + 1.0 / 6.0, 1e-9));

    s.rate = RateDivision::Sixteenth;   // the & is an even 1/16 step: not swung
    const auto straight = renderOne (s);
    for (int b = 0; b < 4; ++b)
        CHECK (near (straight[(size_t) (b * 3)].startBeat, b + 0.5));
}

TEST_CASE ("pattern: broken swings its after-beat eighth")
{
    auto s = rset (RhythmPattern::Broken, RateDivision::Eighth);
    s.swing = 0.6f;
    const auto ev = renderOne (s, kCMajor, 2);
    REQUIRE (ev.size() == 6);
    CHECK (near (ev[0].startBeat, 0.0));
    CHECK (near (ev[1].startBeat, 0.6, 1e-6));
    CHECK (near (ev[3].startBeat, 1.0));
}

TEST_CASE ("pattern: humanize is deterministic")
{
    for (int p = 0; p < numRhythmPatterns; ++p)
    {
        auto s = rset ((RhythmPattern) p, RateDivision::Sixteenth, BassMode::Octaves);
        s.humanize = 0.8f;
        const auto a = renderOne (s, kCMaj7);
        const auto b = renderOne (s, kCMaj7);
        CHECK (a == b);
    }
}

TEST_CASE ("pattern: humanize is bounded and actually varies")
{
    for (int p = 0; p < numRhythmPatterns; ++p)
        for (float h : { 0.5f, 1.0f })
        {
            auto s = rset ((RhythmPattern) p, RateDivision::Eighth, BassMode::Pulse);
            const auto plain = renderOne (s, kCMaj7);
            s.humanize = h;
            const auto human = renderOne (s, kCMaj7);
            REQUIRE (plain.size() == human.size());

            auto byNote = [] (std::vector<NoteEvent> v)
            {
                std::stable_sort (v.begin(), v.end(), [] (const NoteEvent& a, const NoteEvent& b)
                {
                    if (a.channel != b.channel) return a.channel < b.channel;
                    if (a.note != b.note) return a.note < b.note;
                    return a.startBeat < b.startBeat;
                });
                return v;
            };
            const auto x = byNote (plain);
            const auto y = byNote (human);
            int differing = 0;
            for (size_t i = 0; i < x.size(); ++i)
            {
                CHECK_EQ (x[i].note, y[i].note);
                CHECK (std::abs (x[i].startBeat - y[i].startBeat) <= 0.015 * h + 1e-9);
                CHECK (std::abs (x[i].velocity - y[i].velocity) <= 0.12f * h + 1e-5f);
                CHECK (y[i].startBeat >= 0.0);
                CHECK (y[i].startBeat + y[i].lengthBeats <= 4.0 + kTol);
                if (x[i].startBeat != y[i].startBeat || x[i].velocity != y[i].velocity)
                    ++differing;
            }
            CHECK (differing > (int) x.size() / 2);
        }
}

TEST_CASE ("pattern: velocities stay in (0, 1]")
{
    for (float vel : { 0.0f, 0.01f, 0.5f, 1.0f, 2.0f })
        for (float h : { 0.0f, 1.0f })
        {
            auto s = rset (RhythmPattern::Pulse, RateDivision::Sixteenth, BassMode::Pulse);
            s.velocity = vel;
            s.humanize = h;
            const auto ev = renderOne (s, kCMaj7, 8);
            REQUIRE (! ev.empty());
            for (auto& e : ev)
            {
                CHECK (e.velocity > 0.0f);
                CHECK (e.velocity <= 1.0f);
            }
        }

    auto s = rset (RhythmPattern::Block, RateDivision::Eighth, BassMode::Root);
    s.velocity = 0.6f;
    for (auto& e : renderOne (s))
        CHECK_EQ (e.velocity, 0.6f);   // bass uses the same base velocity
}

TEST_CASE ("pattern: bass root sits below the voicing and inside 28..55")
{
    struct Case { int root; std::vector<int> voicing; int expected; };
    const Case cases[] = {
        { 0, { 60, 64, 67 }, 48 },   // window [48, 59]
        { 9, { 57, 60, 64 }, 45 },   // [45, 56]
        { 5, { 65, 69, 72 }, 53 },   // [53, 64]
        { 7, { 67, 71, 74 }, 55 },   // [55, 66]
        { 9, { 69, 72, 76 }, 45 },   // [57, 68] -> 57 > 55 -> 45
        { 0, { 36, 40, 43 }, 36 },   // [24, 35] -> 24 < 28 -> 36
        { 0, { 84, 88, 91 }, 48 },   // [72, 83] -> 72 -> 48
        { 2, { 62, 65, 69, 72 }, 50 },
    };
    for (auto& c : cases)
    {
        const auto ev = channel (renderOne (rset (RhythmPattern::Block, RateDivision::Eighth, BassMode::Root), c.voicing, 4,
                                            Chord { c.root, ChordQuality::Major }), 2);
        REQUIRE (ev.size() == 1);
        CHECK_EQ (ev[0].note, c.expected);
    }
}

TEST_CASE ("pattern: bass Root holds the root for gate * slot on channel 2")
{
    const auto p = makeProgression ({ ch (0, ChordQuality::Major), ch (7, ChordQuality::Major) }, { 4, 2 });
    const auto ev = channel (renderProgression (p, { kCMajor, { 59, 62, 67 } }, rset (RhythmPattern::Block, RateDivision::Eighth, BassMode::Root)), 2);
    REQUIRE (ev.size() == 2);
    CHECK_EQ (ev[0].note, 48);
    CHECK_EQ (ev[0].startBeat, 0.0);
    CHECK (near (ev[0].lengthBeats, 3.6, 1e-6));
    CHECK_EQ (ev[1].note, 55);   // G in [47, 58]
    CHECK_EQ (ev[1].startBeat, 4.0);
    CHECK (near (ev[1].lengthBeats, 1.8, 1e-6));
}

TEST_CASE ("pattern: bass Root + 5th alternates every beat starting with the root")
{
    const auto ev = channel (renderOne (rset (RhythmPattern::Block, RateDivision::Sixteenth, BassMode::RootFifth)), 2);
    REQUIRE (ev.size() == 4);
    for (int b = 0; b < 4; ++b)
    {
        CHECK (near (ev[(size_t) b].startBeat, (double) b));
        CHECK_EQ (ev[(size_t) b].note, b % 2 == 0 ? 48 : 55);
        CHECK (near (ev[(size_t) b].lengthBeats, 0.9, 1e-6));
    }

    // G root at 55 would put its 5th at 62: the figure moves down an octave.
    const auto g = channel (renderOne (rset (RhythmPattern::Block, RateDivision::Eighth, BassMode::RootFifth), { 67, 71, 74 }, 2,
                                       Chord { 7, ChordQuality::Major }), 2);
    REQUIRE (g.size() == 2);
    CHECK_EQ (g[0].note, 43);
    CHECK_EQ (g[1].note, 50);
}

TEST_CASE ("pattern: bass Octaves alternates low/high every step")
{
    const auto ev = channel (renderOne (rset (RhythmPattern::Block, RateDivision::Eighth, BassMode::Octaves)), 2);
    REQUIRE (ev.size() == 8);
    for (int k = 0; k < 8; ++k)
    {
        CHECK (near (ev[(size_t) k].startBeat, 0.5 * k));
        CHECK_EQ (ev[(size_t) k].note, k % 2 == 0 ? 36 : 48);
        CHECK (near (ev[(size_t) k].lengthBeats, 0.45, 1e-6));
    }
}

TEST_CASE ("pattern: bass Pulse retriggers the root every step")
{
    const auto ev = channel (renderOne (rset (RhythmPattern::Block, RateDivision::EighthTriplet, BassMode::Pulse)), 2);
    REQUIRE (ev.size() == 12);
    for (int k = 0; k < 12; ++k)
    {
        CHECK (near (ev[(size_t) k].startBeat, k / 3.0));
        CHECK_EQ (ev[(size_t) k].note, 48);
    }
}

TEST_CASE ("pattern: bass Off renders no bass")
{
    for (int p = 0; p < numRhythmPatterns; ++p)
        CHECK (channel (renderOne (rset ((RhythmPattern) p, RateDivision::Eighth, BassMode::Off)), 2).empty());
}

TEST_CASE ("pattern: bass notes stay in 28..55 for every mode, chord and register")
{
    for (int b = 1; b < numBassModes; ++b)
        for (int root = 0; root < 12; ++root)
            for (int centre = 24; centre <= 96; centre += 6)
                for (auto style : kStyles)
                {
                    const Chord c = ch (root, ChordQuality::Dominant7);
                    const auto v = voiceChord (c, vset (style, centre), -1, 0, {});
                    const auto ev = channel (renderOne (rset (RhythmPattern::Block, RateDivision::Sixteenth, (BassMode) b), v, 4, c), 2);
                    REQUIRE (! ev.empty());
                    for (auto& e : ev)
                    {
                        CHECK (e.note >= 28);
                        CHECK (e.note <= 55);
                        CHECK_EQ (e.channel, 2);
                        CHECK (pc (e.note) == root || pc (e.note) == pc (root + 7));
                    }
                }
}

TEST_CASE ("pattern: sweep - events stay in their slot, sorted, no same-note overlaps")
{
    const std::vector<Chord> chords { ch (0, ChordQuality::Major9), ch (9, ChordQuality::Minor), ch (5, ChordQuality::Add9),
                                      ch (7, ChordQuality::Dominant7), ch (2, ChordQuality::Sus2) };
    const auto prog = makeProgression (chords, { 1, 3, 4, 2, 5 });

    int renders = 0;
    for (auto style : kStyles)
    {
        const auto voicings = voiceProgression (prog, vset (style, 60));
        for (int p = 0; p < numRhythmPatterns; ++p)
            for (int r = 0; r < numRateDivisions; ++r)
                for (int b = 0; b < numBassModes; ++b)
                    for (float swing : { 0.0f, 0.5f, 1.0f })
                        for (float h : { 0.0f, 1.0f })
                            for (float gate : { 0.05f, 0.5f, 1.0f })
                            {
                                RenderSettings s = rset ((RhythmPattern) p, (RateDivision) r, (BassMode) b);
                                s.swing = swing;
                                s.humanize = h;
                                s.gate = gate;
                                const auto ev = renderProgression (prog, voicings, s);
                                ++renders;
                                REQUIRE (! ev.empty());

                                for (size_t i = 0; i < ev.size(); ++i)
                                {
                                    const auto& e = ev[i];
                                    const int slot = prog.slotAtBeat (e.startBeat);
                                    REQUIRE (slot >= 0);
                                    CHECK (e.startBeat >= prog.slotStartBeat (slot) - kTol);
                                    CHECK (e.startBeat + e.lengthBeats <= prog.slotStartBeat (slot + 1) + kTol);
                                    CHECK (e.lengthBeats > 0.0);
                                    CHECK (e.velocity > 0.0f && e.velocity <= 1.0f);
                                    CHECK (e.note >= 0 && e.note <= 127);
                                    CHECK (e.channel == 1 || e.channel == 2);

                                    if (i > 0)
                                    {
                                        const auto& a = ev[i - 1];
                                        const bool ordered = a.startBeat < e.startBeat
                                                          || (a.startBeat == e.startBeat && (a.channel < e.channel
                                                              || (a.channel == e.channel && a.note < e.note)));
                                        CHECK (ordered);
                                    }

                                    for (size_t j = i + 1; j < ev.size(); ++j)
                                        if (ev[j].channel == e.channel && ev[j].note == e.note)
                                        {
                                            CHECK (e.startBeat + e.lengthBeats <= ev[j].startBeat + kTol);
                                            break;
                                        }
                                }
                            }
    }
    CHECK_EQ (renders, 3 * numRhythmPatterns * numRateDivisions * numBassModes * 3 * 2 * 3);
}

TEST_CASE ("pattern: pattern event counts per slot")
{
    // 4-beat slot, 3-note chord
    struct Case { RhythmPattern p; RateDivision r; size_t count; };
    const Case cases[] = {
        { RhythmPattern::Block, RateDivision::Eighth, 3 },       { RhythmPattern::Strum, RateDivision::Sixteenth, 3 },
        { RhythmPattern::Pulse, RateDivision::Quarter, 12 },     { RhythmPattern::Offbeat, RateDivision::Eighth, 12 },
        { RhythmPattern::ArpUp, RateDivision::Sixteenth, 16 },   { RhythmPattern::ArpDown, RateDivision::EighthTriplet, 12 },
        { RhythmPattern::ArpUpDown, RateDivision::Eighth, 8 },   { RhythmPattern::Broken, RateDivision::Eighth, 12 },
        { RhythmPattern::Broken, RateDivision::Sixteenth, 28 },  { RhythmPattern::Broken, RateDivision::EighthTriplet, 20 },
    };
    for (auto& c : cases)
        CHECK_EQ (renderOne (rset (c.p, c.r)).size(), c.count);
}

TEST_CASE ("pattern: missing or empty voicings render safely")
{
    const auto p = makeProgression ({ ch (0, ChordQuality::Major), ch (7, ChordQuality::Major) }, { 4, 4 });
    for (int pat = 0; pat < numRhythmPatterns; ++pat)
    {
        const auto none = renderProgression (p, {}, rset ((RhythmPattern) pat, RateDivision::Eighth, BassMode::Root));
        CHECK (channel (none, 1).empty());
        REQUIRE (channel (none, 2).size() == 2);
        for (auto& e : none)
            CHECK (e.note >= 28 && e.note <= 55);

        const auto partial = renderProgression (p, { kCMajor }, rset ((RhythmPattern) pat, RateDivision::Eighth, BassMode::Off));
        for (auto& e : partial)
            CHECK (e.startBeat < 4.0);
    }

    // zero-length slots are skipped
    Progression bad = p;
    bad.slots[0].lengthBeats = 0;
    const auto ev = renderProgression (bad, { kCMajor, { 59, 62, 67 } }, rset (RhythmPattern::Block, RateDivision::Eighth, BassMode::Off));
    REQUIRE (ev.size() == 3);
    CHECK_EQ (ev[0].note, 59);
    CHECK_EQ (ev[0].startBeat, 0.0);
}

TEST_CASE ("pattern: end to end I-vi-IV-V with default settings")
{
    const auto p = makeProgression ({ ch (0, ChordQuality::Major), ch (9, ChordQuality::Minor), ch (5, ChordQuality::Major), ch (7, ChordQuality::Major) });
    const auto voicings = voiceProgression (p, VoicingSettings {});
    const auto ev = renderProgression (p, voicings, RenderSettings {});
    CHECK_EQ (ev.size(), (size_t) 16);   // Block: 3 chord notes + 1 bass note per slot
    CHECK_EQ (channel (ev, 2).size(), (size_t) 4);
    CHECK_EQ (channel (ev, 2)[0].note, 48);
    CHECK_EQ (channel (ev, 2)[1].note, 45);
    CHECK_EQ (channel (ev, 2)[2].note, 53);
    CHECK_EQ (channel (ev, 2)[3].note, 55);
    for (auto& e : ev)
        CHECK (e.startBeat + e.lengthBeats <= p.slotStartBeat (p.slotAtBeat (e.startBeat) + 1) + kTol);
}
