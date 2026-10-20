#include "TestFramework.h"

#include "core/MelodyGen.h"
#include "core/Voicing.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

// Melody generator (owner: WP1; final-spec 4.6, 5.4, 9 WP1). WP0 seeded this file with checks of the frozen names and
// the mood table; WP1 adds the generator tests. Contexts are built only from core functions (voiceProgression,
// renderProgression with classic bass modes) and hand-written channel-4 events; nothing here reads the MIDI corpus.

using namespace cb;

TEST_CASE ("melody gen: WP0 names, mood table and settings equality")
{
    CHECK_EQ (std::string (melodyStyleName (MelodyStyle::Uplifting)), std::string ("Uplifting lead"));
    CHECK_EQ (std::string (melodyStyleShortName (MelodyStyle::Driving)), std::string ("Driving"));
    CHECK_EQ (std::string (melodyContourName (MelodyContour::Wave)), std::string ("Wave"));
    CHECK (melodyStyleForMood (Mood::Dark) == MelodyStyle::Driving);
    CHECK (melodyStyleForMood (Mood::Dreamy) == MelodyStyle::Sparse);
    CHECK (melodyStyleForMood (Mood::LoFi) == MelodyStyle::Lyrical);

    MelodyGenSettings a, b;
    CHECK (a == b);
    b.contour = MelodyContour::Arch;
    CHECK (a != b);
}

namespace
{
using Q = ChordQuality;

constexpr MelodyStyle kAllStyles[] = { MelodyStyle::Uplifting, MelodyStyle::Lyrical, MelodyStyle::Driving, MelodyStyle::Sparse };
constexpr MelodyContour kAllContours[] = { MelodyContour::Auto, MelodyContour::Rising, MelodyContour::Falling,
                                           MelodyContour::Arch, MelodyContour::Wave };

struct SlotSpec
{
    int root;
    Q quality;
    int beats;
    int inversion = -1;
};

Progression prog (std::initializer_list<SlotSpec> specs)
{
    Progression p;
    for (const auto& s : specs)
    {
        ProgressionSlot slot;
        slot.chord = Chord { s.root, s.quality };
        slot.lengthBeats = s.beats;
        slot.inversion = s.inversion;
        p.slots.push_back (slot);
    }
    return p;
}

Progression repeated (const Progression& p, int times)
{
    Progression out;
    for (int i = 0; i < times; ++i)
        out.slots.insert (out.slots.end(), p.slots.begin(), p.slots.end());
    return out;
}

const Key kCMajor { 0, ScaleType::Major };
const Key kAMinor { 9, ScaleType::NaturalMinor };

Progression popAxis() { return prog ({ { 0, Q::Major, 4 }, { 7, Q::Major, 4 }, { 9, Q::Minor, 4 }, { 5, Q::Major, 4 } }); }
Progression epicMinor() { return prog ({ { 9, Q::Minor, 4 }, { 5, Q::Major, 4 }, { 0, Q::Major, 4 }, { 7, Q::Major, 4 } }); }
Progression twoBeat()
{
    return prog ({ { 9, Q::Minor, 2 }, { 5, Q::Major, 2 }, { 0, Q::Major, 2 }, { 7, Q::Major, 2 },
                   { 9, Q::Minor, 2 }, { 2, Q::Minor, 2 }, { 4, Q::Minor, 2 }, { 9, Q::Minor, 2 } });
}
Progression mixedLengths() { return prog ({ { 0, Q::Major9, 1 }, { 2, Q::Minor, 2 }, { 5, Q::Add9, 3 }, { 7, Q::Dominant7, 5 } }); }
Progression oddTotal() { return prog ({ { 0, Q::Major, 4 }, { 9, Q::Minor7, 4 }, { 5, Q::Major7, 4 }, { 7, Q::Sus4, 2 } }); }
Progression oneSlot() { return prog ({ { 9, Q::Minor, 4 } }); }
Progression sixtyFourBars()
{
    Progression p;
    const SlotSpec cycle[] = { { 9, Q::Minor, 16 }, { 5, Q::Major, 16 }, { 0, Q::Major, 16 }, { 7, Q::Major, 16 } };
    for (int i = 0; i < 16; ++i)
    {
        ProgressionSlot s;
        s.chord = Chord { cycle[i % 4].root, cycle[i % 4].quality };
        s.lengthBeats = 16;
        p.slots.push_back (s);
    }
    return p;
}
Progression slashAndShort()
{
    // C/E (1st inversion) - Am7 - F/C - G7/F - Dm (explicit root position), slots of 1..3 beats
    return prog ({ { 0, Q::Major, 3, 1 }, { 9, Q::Minor7, 2 }, { 5, Q::Major, 1, 2 }, { 7, Q::Dominant7, 3, 3 },
                   { 2, Q::Minor, 2, 0 }, { 0, Q::Major, 1 } });
}
Progression secondaryBorrowed()
{
    // C major: C - D7 - G - E7 - Am - Fm - C - Bb
    return prog ({ { 0, Q::Major, 4 }, { 2, Q::Dominant7, 4 }, { 7, Q::Major, 4 }, { 4, Q::Dominant7, 4 },
                   { 9, Q::Minor, 4 }, { 5, Q::Minor, 4 }, { 0, Q::Major, 4 }, { 10, Q::Major, 4 } });
}

MelodyGenContext contextFor (const Progression& p, bool withAccompaniment = true)
{
    MelodyGenContext c;
    c.voicings = voiceProgression (p, VoicingSettings {});
    if (withAccompaniment)
    {
        RenderSettings rs;
        c.accompaniment = renderProgression (p, c.voicings, rs);
    }
    return c;
}

MelodyGenSettings settingsFor (MelodyStyle style, float density = 0.5f, int shift = 0,
                               MelodyContour contour = MelodyContour::Auto, float repetition = 0.5f, bool resolve = true)
{
    MelodyGenSettings s;
    s.style = style;
    s.density = density;
    s.registerShift = shift;
    s.contour = contour;
    s.repetition = repetition;
    s.resolveEnding = resolve;
    return s;
}

bool onGrid16 (double beat) { return std::abs (beat * 4.0 - std::round (beat * 4.0)) < 1.0e-9; }

std::vector<double> onsets (const Melody& m)
{
    std::vector<double> v;
    for (const auto& n : m.notes)
        v.push_back (n.startBeat);
    return v;
}

int countDistinctLetters (const std::string& signature)
{
    const auto a = signature.find (' ');
    const auto b = signature.find (' ', a + 1);
    const auto letters = signature.substr (a + 1, b - a - 1);
    int mask = 0, n = 0;
    for (char c : letters)
        if ((mask & (1 << (c - 'A'))) == 0)
        {
            mask |= 1 << (c - 'A');
            ++n;
        }
    return n;
}

// Every invariant of the header comment; returns false (and reports) on the first failure.
bool checkInvariants (const Melody& m, const Progression& p, const MelodyGenContext& ctx, const MelodyGenSettings& s)
{
    const double total = p.totalBeats();
    const auto window = melodyRegisterWindow (p, ctx, s);
    bool ok = ! m.empty() && (int) m.notes.size() <= kMaxMelodyNotes && m == sanitiseMelody (m);
    for (const auto& n : m.notes)
    {
        ok = ok && n.startBeat >= 0.0 && n.startBeat < total && onGrid16 (n.startBeat);
        ok = ok && n.startBeat + n.lengthBeats <= total + 1.0e-9;
        ok = ok && n.note >= window.first && n.note <= window.second && n.note >= kMelodyLowestNote && n.note <= kMelodyHighestNote;
        ok = ok && n.velocity == kDefaultMelodyVelocity;
    }
    for (size_t i = 1; i < m.notes.size(); ++i)   // monophonic line
        ok = ok && m.notes[i].startBeat >= m.notes[i - 1].startBeat + m.notes[i - 1].lengthBeats - 1.0e-9;
    ok = ok && analyseMelody (m, p, Key {}).seamInterval <= 12;
    return ok;
}

// Pooled statistics over many melodies (every share weighted by its own denominator).
struct Pool
{
    double notes = 0, activeBars = 0, ct = 0, down = 0, downCt = 0, intervals = 0, repeats = 0, moving = 0, steps = 0,
           leaps = 0, revDen = 0, rev = 0, odd = 0, finals = 0, finalCt = 0, lag4 = 0, lag4n = 0, outScale = 0;
    int melodies = 0;

    void add (const Melody& m, const Progression& p, const Key& key)
    {
        const auto st = analyseMelody (m, p, key);
        if (st.notes == 0)
            return;
        ++melodies;
        int downs = 0, mov = 0, lps = 0, lpsNext = 0, fin = 0;
        for (size_t i = 0; i < m.notes.size(); ++i)
        {
            const auto& n = m.notes[i];
            const double s = n.startBeat * 4.0;
            downs += std::abs (s - std::round (s)) < 1e-6 && ((int) std::round (s)) % 16 == 0 ? 1 : 0;
            const bool last = i + 1 == m.notes.size();
            fin += last || m.notes[i + 1].startBeat - (n.startBeat + n.lengthBeats) >= 1.0 - 1e-9 ? 1 : 0;
            if (i == 0)
                continue;
            const int d = n.note - m.notes[i - 1].note;
            if (d == 0)
                continue;
            ++mov;
            if (std::abs (d) >= 5)
            {
                ++lps;
                for (size_t j = i + 1; j < m.notes.size(); ++j)
                    if (m.notes[j].note != m.notes[j - 1].note)
                    {
                        ++lpsNext;
                        break;
                    }
            }
        }
        notes += st.notes;
        activeBars += st.notes / st.notesPerActiveBar;
        ct += st.chordToneShare * st.notes;
        down += downs;
        downCt += st.downbeatChordToneShare * downs;
        intervals += st.notes - 1;
        repeats += st.repeatShare * (st.notes - 1);
        moving += mov;
        steps += st.stepShareMoving * mov;
        leaps += st.leapShareMoving * mov;
        revDen += lpsNext;
        rev += st.reverseAfterLeap * lpsNext;
        odd += st.odd16Share * st.notes;
        finals += fin;
        finalCt += st.phraseFinalChordToneShare * fin;
        outScale += st.outOfChordScaleShare * st.notes;
        if (p.totalBeats() >= 32)
        {
            lag4 += st.rhythmRepeatLag4;
            lag4n += 1;
        }
    }

    double npb() const { return activeBars > 0 ? notes / activeBars : 0; }
    double ctShare() const { return notes > 0 ? ct / notes : 0; }
    double downShare() const { return down > 0 ? downCt / down : 0; }
    double repeatShare() const { return intervals > 0 ? repeats / intervals : 0; }
    double stepShare() const { return moving > 0 ? steps / moving : 0; }
    double leapShare() const { return moving > 0 ? leaps / moving : 0; }
    double reverseShare() const { return revDen > 0 ? rev / revDen : 0; }
    double oddShare() const { return notes > 0 ? odd / notes : 0; }
    double finalShare() const { return finals > 0 ? finalCt / finals : 0; }
    double lag4Share() const { return lag4n > 0 ? lag4 / lag4n : 0; }
    double outScaleShare() const { return notes > 0 ? outScale / notes : 0; }
};

void printPool (const char* label, const Pool& p)
{
    std::printf ("    %-10s npb %.2f  downCT %.3f  CT %.3f  rep %.3f  step %.3f  leap %.3f  rev %.3f  odd %.3f  finalCT %.3f  lag4 %.3f\n",
                 label, p.npb(), p.downShare(), p.ctShare(), p.repeatShare(), p.stepShare(), p.leapShare(), p.reverseShare(),
                 p.oddShare(), p.finalShare(), p.lag4Share());
}

bool inBand (double v, double lo, double hi) { return v >= lo - 1e-12 && v <= hi + 1e-12; }

int medianPitch (std::vector<int> v)
{
    if (v.empty())
        return 0;
    std::sort (v.begin(), v.end());
    return v[v.size() / 2];
}

MelodyNote mn (int note, double start, double length)
{
    MelodyNote n;
    n.note = note;
    n.startBeat = start;
    n.lengthBeats = length;
    return n;
}
} // namespace

//==============================================================================
TEST_CASE ("melody gen: mood table, names and contour names")
{
    CHECK (melodyStyleForMood (Mood::Uplifting) == MelodyStyle::Uplifting);
    CHECK (melodyStyleForMood (Mood::Emotional) == MelodyStyle::Lyrical);
    CHECK (melodyStyleForMood (Mood::Dark) == MelodyStyle::Driving);
    CHECK (melodyStyleForMood (Mood::Dreamy) == MelodyStyle::Sparse);
    CHECK (melodyStyleForMood (Mood::Epic) == MelodyStyle::Uplifting);
    CHECK (melodyStyleForMood (Mood::LoFi) == MelodyStyle::Lyrical);
    CHECK_EQ (std::string (melodyStyleName (MelodyStyle::Lyrical)), std::string ("Lyrical hook"));
    CHECK_EQ (std::string (melodyStyleName (MelodyStyle::Driving)), std::string ("Driving riff"));
    CHECK_EQ (std::string (melodyStyleName (MelodyStyle::Sparse)), std::string ("Sparse"));
    for (auto s : kAllStyles)
        CHECK (std::string (melodyStyleDescription (s)).size() > 10);
    CHECK_EQ (std::string (melodyContourName (MelodyContour::Auto)), std::string ("Auto"));
    CHECK_EQ (std::string (melodyContourName (MelodyContour::Arch)), std::string ("Arch"));
}

TEST_CASE ("melody gen: empty progression gives an empty melody, no plan and zero stats")
{
    const Progression empty;
    for (auto s : kAllStyles)
        CHECK (generateMelody (empty, kCMajor, {}, 7, settingsFor (s)).empty());
    CHECK (melodyPlanSignature (empty, kCMajor, 7, {}).empty());

    const auto st = analyseMelody (Melody {}, popAxis(), kCMajor);
    CHECK_EQ (st.notes, 0);
    CHECK_EQ (st.notesPerActiveBar, 0.0);
}

TEST_CASE ("melody gen: deterministic, and seed + 1 gives a different melody")
{
    const auto p = popAxis();
    const auto ctx = contextFor (p);
    for (auto s : kAllStyles)
    {
        const auto set = settingsFor (s);
        int differ = 0;
        for (std::uint32_t seed = 1; seed <= 200; ++seed)
        {
            const auto a = generateMelody (p, kCMajor, ctx, seed, set);
            CHECK (a == generateMelody (p, kCMajor, ctx, seed, set));
            differ += a != generateMelody (p, kCMajor, ctx, seed + 1, set) ? 1 : 0;
        }
        CHECK (differ >= 190);
    }
}

TEST_CASE ("melody gen: invariants for every style, contour, density and range on edge progressions")
{
    const Progression progressions[] = { popAxis(), twoBeat(), mixedLengths(), oddTotal(), sixtyFourBars(), oneSlot(),
                                         slashAndShort() };
    int failures = 0, runs = 0;
    for (const auto& p : progressions)
    {
        const MelodyGenContext contexts[] = { MelodyGenContext {}, contextFor (p) };
        for (auto s : kAllStyles)
            for (auto c : kAllContours)
                for (float d : { 0.0f, 0.5f, 1.0f })
                    for (int shift : { -12, 0, 12 })
                    {
                        const auto set = settingsFor (s, d, shift, c);
                        const auto& ctx = contexts[(runs++) % 2];
                        const std::uint32_t seed = (std::uint32_t) (runs * 7919);
                        const auto m = generateMelody (p, kCMajor, ctx, seed, set);
                        if (! checkInvariants (m, p, ctx, set))
                        {
                            if (failures++ < 5)
                                std::printf ("    invariant failure: style %d contour %d density %.1f shift %d beats %d\n",
                                             (int) s, (int) c, d, shift, p.totalBeats());
                        }
                    }
    }
    CHECK_EQ (failures, 0);
}

TEST_CASE ("melody gen: note cap holds for Driving at density 1 over 64 bars")
{
    const auto p = sixtyFourBars();
    const auto ctx = contextFor (p);
    for (std::uint32_t seed = 1; seed <= 10; ++seed)
    {
        const auto set = settingsFor (MelodyStyle::Driving, 1.0f);
        const auto m = generateMelody (p, kAMinor, ctx, seed, set);
        CHECK ((int) m.notes.size() <= kMaxMelodyNotes);
        CHECK ((int) m.notes.size() > 300);
        CHECK (m.notes.back().startBeat >= 240.0);   // the cap thins the rhythm, it never cuts the loop's end off
        CHECK (checkInvariants (m, p, ctx, set));
    }
}

TEST_CASE ("melody gen: stream independence (plan, onsets, repetition)")
{
    const Progression progressions[] = { popAxis(), repeated (epicMinor(), 2), twoBeat(), repeated (popAxis(), 4) };
    for (const auto& p : progressions)
    {
        const auto full = contextFor (p);
        MelodyGenContext withArp = full;
        for (int b = 0; b < p.totalBeats(); ++b)
            withArp.accompaniment.push_back (NoteEvent { 64 + (b % 3) * 3, 0.7f, (double) b, 0.25, 4 });

        for (auto s : kAllStyles)
            for (std::uint32_t seed = 1; seed <= 40; ++seed)
            {
                const auto set = settingsFor (s);
                const auto sig = melodyPlanSignature (p, kCMajor, seed, set);
                CHECK (! sig.empty());

                auto other = set;
                other.density = 0.9f;
                CHECK_EQ (melodyPlanSignature (p, kCMajor, seed, other), sig);
                other = set;
                other.contour = MelodyContour::Falling;
                CHECK_EQ (melodyPlanSignature (p, kCMajor, seed, other), sig);
                other = set;
                other.registerShift = 7;
                CHECK_EQ (melodyPlanSignature (p, kCMajor, seed, other), sig);
                other = set;
                other.resolveEnding = false;
                CHECK_EQ (melodyPlanSignature (p, kCMajor, seed, other), sig);

                // Onset times survive contour, range, ending and context changes.
                const auto base = onsets (generateMelody (p, kCMajor, full, seed, set));
                for (auto c : kAllContours)
                {
                    auto cs = set;
                    cs.contour = c;
                    CHECK (onsets (generateMelody (p, kCMajor, full, seed, cs)) == base);
                }
                for (int shift : { -12, -5, 5, 12 })
                {
                    auto rs = set;
                    rs.registerShift = shift;
                    CHECK (onsets (generateMelody (p, kCMajor, full, seed, rs)) == base);
                }
                auto es = set;
                es.resolveEnding = false;
                CHECK (onsets (generateMelody (p, kCMajor, full, seed, es)) == base);
                CHECK (onsets (generateMelody (p, kCMajor, withArp, seed, set)) == base);
                CHECK (onsets (generateMelody (p, kCMajor, MelodyGenContext {}, seed, set)) == base);

                // Repetition never gives a less repetitive form at a higher value.
                int previous = 1000;
                for (float r : { 0.15f, 0.5f, 0.85f })
                {
                    auto rs = set;
                    rs.repetition = r;
                    const int distinct = countDistinctLetters (melodyPlanSignature (p, kCMajor, seed, rs));
                    CHECK (distinct <= previous);
                    previous = distinct;
                }
            }
    }
}

TEST_CASE ("melody gen: plan signature format")
{
    const auto sig = melodyPlanSignature (repeated (popAxis(), 2), kCMajor, 3, settingsFor (MelodyStyle::Uplifting));
    CHECK (sig.rfind ("U1 ", 0) == 0 || sig.rfind ("U2 ", 0) == 0);
    CHECK (sig.find (" gap:") != std::string::npos);
    CHECK (sig.find (" cad:") != std::string::npos);
    CHECK (sig.find (" pedal:0") != std::string::npos);
    CHECK (sig.back() == '0');   // only Driving runs the pedal mode

    int pedal = 0;
    for (std::uint32_t seed = 1; seed <= 100; ++seed)
        pedal += melodyPlanSignature (popAxis(), kAMinor, seed, settingsFor (MelodyStyle::Driving)).back() == '1' ? 1 : 0;
    CHECK (pedal > 20);   // p 0.4 per melody
    CHECK (pedal < 60);
}

TEST_CASE ("melody gen: hard statistic bands (seeds 1..200 x 4 styles x 3 progressions)")
{
    struct Band { double npbLo, npbHi, downLo, downHi, stepLo, stepHi, leapLo, leapHi, oddLo, oddHi; };
    const Band bands[] = {
        { 5.0, 6.6, .70, .85, .33, .53, .27, .47, 0.0, .30 },
        { 2.8, 4.0, .70, .85, .42, .62, .15, .33, 0.0, .12 },
        { 9.0, 12.5, .74, .90, .10, .28, .60, .82, .30, .50 },
        { 1.1, 2.0, .78, .92, .45, .65, .08, .24, 0.0, .08 },
    };
    struct Case { Progression p; Key key; };
    const Case cases[] = { { epicMinor(), kAMinor }, { popAxis(), kCMajor }, { twoBeat(), kAMinor } };

    std::printf ("  hard bands (pooled):\n");
    for (int si = 0; si < 4; ++si)
    {
        const auto style = kAllStyles[si];
        Pool pool;
        for (const auto& c : cases)
        {
            const auto ctx = contextFor (c.p);
            for (std::uint32_t seed = 1; seed <= 200; ++seed)
                pool.add (generateMelody (c.p, c.key, ctx, seed, settingsFor (style)), c.p, c.key);
        }
        printPool (melodyStyleShortName (style), pool);
        const auto& b = bands[si];
        CHECK (inBand (pool.npb(), b.npbLo, b.npbHi));
        CHECK (inBand (pool.downShare(), b.downLo, b.downHi));
        CHECK (inBand (pool.stepShare(), b.stepLo, b.stepHi));
        CHECK (inBand (pool.leapShare(), b.leapLo, b.leapHi));
        CHECK (inBand (pool.oddShare(), b.oddLo, b.oddHi));
    }
}

TEST_CASE ("melody gen: soft statistics (printed for the listening pass)")
{
    struct Case { Progression p; Key key; };
    const Case cases[] = { { repeated (epicMinor(), 2), kAMinor }, { repeated (popAxis(), 2), kCMajor },
                           { repeated (twoBeat(), 2), kAMinor } };
    std::printf ("  soft metrics, 8-bar loops (bands: CT .60-.78 / .58-.76 / .70-.86 / .62-.82, rep .30-.45 / .20-.35 / "
                 ".22-.38 / .14-.30, rev >= .55 / .58 / .62 / .58, lag4 >= .60 / .50 / .75 / .50, finalCT >= .60 / .60 / - / .65):\n");
    for (auto style : kAllStyles)
    {
        Pool pool;
        for (const auto& c : cases)
        {
            const auto ctx = contextFor (c.p);
            for (std::uint32_t seed = 1; seed <= 100; ++seed)
                pool.add (generateMelody (c.p, c.key, ctx, seed, settingsFor (style)), c.p, c.key);
        }
        printPool (melodyStyleShortName (style), pool);
        CHECK (pool.melodies == 300);
    }
}

TEST_CASE ("melody gen: R1 chord-scale adaptation over secondary and borrowed chords")
{
    const auto p = secondaryBorrowed();
    // The key tones each chord replaces: D7 F -> F#, E7 G -> G#, Fm A -> Ab, Bb B -> Bb.
    const int replaced[] = { -1, 5, -1, 7, -1, 9, -1, 11 };
    const std::vector<std::vector<int>> chordPcs = { { 0, 4, 7 }, { 2, 6, 9, 0 }, { 7, 11, 2 }, { 4, 8, 11, 2 },
                                                     { 9, 0, 4 }, { 5, 8, 0 }, { 0, 4, 7 }, { 10, 2, 5 } };
    const auto ctx = contextFor (p);
    Pool pool;
    int strongOut = 0, outsideScale = 0;
    for (auto style : kAllStyles)
        for (std::uint32_t seed = 1; seed <= 100; ++seed)
        {
            const auto m = generateMelody (p, kCMajor, ctx, seed, settingsFor (style));
            pool.add (m, p, kCMajor);
            for (const auto& n : m.notes)
            {
                const int slot = p.slotAtBeat (n.startBeat);
                const int npc = pc (n.note);
                const auto& tones = chordPcs[(size_t) slot];
                const bool ct = std::find (tones.begin(), tones.end(), npc) != tones.end();
                const int step = (int) std::round (n.startBeat * 4.0) % 16;
                if ((step == 0 || step == 8) && npc == replaced[slot] && ! ct)
                    ++strongOut;
                // Every note is a chord tone or a tone of the local chord-scale.
                const bool keyTone = kCMajor.contains (npc) && npc != replaced[slot];
                if (! ct && ! keyTone)
                    ++outsideScale;
            }
        }
    CHECK_EQ (strongOut, 0);
    CHECK_EQ (outsideScale, 0);
    CHECK (pool.outScaleShare() <= 0.02);
    std::printf ("    R1 out-of-chord-scale share %.4f\n", pool.outScaleShare());
}

TEST_CASE ("melody gen: resolveEnding, loop seam (R7) and cadences")
{
    struct Case { Progression p; Key key; };
    const Case cases[] = { { popAxis(), kCMajor }, { epicMinor(), kAMinor }, { repeated (popAxis(), 2), kCMajor },
                           { secondaryBorrowed(), kCMajor } };
    for (auto style : kAllStyles)
    {
        int tonicFamily = 0, runs = 0, tonicFamilyOff = 0;
        for (const auto& c : cases)
        {
            const auto ctx = contextFor (c.p);
            const auto k = c.key.pitchClasses();
            for (std::uint32_t seed = 1; seed <= 100; ++seed)
            {
                const auto m = generateMelody (c.p, c.key, ctx, seed, settingsFor (style));
                const auto off = generateMelody (c.p, c.key, ctx, seed, settingsFor (style, 0.5f, 0, MelodyContour::Auto, 0.5f, false));
                ++runs;
                const int last = pc (m.notes.back().note), lastOff = pc (off.notes.back().note);
                tonicFamily += (last == k[0] || last == k[2] || last == k[4]) ? 1 : 0;
                tonicFamilyOff += (lastOff == k[0] || lastOff == k[2] || lastOff == k[4]) ? 1 : 0;
                CHECK (analyseMelody (m, c.p, c.key).seamInterval <= 12);
                CHECK (analyseMelody (off, c.p, c.key).seamInterval <= 12);
            }
        }
        std::printf ("    %-10s last note tonic family: %.2f (resolveEnding off %.2f)\n", melodyStyleShortName (style),
                     tonicFamily / (double) runs, tonicFamilyOff / (double) runs);
        CHECK (tonicFamily >= 0.6 * runs);
    }
}

TEST_CASE ("melody gen: R6 a note held into a clashing chord is cut (suspensions only in Lyrical / Sparse)")
{
    const Progression progressions[] = { secondaryBorrowed(), twoBeat(), slashAndShort(), mixedLengths(),
                                         prog ({ { 0, Q::Major, 4 }, { 4, Q::Minor, 4 }, { 5, Q::Major7, 4 }, { 11, Q::Diminished, 4 } }) };
    int crossings = 0, suspensions = 0, bad = 0;
    for (const auto& p : progressions)
    {
        const auto ctx = contextFor (p);
        for (auto style : kAllStyles)
            for (std::uint32_t seed = 1; seed <= 60; ++seed)
            {
                const auto m = generateMelody (p, kCMajor, ctx, seed, settingsFor (style, 0.3f));
                for (size_t i = 0; i < m.notes.size(); ++i)
                {
                    const auto& n = m.notes[i];
                    for (int sl = p.slotAtBeat (n.startBeat) + 1; sl < (int) p.slots.size(); ++sl)
                    {
                        const double boundary = p.slotStartBeat (sl);
                        if (boundary >= n.startBeat + n.lengthBeats - 1e-9)
                            break;
                        ++crossings;
                        const auto& chord = p.slots[(size_t) sl].chord;
                        const auto tones = chord.pitchClasses();
                        const int npc = pc (n.note);
                        auto isTone = [&] (int x) { return std::find (tones.begin(), tones.end(), pc (x)) != tones.end(); };
                        const int bass = slotBassPitchClass (p.slots[(size_t) sl], ctx.voicings[(size_t) sl]);
                        const bool clash = ! isTone (npc) && (isTone (npc + 1) || isTone (npc - 1) || npc == pc (bass + 1));
                        if (! clash)
                            continue;
                        const bool susStyle = style == MelodyStyle::Lyrical || style == MelodyStyle::Sparse;
                        const bool resolves = i + 1 < m.notes.size() && m.notes[i + 1].startBeat <= boundary + 1.0 + 1e-9
                                           && n.note - m.notes[i + 1].note >= 1 && n.note - m.notes[i + 1].note <= 2
                                           && [&] { const auto nc = chordAtBeat (p, m.notes[i + 1].startBeat)->pitchClasses();
                                                    return std::find (nc.begin(), nc.end(), pc (m.notes[i + 1].note)) != nc.end(); }();
                        if (! (susStyle && resolves))
                            std::printf ("      R6 clash: style %d note %d at %.3f len %.3f boundary %.1f slot %d\n", (int) style,
                                         n.note, n.startBeat, n.lengthBeats, boundary, sl);
                        if (susStyle && resolves)
                            ++suspensions;
                        else
                            ++bad;
                        break;
                    }
                }
            }
    }
    std::printf ("    R6: %d boundary crossings, %d suspensions\n", crossings, suspensions);
    CHECK (crossings > 50);   // notes do ring over chord changes (the corpus keeps 30% non-chord tones there)
    CHECK_EQ (bad, 0);
}

TEST_CASE ("melody gen: R8 window and line sit above the arp")
{
    const auto p = repeated (popAxis(), 2);
    MelodyGenContext plain = contextFor (p);
    MelodyGenContext arp = plain;
    for (int i = 0; i < p.totalBeats() * 4; ++i)
        arp.accompaniment.push_back (NoteEvent { 60 + (i % 3) * 5 - (i % 6 == 5 ? 5 : 0), 0.7f, i * 0.25, 0.2, 4 });

    std::vector<int> arpNotes;
    for (const auto& e : arp.accompaniment)
        if (e.channel == 4)
            arpNotes.push_back (e.note);
    CHECK_EQ (medianPitch (arpNotes), 65);

    for (auto style : kAllStyles)
    {
        const auto set = settingsFor (style);
        const auto w0 = melodyRegisterWindow (p, plain, set);
        const auto w1 = melodyRegisterWindow (p, arp, set);
        CHECK (w1.first - w0.first >= 0);
        CHECK (w1.first - w0.first <= 3);

        std::vector<int> a, b;
        for (std::uint32_t seed = 1; seed <= 100; ++seed)
        {
            for (const auto& n : generateMelody (p, kCMajor, plain, seed, set).notes)
                a.push_back (n.note);
            for (const auto& n : generateMelody (p, kCMajor, arp, seed, set).notes)
                b.push_back (n.note);
        }
        double ma = 0, mb = 0;
        for (int x : a)
            ma += x;
        for (int x : b)
            mb += x;
        ma /= (double) a.size();
        mb /= (double) b.size();
        std::printf ("    %-10s window low %d -> %d, mean pitch %.2f -> %.2f\n", melodyStyleShortName (style), w0.first,
                     w1.first, ma, mb);
        CHECK (mb > ma);
    }
}

TEST_CASE ("melody gen: density is monotonic and Range moves the register")
{
    const auto p = repeated (epicMinor(), 2);
    const auto ctx = contextFor (p);
    for (auto style : kAllStyles)
    {
        double previous = 0.0;
        for (float d : { 0.15f, 0.5f, 0.85f })
        {
            Pool pool;
            for (std::uint32_t seed = 1; seed <= 100; ++seed)
                pool.add (generateMelody (p, kAMinor, ctx, seed, settingsFor (style, d)), p, kAMinor);
            CHECK (pool.npb() > previous);
            previous = pool.npb();
        }

        std::vector<int> mid, high;
        for (std::uint32_t seed = 1; seed <= 100; ++seed)
        {
            for (const auto& n : generateMelody (p, kAMinor, ctx, seed, settingsFor (style)).notes)
                mid.push_back (n.note);
            for (const auto& n : generateMelody (p, kAMinor, ctx, seed, settingsFor (style, 0.5f, 5)).notes)
                high.push_back (n.note);
        }
        const int rise = medianPitch (high) - medianPitch (mid);
        std::printf ("    %-10s Range High raises the median by %d\n", melodyStyleShortName (style), rise);
        CHECK (rise >= 3);
        CHECK (rise <= 7);
    }
}

TEST_CASE ("melody gen: register window")
{
    const auto p = popAxis();
    const MelodyGenContext none;
    CHECK (melodyRegisterWindow (p, none, settingsFor (MelodyStyle::Uplifting)) == std::make_pair (63, 75));
    CHECK (melodyRegisterWindow (p, none, settingsFor (MelodyStyle::Lyrical)) == std::make_pair (63, 76));
    CHECK (melodyRegisterWindow (p, none, settingsFor (MelodyStyle::Driving)) == std::make_pair (54, 75));
    CHECK (melodyRegisterWindow (p, none, settingsFor (MelodyStyle::Sparse)) == std::make_pair (64, 77));
    CHECK (melodyRegisterWindow (p, none, settingsFor (MelodyStyle::Uplifting, 0.5f, 12)) == std::make_pair (75, 87));
    CHECK (melodyRegisterWindow (p, none, settingsFor (MelodyStyle::Uplifting, 0.5f, 40)) == std::make_pair (75, 87));   // clamped shift

    MelodyGenContext v;
    v.voicings = { { 60, 64, 67 }, { 59, 62, 67 }, { 57, 60, 64 }, { 57, 60, 65 } };   // tops 67 67 64 65 -> median 65
    CHECK (melodyRegisterWindow (p, v, settingsFor (MelodyStyle::Uplifting)) == std::make_pair (60, 72));

    MelodyGenContext high;
    high.voicings = { { 100, 104, 107 } };
    const auto w = melodyRegisterWindow (prog ({ { 0, Q::Major, 4 } }), high, settingsFor (MelodyStyle::Sparse, 0.5f, 12));
    CHECK (w.second == kMelodyHighestNote);
    CHECK (w.second - w.first >= 12);
}

TEST_CASE ("melody gen: every style over short slots, slash chords and modes stays musical")
{
    struct Case { Progression p; Key key; };
    const Case cases[] = {
        { slashAndShort(), kCMajor },
        { mixedLengths(), kCMajor },
        { prog ({ { 2, Q::Minor7, 1 }, { 7, Q::Dominant9, 1 }, { 0, Q::Major7, 2 }, { 9, Q::Dominant7Flat9, 3 }, { 2, Q::Minor9, 1 } }), kCMajor },
        { prog ({ { 9, Q::Minor, 3 }, { 7, Q::Major, 3 }, { 5, Q::Major, 2 }, { 4, Q::Dominant7, 4, 1 } }), Key { 9, ScaleType::HarmonicMinor } },
        { prog ({ { 2, Q::Minor, 4 }, { 7, Q::Major, 4 } }), Key { 2, ScaleType::Dorian } },
        { prog ({ { 7, Q::Major, 4 }, { 5, Q::Major, 4 }, { 0, Q::Major, 4 }, { 7, Q::Major, 4 } }), Key { 7, ScaleType::Mixolydian } },
    };
    for (const auto& c : cases)
    {
        const auto ctx = contextFor (c.p);
        for (auto style : kAllStyles)
        {
            Pool pool;
            int failures = 0;
            for (std::uint32_t seed = 1; seed <= 40; ++seed)
            {
                const auto set = settingsFor (style);
                const auto m = generateMelody (c.p, c.key, ctx, seed, set);
                failures += checkInvariants (m, c.p, ctx, set) ? 0 : 1;
                pool.add (m, c.p, c.key);
            }
            CHECK_EQ (failures, 0);
            CHECK (pool.downShare() >= 0.6);
            CHECK (pool.ctShare() >= 0.5);
        }
    }
}

TEST_CASE ("melody gen: analyseMelody on hand-built melodies")
{
    // C major, C - G - Am - F (4 beats each); accompaniment: one held B (a semitone under C) over the first bar.
    const auto p = popAxis();
    Melody m;
    m.notes = {
        mn (72, 0.0, 1.0),     // C over C: chord tone, downbeat, strong
        mn (72, 1.0, 0.5),     // repeat
        mn (74, 1.5, 0.25),    // D: step, off8 (odd? no: step 6)
        mn (79, 2.25, 0.5),    // G: leap +5, e16 (odd step 9)
        mn (77, 4.0, 0.5),     // F over G: not a chord tone, downbeat; reverse after the leap
        mn (76, 6.0, 0.5),     // E over G: rest of 1.5 beats before -> F was phrase-final
        mn (69, 8.0, 4.0),     // A over Am, downbeat, chord tone; leap -7
        mn (65, 13.0, 1.0),    // F over F; leap -4; last note (phrase-final)
        mn (60, 20.0, 1.0),    // parked: at or after the loop end (16 beats) -> ignored
    };
    std::vector<NoteEvent> acc { NoteEvent { 71, 0.7f, 0.0, 4.0, 1 }, NoteEvent { 66, 0.7f, 4.0, 4.0, 4 } };
    const auto st = analyseMelody (m, p, kCMajor, acc);

    CHECK_EQ (st.notes, 8);
    CHECK_EQ (st.lowest, 65);
    CHECK_EQ (st.highest, 79);
    CHECK (std::abs (st.notesPerActiveBar - 2.0) < 1e-9);                // 8 notes in 4 active bars
    // chord tones: C, C, -, G(over C), -, -, A, F -> 5 of 8 (D over C and F, E over G are not)
    CHECK (std::abs (st.chordToneShare - 5.0 / 8.0) < 1e-9);
    CHECK (std::abs (st.downbeatChordToneShare - 2.0 / 3.0) < 1e-9);     // C, F(no), A
    // phrase-final (>= 1 beat of rest after the note): G@2.25 (ends 2.75, next 4), F@4, E@6, A@8 (ends 12, next 13),
    // F (last) -> chord tones G, A, F
    CHECK (std::abs (st.phraseFinalChordToneShare - 3.0 / 5.0) < 1e-9);
    CHECK (std::abs (st.repeatShare - 1.0 / 7.0) < 1e-9);
    // moving intervals: +2, +5, -2, -1, -7, -4 -> steps 3 of 6, leaps (>= 5) 2 of 6
    CHECK (std::abs (st.stepShareMoving - 3.0 / 6.0) < 1e-9);
    CHECK (std::abs (st.leapShareMoving - 2.0 / 6.0) < 1e-9);
    // +5 is followed by -2 (reverse), -7 by -4 (same direction)
    CHECK (std::abs (st.reverseAfterLeap - 0.5) < 1e-9);
    CHECK (std::abs (st.odd16Share - 1.0 / 8.0) < 1e-9);                 // only the onset at 2.25 (step 9)
    CHECK_EQ (st.rhythmRepeatLag4, 0.0);                                   // a 4-bar loop
    // strong-beat notes: C@0, F@4 (not a chord tone; F# of the arp is a semitone away -> clash), E@6 (beat 3, a whole
    // tone from F#), A@8 -> 1 of 4
    CHECK (std::abs (st.strongBeatClashShare - 1.0 / 4.0) < 1e-9);
    CHECK_EQ (st.outOfChordScaleShare, 0.0);
    CHECK_EQ (st.seamInterval, 7);                                         // F 65 -> C 72

    // R1 share and lag-4 repeats on an 8-bar loop over C - E7 (G -> G#).
    const auto p2 = repeated (prog ({ { 0, Q::Major, 4 }, { 4, Q::Dominant7, 4 } }), 4);
    Melody r;
    for (int bar = 0; bar < 8; ++bar)
    {
        r.notes.push_back (mn (bar % 2 == 1 ? 67 : 72, bar * 4.0, 1.0));   // G over E7 is out of the chord-scale
        if (bar != 6)
            r.notes.push_back (mn (76, bar * 4.0 + 2.0, 1.0));
    }
    const auto st2 = analyseMelody (sanitiseMelody (r), p2, kCMajor);
    CHECK (std::abs (st2.outOfChordScaleShare - 4.0 / 15.0) < 1e-9);
    // bars 4..7 vs 0..3: bar 6 lost its second onset -> 3 of 4 equal
    CHECK (std::abs (st2.rhythmRepeatLag4 - 0.75) < 1e-9);
}

TEST_CASE ("melody gen: parked notes and empty input in analyseMelody")
{
    const auto p = popAxis();
    Melody parked;
    parked.notes = { mn (72, 16.0, 1.0), mn (74, 30.0, 1.0) };
    const auto st = analyseMelody (parked, p, kCMajor);
    CHECK_EQ (st.notes, 0);
    CHECK_EQ (st.seamInterval, 0);

    // Generated melodies never contain notes past the loop end.
    for (auto style : kAllStyles)
        for (const auto& n : generateMelody (p, kCMajor, contextFor (p), 11, settingsFor (style)).notes)
            CHECK (n.startBeat + n.lengthBeats <= 16.0 + 1e-9);
}

TEST_CASE ("melody gen: phrasing - repeated letters really repeat and phrase ends breathe")
{
    // With repetition at loop-like, most 8-bar Uplifting loops repeat whole bars of rhythm 4 bars later.
    const auto p = repeated (popAxis(), 2);
    const auto ctx = contextFor (p);
    double lagLoop = 0.0, lagVaried = 0.0;
    for (std::uint32_t seed = 1; seed <= 100; ++seed)
    {
        lagLoop += analyseMelody (generateMelody (p, kCMajor, ctx, seed, settingsFor (MelodyStyle::Uplifting, 0.5f, 0, MelodyContour::Auto, 0.85f)), p, kCMajor).rhythmRepeatLag4;
        lagVaried += analyseMelody (generateMelody (p, kCMajor, ctx, seed, settingsFor (MelodyStyle::Uplifting, 0.5f, 0, MelodyContour::Auto, 0.15f)), p, kCMajor).rhythmRepeatLag4;
    }
    std::printf ("    lag-4 rhythm repeats: loop-like %.2f, varied %.2f\n", lagLoop / 100.0, lagVaried / 100.0);
    CHECK (lagLoop > lagVaried);
}

TEST_CASE ("melody gen: budget (64 bars, mean of 20 runs per style)")
{
    const auto p = sixtyFourBars();
    const auto ctx = contextFor (p);
    for (auto style : kAllStyles)
    {
        const auto set = settingsFor (style, 0.85f);
        const auto t0 = std::chrono::steady_clock::now();
        size_t notes = 0;
        for (std::uint32_t seed = 1; seed <= 20; ++seed)
            notes += generateMelody (p, kAMinor, ctx, seed, set).notes.size();
        const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count() / 20.0;
        std::printf ("    %-10s %.3f ms per 64-bar melody (%zu notes avg)\n", melodyStyleShortName (style), ms, notes / 20);
#ifdef NDEBUG
        CHECK (ms < 5.0);
#endif
    }
}
