#include "TestFramework.h"

#include "core/MelodyGen.h"
#include "core/Voicing.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// R6's true clash with a chord: a b9 above its bass pitch class (unless the chord itself has that tone as an extension,
// such as a root-position 7b9), or a non-chord tone a semitone from one of its tones.
bool isTrueClash (const Chord& chord, int bassPc, int npc)
{
    int interval = -1;
    for (int iv : chord.intervals())
        if (pc (chord.root + iv) == pc (npc))
            interval = iv;
    if (pc (npc) == pc (bassPc + 1) && interval < 12)
        return true;
    if (interval >= 0)
        return false;
    for (int t : chord.pitchClasses())
        if (pc (t) == pc (npc + 1) || pc (t) == pc (npc - 1))
            return true;
    return false;
}

Progression heldIntoInversions()
{
    // C -> Cmaj7/B (3rd inversion: a held C is a b9 above the bass) -> F -> Fmaj7/E (a held F is a b9 above E)
    return prog ({ { 0, Q::Major, 4 }, { 0, Q::Major7, 4, 3 }, { 5, Q::Major, 4 }, { 5, Q::Major7, 4, 3 } });
}

bool isChordTone (const Progression& p, const MelodyNote& n)
{
    const auto chord = chordAtBeat (p, n.startBeat);
    if (! chord)
        return false;
    for (int t : chord->pitchClasses())
        if (pc (t) == pc (n.note))
            return true;
    return false;
}

// Onsets and pitches of one 4-beat bar: (16th step in the bar, pitch).
using BarNotes = std::vector<std::pair<int, int>>;
std::vector<BarNotes> barsOf (const Melody& m, int numBars)
{
    std::vector<BarNotes> bars ((size_t) numBars);
    for (const auto& n : m.notes)
    {
        const int step = (int) std::lround (n.startBeat * 4.0);
        if (step / 16 < numBars)
            bars[(size_t) (step / 16)].push_back ({ step % 16, n.note });
    }
    return bars;
}

bool sameRhythm (const BarNotes& a, const BarNotes& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].first != b[i].first)
            return false;
    return true;
}

MelodyNote mn (int note, double start, double length)
{
    MelodyNote n;
    n.note = note;
    n.startBeat = start;
    n.lengthBeats = length;
    return n;
}

// The cadence notes of a generated melody, read from its plan signature ("U1 ABAB gap:-,2p1,-,- cad:o,c,o,c ..."):
// the last own (non-pickup) note of each unit with a cadence tag; the last own note of the loop is the loop end.
// A generated melody has one note per onset, so the notes of a unit are the notes starting inside its span.
struct CadenceNote
{
    size_t index;
    bool closed, loopEnd;
};

std::vector<CadenceNote> cadenceNotes (const std::string& sig, const Melody& m, double totalBeats)
{
    std::vector<CadenceNote> out;
    if (sig.size() < 2 || sig[0] != 'U' || m.notes.empty())
        return out;
    const double unitBeats = 4.0 * (sig[1] - '0');
    auto field = [&] (const char* name) {
        const auto a = sig.find (name);
        const auto from = a + std::strlen (name);
        return a == std::string::npos ? std::string() : sig.substr (from, sig.find (' ', from) - from);
    };
    auto split = [] (const std::string& s) {
        std::vector<std::string> v (1);
        for (char c : s)
        {
            if (c == ',')
                v.emplace_back();
            else
                v.back() += c;
        }
        return v;
    };
    const auto gaps = split (field (" gap:")), cads = split (field (" cad:"));
    if (gaps.size() != cads.size())
        return out;

    long lastOwn = -1;
    for (size_t k = 0; k < cads.size(); ++k)
    {
        const double from = k * unitBeats, to = std::min ((k + 1) * unitBeats, totalBeats);
        const auto p = gaps[k].find ('p');
        const int pickups = p == std::string::npos ? 0 : std::atoi (gaps[k].c_str() + p + 1);
        std::vector<size_t> inUnit;
        for (size_t i = 0; i < m.notes.size(); ++i)
            if (m.notes[i].startBeat >= from - 1e-9 && m.notes[i].startBeat < to - 1e-9)
                inUnit.push_back (i);
        if ((int) inUnit.size() <= pickups)
            continue;
        const size_t last = inUnit[inUnit.size() - 1 - (size_t) pickups];
        lastOwn = (long) last;
        if (cads[k] != "-")
            out.push_back ({ last, cads[k] == "c", false });
    }
    for (auto& c : out)
        c.loopEnd = (long) c.index == lastOwn;
    if (lastOwn >= 0 && (out.empty() || ! out.back().loopEnd))
        out.push_back ({ (size_t) lastOwn, true, true });
    return out;
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
    // final-spec 9 WP1: seed + 1 differs in >= 95% of 200 pairs, also on 1-2 bar loops (where Sparse used to collapse to
    // a few dozen one-note or repeated-tonic melodies) and at Sparse / Busy density.
    struct Case { const char* name; Progression p; Key key; bool shortLoop; };
    const Case cases[] = { { "popAxis", popAxis(), kCMajor, false },
                           { "twoBeat", twoBeat(), kCMajor, false },
                           { "oneSlot", oneSlot(), kCMajor, true },
                           { "oneSlot A minor", oneSlot(), kAMinor, true },
                           { "Am-F 2 bars", prog ({ { 9, Q::Minor, 4 }, { 5, Q::Major, 4 } }), kAMinor, true },
                           { "C-G 1 bar", prog ({ { 0, Q::Major, 2 }, { 7, Q::Major, 2 } }), kCMajor, true } };
    for (const auto& c : cases)
    {
        const auto ctx = contextFor (c.p);
        for (auto s : kAllStyles)
            for (float d : { 0.15f, 0.5f, 0.85f })
            {
                if (! c.shortLoop && d != 0.5f)
                    continue;
                const auto set = settingsFor (s, d);
                int differ = 0, single = 0;
                for (std::uint32_t seed = 1; seed <= 200; ++seed)
                {
                    const auto a = generateMelody (c.p, c.key, ctx, seed, set);
                    CHECK (a == generateMelody (c.p, c.key, ctx, seed, set));
                    differ += a != generateMelody (c.p, c.key, ctx, seed + 1, set) ? 1 : 0;
                    single += a.notes.size() == 1 ? 1 : 0;
                }
                const bool minOnsetStyle = s == MelodyStyle::Sparse || s == MelodyStyle::Lyrical;
                if (differ < 190 || (c.shortLoop && minOnsetStyle && single > 0))
                    std::printf ("    %-16s %-10s density %.2f: seed + 1 differs %d / 200, %d one-note melodies\n", c.name,
                                 melodyStyleShortName (s), d, differ, single);
                CHECK (differ >= 190);
                if (c.shortLoop && minOnsetStyle)
                    CHECK_EQ (single, 0);   // a short Sparse / Lyrical loop keeps a minimum of onsets
            }
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
    CHECK (pedal >= 30);   // p 0.5 per melody (final-spec 5.4.3 / 5.4.4)
    CHECK (pedal < 70);
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
    // The key tones each chord replaces: D7 F -> F#, E7 G -> G#, Fm A -> Ab, Bb B -> Bb. The borrowed Fm and Bb take
    // their chord-scale from the parallel minor (C D Eb F G Ab Bb), so E, A and B never sound over them.
    const int replaced[] = { -1, 5, -1, 7, -1, 9, -1, 11 };
    const Key cMinor { 0, ScaleType::NaturalMinor };
    int borrowedKeyTones = 0;
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
                const bool borrowedSlot = slot == 5 || slot == 7;
                const bool keyTone = borrowedSlot ? cMinor.contains (npc) : kCMajor.contains (npc) && npc != replaced[slot];
                if (! ct && ! keyTone)
                    ++outsideScale;
                if (borrowedSlot && ! ct && (npc == 4 || npc == 9 || npc == 11))
                    ++borrowedKeyTones;
            }
        }
    CHECK_EQ (strongOut, 0);
    CHECK_EQ (outsideScale, 0);
    CHECK_EQ (borrowedKeyTones, 0);
    CHECK (pool.outScaleShare() <= 0.02);
    std::printf ("    R1 out-of-chord-scale share %.4f\n", pool.outScaleShare());
}

TEST_CASE ("melody gen: resolveEnding, loop seam (R7) and cadences")
{
    struct Case { Progression p; Key key; };
    const Case cases[] = { { popAxis(), kCMajor }, { epicMinor(), kAMinor }, { repeated (popAxis(), 2), kCMajor },
                           { secondaryBorrowed(), kCMajor } };
    // final-spec 9 WP1: the loop end is home (MelodyStats::loopEndHome: 1, 3 or 5 of the key, or - over a last chord
    // with none of them, such as epicMinor's G or secondaryBorrowed's Bb - a tone of that chord a step from the first
    // note, resolving through the seam) in >= 65% of seeds, and resolveEnding adds >= 15 points. The old metric (tonic
    // family only) forced non-chord tonic endings over those chords; the share of such endings is printed. Over the two
    // popAxis cases (the last chord F holds the tonic) most ends are still tonic family.
    for (auto style : kAllStyles)
    {
        int home = 0, homeOff = 0, runs = 0, tonicFamily = 0, nctFamily = 0, popRuns = 0, popFamily = 0;
        for (const auto& c : cases)
        {
            const auto ctx = contextFor (c.p);
            const auto k = c.key.pitchClasses();
            const bool popCase = c.p.slots.back().chord.root == 5;
            for (std::uint32_t seed = 1; seed <= 100; ++seed)
            {
                const auto m = generateMelody (c.p, c.key, ctx, seed, settingsFor (style));
                const auto off = generateMelody (c.p, c.key, ctx, seed, settingsFor (style, 0.5f, 0, MelodyContour::Auto, 0.5f, false));
                const auto st = analyseMelody (m, c.p, c.key), stOff = analyseMelody (off, c.p, c.key);
                ++runs;
                const int last = pc (m.notes.back().note);
                const bool family = last == k[0] || last == k[2] || last == k[4];
                home += st.loopEndHome ? 1 : 0;
                homeOff += stOff.loopEndHome ? 1 : 0;
                tonicFamily += family ? 1 : 0;
                nctFamily += family && ! isChordTone (c.p, m.notes.back()) ? 1 : 0;
                popRuns += popCase ? 1 : 0;
                popFamily += popCase && family ? 1 : 0;
                CHECK (st.seamInterval <= 12);
                CHECK (stOff.seamInterval <= 12);
            }
        }
        std::printf ("    %-10s loop end home %.2f (resolveEnding off %.2f); tonic family %.2f, %.2f of them non-chord tones; "
                     "popAxis tonic family %.2f\n", melodyStyleShortName (style), home / (double) runs, homeOff / (double) runs,
                     tonicFamily / (double) runs, nctFamily / (double) std::max (1, tonicFamily), popFamily / (double) popRuns);
        CHECK (home >= 0.65 * runs);
        CHECK (home - homeOff >= 0.15 * runs);
        CHECK (popFamily >= 0.5 * popRuns);
    }
}

TEST_CASE ("melody gen: resolveEnding has an effect on loops that end on V")
{
    // Am - F - C - G and C - Am - Dm - G in C major: the tonic C clashes with G's B, so before the family boost
    // (kResolveFamilyStrong) the toggle hardly changed these loop ends. G holds the tonic-family G, so home = tonic family.
    const Progression progressions[] = { prog ({ { 9, Q::Minor, 4 }, { 5, Q::Major, 4 }, { 0, Q::Major, 4 }, { 7, Q::Major, 4 } }),
                                         prog ({ { 0, Q::Major, 4 }, { 9, Q::Minor, 4 }, { 2, Q::Minor, 4 }, { 7, Q::Major, 4 } }) };
    for (const auto& p : progressions)
    {
        const auto ctx = contextFor (p);
        for (auto style : kAllStyles)
        {
            int on = 0, off = 0;
            for (std::uint32_t seed = 1; seed <= 100; ++seed)
            {
                on += analyseMelody (generateMelody (p, kCMajor, ctx, seed, settingsFor (style)), p, kCMajor).loopEndHome ? 1 : 0;
                off += analyseMelody (generateMelody (p, kCMajor, ctx, seed, settingsFor (style, 0.5f, 0, MelodyContour::Auto, 0.5f, false)),
                                      p, kCMajor).loopEndHome ? 1 : 0;
            }
            std::printf ("    %-10s %s ... G: loop end home %.2f (resolveEnding off %.2f)\n", melodyStyleShortName (style),
                         p.slots[0].chord.root == 9 ? "Am - F - C" : "C - Am - Dm", on / 100.0, off / 100.0);
            CHECK (on >= 55);
            CHECK (on - off >= 10);
        }
    }
}

TEST_CASE ("melody gen: R3 closed block ends over a chord without the tonic keep its chord tones")
{
    // A closed block end (not the loop end) over G in A minor (bVII) or C major (V): R3's closed boost (tonic x1.6)
    // applies to chord tones only, so the non-chord tonic no longer outweighs G's own tones.
    struct Case { Progression p; Key key; };
    const Case cases[] = { { repeated (epicMinor(), 2), kAMinor },
                           { repeated (prog ({ { 0, Q::Major, 4 }, { 9, Q::Minor, 4 }, { 5, Q::Major, 4 }, { 7, Q::Major, 4 } }), 2), kCMajor } };
    // Driving pedal plans are left out (their cadences used to be pedal notes, see the pedal test).
    int allCadences = 0, allTonics = 0, allChordTones = 0;
    for (auto style : kAllStyles)
    {
        int cadences = 0, chordTones = 0, tonics = 0;
        for (const auto& c : cases)
        {
            const auto ctx = contextFor (c.p);
            for (std::uint32_t seed = 1; seed <= 200; ++seed)
            {
                const auto set = settingsFor (style);
                const auto sig = melodyPlanSignature (c.p, c.key, seed, set);
                if (sig.back() == '1')
                    continue;
                const auto m = generateMelody (c.p, c.key, ctx, seed, set);
                for (const auto& cad : cadenceNotes (sig, m, c.p.totalBeats()))
                {
                    const auto& n = m.notes[cad.index];
                    if (! cad.closed || cad.loopEnd || chordAtBeat (c.p, n.startBeat)->root != 7)
                        continue;
                    ++cadences;
                    chordTones += isChordTone (c.p, n) ? 1 : 0;
                    tonics += pc (n.note) == c.key.tonic ? 1 : 0;   // the tonic is never a tone of G here
                }
            }
        }
        std::printf ("    %-10s closed block ends over G: %.2f chord tones, %.2f the non-chord tonic (%d)\n",
                     melodyStyleShortName (style), chordTones / (double) std::max (1, cadences),
                     tonics / (double) std::max (1, cadences), cadences);
        CHECK (cadences >= 40);
        CHECK (chordTones >= 0.6 * cadences);
        allCadences += cadences;
        allTonics += tonics;
        allChordTones += chordTones;
    }
    CHECK (allTonics <= 0.11 * allCadences);          // 0.14 before the cap
    CHECK (allChordTones >= 0.69 * allCadences);      // 0.66 before the cap
}

TEST_CASE ("melody gen: R6 a note held into a clashing chord is cut (suspensions only in Lyrical / Sparse)")
{
    const Progression progressions[] = { secondaryBorrowed(), twoBeat(), slashAndShort(), mixedLengths(),
                                         prog ({ { 0, Q::Major, 4 }, { 4, Q::Minor, 4 }, { 5, Q::Major7, 4 }, { 11, Q::Diminished, 4 } }),
                                         heldIntoInversions() };
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
                        const int npc = pc (n.note);
                        const int bass = slotBassPitchClass (p.slots[(size_t) sl], ctx.voicings[(size_t) sl]);
                        const bool clash = isTrueClash (chord, bass, npc);
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

TEST_CASE ("melody gen: density is monotonic, Busy reaches its target and Range moves the register")
{
    const auto p = repeated (epicMinor(), 2);
    const auto ctx = contextFor (p);
    const auto pop = repeated (popAxis(), 2);
    const auto popCtx = contextFor (pop);
    // GP 1.3.1 notes per active bar at density 0 / 0.5 / 1 (final-spec 5.4.2: Sparse / Medium / Busy = 0.15 / 0.5 /
    // 0.85 on the piecewise-linear map).
    const double npbMap[4][3] = { { 4.5, 5.8, 7.5 }, { 2.0, 3.4, 4.5 }, { 8.0, 10.9, 16.0 }, { 1.0, 1.5, 2.5 } };
    for (int si = 0; si < 4; ++si)
    {
        const auto style = kAllStyles[si];
        double previous = 0.0;
        std::printf ("    %-10s notes per active bar (mapped target):", melodyStyleShortName (style));
        for (float d : { 0.15f, 0.5f, 0.85f })
        {
            Pool pool;
            for (std::uint32_t seed = 1; seed <= 100; ++seed)
            {
                pool.add (generateMelody (p, kAMinor, ctx, seed, settingsFor (style, d)), p, kAMinor);
                pool.add (generateMelody (pop, kCMajor, popCtx, seed, settingsFor (style, d)), pop, kCMajor);
            }
            CHECK (pool.npb() > previous);
            previous = pool.npb();

            const auto& m = npbMap[si];
            const double target = d < 0.5f ? m[0] + (m[1] - m[0]) * 2.0 * d : m[1] + (m[2] - m[1]) * (2.0 * d - 1.0);
            std::printf ("  %.2f: %.2f (%.2f)", d, pool.npb(), target);
            // Busy reaches its mapped target within 10% (Sparse and Lyrical: at least 90% of it, their templates used
            // to cap Busy about 25% short).
            if (d > 0.8f)
            {
                CHECK (pool.npb() >= 0.9 * target);
                CHECK (pool.npb() <= 1.1 * target);
            }
        }
        std::printf ("\n");
    }
    for (auto style : kAllStyles)
    {
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
    // Uplifting and Driving anchor at a voicing top of at least 67 (the lead sits over the pad); Lyrical does not.
    CHECK (melodyRegisterWindow (p, v, settingsFor (MelodyStyle::Uplifting)) == std::make_pair (62, 74));
    CHECK (melodyRegisterWindow (p, v, settingsFor (MelodyStyle::Driving)) == std::make_pair (53, 74));
    CHECK (melodyRegisterWindow (p, v, settingsFor (MelodyStyle::Lyrical)) == std::make_pair (60, 73));

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
    CHECK_EQ (st.downbeatNotes, 3);
    CHECK_EQ (st.strongNotes, 4);
    CHECK_EQ (st.phraseFinals, 5);
    CHECK (! st.loopEndHome);        // F is not 1, 3 or 5 of C major, and the last chord F holds the tonic
    CHECK (st.loopEndChordTone);     // F over F

    // Loop end: a short G in the final beat over F anticipates the restart chord C (chord fit, and home); the same G
    // earlier in the bar fits neither F nor the restart. The spec's phrase-final share still judges it against F.
    {
        Melody a;
        a.notes = { mn (72, 0.0, 2.0), mn (69, 8.0, 2.0), mn (67, 15.5, 0.5) };
        const auto sa = analyseMelody (a, p, kCMajor);
        CHECK (sa.loopEndChordTone);
        CHECK (sa.loopEndHome);
        CHECK_EQ (sa.phraseFinalChordToneShare, 2.0 / 3.0);   // C over C, A over Am; G over F is not
        a.notes.back().startBeat = 14.0;
        CHECK (! analyseMelody (a, p, kCMajor).loopEndChordTone);
    }
    // Home through the seam: over epicMinor's last chord G (none of A, C, E) a tone of G a step from the first note.
    {
        Melody h;
        h.notes = { mn (69, 0.0, 1.0), mn (67, 12.0, 1.0) };   // A ... G (2 below A)
        CHECK (analyseMelody (h, epicMinor(), kAMinor).loopEndHome);
        h.notes.back().note = 74;                              // D: a tone of G, but 5 from A
        CHECK (! analyseMelody (h, epicMinor(), kAMinor).loopEndHome);
        h.notes.back().note = 69;                              // A: the tonic (a non-chord tone over G)
        CHECK (analyseMelody (h, epicMinor(), kAMinor).loopEndHome);
    }
    // No note on a downbeat: the share is 0 with a zero count (the demos report prints n/a).
    {
        Melody o;
        o.notes = { mn (72, 0.5, 0.5), mn (76, 1.5, 0.5), mn (79, 2.5, 0.5) };
        const auto so = analyseMelody (o, p, kCMajor);
        CHECK_EQ (so.downbeatNotes, 0);
        CHECK_EQ (so.strongNotes, 0);
        CHECK_EQ (so.downbeatChordToneShare, 0.0);
        CHECK_EQ (so.phraseFinals, 1);
    }

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

    // Repetition is audible: the share of bars that exactly repeat an earlier bar (onsets and pitches) rises with it,
    // and Loop-like restates at least 40% of the bars of an 8-bar Uplifting or Driving loop.
    for (auto style : kAllStyles)
    {
        double shares[3] = {};
        int idx = 0;
        for (float r : { 0.15f, 0.5f, 0.85f })
        {
            int repeatedBars = 0, total = 0;
            for (std::uint32_t seed = 1; seed <= 200; ++seed)
            {
                const auto bars = barsOf (generateMelody (p, kCMajor, ctx, seed, settingsFor (style, 0.5f, 0, MelodyContour::Auto, r)), 8);
                for (int b = 1; b < 8; ++b)
                {
                    ++total;
                    for (int e = 0; e < b; ++e)
                        if (! bars[(size_t) b].empty() && bars[(size_t) b] == bars[(size_t) e])
                        {
                            ++repeatedBars;
                            break;
                        }
                }
            }
            shares[idx++] = repeatedBars / (double) total;
        }
        std::printf ("    %-10s bars that restate an earlier bar: varied %.2f, balanced %.2f, loop-like %.2f\n",
                     melodyStyleShortName (style), shares[0], shares[1], shares[2]);
        CHECK (shares[0] < shares[1]);
        CHECK (shares[1] < shares[2]);
        if (style == MelodyStyle::Uplifting || style == MelodyStyle::Driving)
            CHECK (shares[2] >= 0.4);
    }
}

TEST_CASE ("melody gen: repeated units restate their motif (exact pitches among lag-4 rhythm copies)")
{
    // GP 1.5.4 over the same chords: exact .53 (Uplifting, Lyrical) / .37 (Driving), plus fit-transposes by 0.
    struct Case { Progression p; Key key; };
    const Case cases[] = { { repeated (popAxis(), 2), kCMajor }, { repeated (epicMinor(), 2), kAMinor } };
    const double floors[] = { 0.35, 0.35, 0.25, 0.0 };   // Sparse: printed only
    for (int si = 0; si < 4; ++si)
    {
        int pairs = 0, exact = 0;
        for (const auto& c : cases)
        {
            const auto ctx = contextFor (c.p);
            for (std::uint32_t seed = 1; seed <= 200; ++seed)
            {
                const auto bars = barsOf (generateMelody (c.p, c.key, ctx, seed, settingsFor (kAllStyles[si])), 8);
                for (int b = 4; b < 8; ++b)
                    if (! bars[(size_t) b].empty() && sameRhythm (bars[(size_t) b], bars[(size_t) b - 4]))
                    {
                        ++pairs;
                        exact += bars[(size_t) b] == bars[(size_t) b - 4] ? 1 : 0;
                    }
            }
        }
        const double share = exact / (double) std::max (1, pairs);
        std::printf ("    %-10s exact pitches in %.2f of %d lag-4 rhythm copies\n", melodyStyleShortName (kAllStyles[si]), share, pairs);
        CHECK (pairs > 200);
        CHECK (share >= floors[si]);
    }
}

TEST_CASE ("melody gen: Driving pedal mode keeps one pedal register per chord and repeats with its riff")
{
    const auto p = repeated (popAxis(), 2);
    const auto ctx = contextFor (p);
    const auto set = settingsFor (MelodyStyle::Driving);
    int pedalMelodies = 0, pairs = 0, exact = 0, pedalNotes = 0, onRootOrFifth = 0, registerJumps = 0, fifthBelow = 0;
    int cadencesOnPedalSlots = 0, cadencesPlayingPedal = 0;
    for (std::uint32_t seed = 1; seed <= 300; ++seed)
    {
        const auto sig = melodyPlanSignature (p, kCMajor, seed, set);
        if (sig.back() != '1')
            continue;
        ++pedalMelodies;
        const auto m = generateMelody (p, kCMajor, ctx, seed, set);
        const auto bars = barsOf (m, 8);
        std::vector<double> cadenceBeats;
        for (const auto& c : cadenceNotes (sig, m, p.totalBeats()))
            cadenceBeats.push_back (m.notes[c.index].startBeat);
        auto isCadence = [&] (double beat) {
            return std::any_of (cadenceBeats.begin(), cadenceBeats.end(), [&] (double b) { return std::abs (b - beat) < 1e-9; });
        };
        std::array<int, 12> rootPitch;
        rootPitch.fill (-1);
        std::vector<std::pair<double, int>> cadencesOnPedal;   // (beat, pitch) of cadence notes on a pedal slot
        for (int b = 0; b < 8; ++b)
        {
            if (b >= 4 && ! bars[(size_t) b].empty() && sameRhythm (bars[(size_t) b], bars[(size_t) b - 4]))
            {
                ++pairs;
                exact += bars[(size_t) b] == bars[(size_t) b - 4] ? 1 : 0;
            }
            // The 2nd, 4th, ... onset of each bar is a pedal note (a cadence note, the loop end included, takes the
            // cadence pick instead).
            for (size_t j = 1; j < bars[(size_t) b].size(); j += 2)
            {
                const double beat = b * 4.0 + bars[(size_t) b][j].first / 4.0;
                const int note = bars[(size_t) b][j].second;
                if (isCadence (beat))
                {
                    if (std::abs (beat - m.notes.back().startBeat) > 1e-9)
                        cadencesOnPedal.push_back ({ beat, note });
                    continue;
                }
                const int root = pc (chordAtBeat (p, beat)->root);
                ++pedalNotes;
                if (pc (note) == root)
                {
                    ++onRootOrFifth;
                    if (rootPitch[(size_t) root] < 0)
                        rootPitch[(size_t) root] = note;
                    registerJumps += rootPitch[(size_t) root] != note ? 1 : 0;
                }
                else if (pc (note) == pc (root + 7))
                {
                    ++onRootOrFifth;
                    // The 5th sits a fifth above this chord's pedal root (never a fourth below it inside the window).
                    if (rootPitch[(size_t) root] >= 0 && note < rootPitch[(size_t) root])
                        ++fifthBelow;
                }
            }
        }
        // A block-end cadence on a pedal slot is picked as a cadence, not replaced by the low pedal root / 5th.
        for (const auto& [beat, note] : cadencesOnPedal)
        {
            const int root = pc (chordAtBeat (p, beat)->root);
            ++cadencesOnPedalSlots;
            cadencesPlayingPedal += rootPitch[(size_t) root] >= 0
                                     && (note == rootPitch[(size_t) root] || note == rootPitch[(size_t) root] + 7) ? 1 : 0;
        }
    }
    const double share = exact / (double) std::max (1, pairs);
    std::printf ("    %d pedal melodies: %d pedal notes, root / 5th %.3f, exact pitches in %.2f of %d lag-4 rhythm copies; "
                 "%d of %d cadences on a pedal slot play the pedal\n",
                 pedalMelodies, pedalNotes, onRootOrFifth / (double) std::max (1, pedalNotes), share, pairs,
                 cadencesPlayingPedal, cadencesOnPedalSlots);
    CHECK (cadencesOnPedalSlots >= 30);
    CHECK (cadencesPlayingPedal <= 0.25 * cadencesOnPedalSlots);
    CHECK (pedalMelodies >= 100);
    CHECK (onRootOrFifth >= 0.97 * pedalNotes);
    CHECK_EQ (registerJumps, 0);
    CHECK_EQ (fifthBelow, 0);
    CHECK (share >= 0.25);
}

TEST_CASE ("melody gen: the loop's last note does not clash with its chord")
{
    // popAxis ends on IV (E over F was the classic clash), C - Am - F - G on V (C over G).
    const Progression progressions[] = { popAxis(), prog ({ { 0, Q::Major, 4 }, { 9, Q::Minor, 4 }, { 5, Q::Major, 4 }, { 7, Q::Major, 4 } }) };
    for (const auto& p : progressions)
    {
        const auto ctx = contextFor (p);
        int runs = 0, clashes = 0, onLastChord = 0;
        for (auto style : kAllStyles)
            for (std::uint32_t seed = 1; seed <= 200; ++seed)
            {
                const auto m = generateMelody (p, kCMajor, ctx, seed, settingsFor (style));
                const auto& n = m.notes.back();
                const int slot = p.slotAtBeat (n.startBeat);   // a loop may end with a rest after an earlier chord
                const int bass = slotBassPitchClass (p.slots[(size_t) slot], ctx.voicings[(size_t) slot]);
                ++runs;
                onLastChord += slot == (int) p.slots.size() - 1 ? 1 : 0;
                clashes += isTrueClash (p.slots[(size_t) slot].chord, bass, n.note) ? 1 : 0;
            }
        std::printf ("    last note clashes with its chord in %.3f of loops (%.2f end over the last chord)\n", clashes / (double) runs,
                     onLastChord / (double) runs);
        CHECK (clashes <= 0.05 * runs);
    }
}

TEST_CASE ("melody gen: non-chord tones do not pile up and avoid notes stay off the strong beats")
{
    struct Case { Progression p; Key key; };
    const Case cases[] = { { popAxis(), kCMajor }, { epicMinor(), kAMinor }, { secondaryBorrowed(), kCMajor } };
    long appoggiaturas = 0, downToChordTone = 0;   // strong-beat avoid notes reached by a leap and left by a step
    for (auto style : kAllStyles)
    {
        long notes = 0, inRun3 = 0, sameRun3 = 0, strong = 0, strongAvoid = 0;
        for (const auto& c : cases)
        {
            const auto ctx = contextFor (c.p);
            for (std::uint32_t seed = 1; seed <= 200; ++seed)
            {
                const auto m = generateMelody (c.p, c.key, ctx, seed, settingsFor (style));
                const size_t n = m.notes.size();
                std::vector<char> nct (n);
                for (size_t i = 0; i < n; ++i)
                    nct[i] = isChordTone (c.p, m.notes[i]) ? 0 : 1;
                notes += (long) n;
                for (size_t i = 0; i < n;)
                {
                    size_t j = i;
                    while (j < n && nct[j])
                        ++j;
                    inRun3 += j - i >= 3 ? (long) (j - i) : 0;
                    i = j == i ? i + 1 : j;
                }
                for (size_t i = 0; i < n;)
                {
                    size_t j = i + 1;
                    while (j < n && nct[i] && nct[j] && m.notes[j].note == m.notes[i].note)
                        ++j;
                    sameRun3 += nct[i] && j - i >= 3 ? (long) (j - i) : 0;
                    i = j;
                }
                if (&c == &cases[2])
                    continue;   // R2 is measured over the diatonic loops
                for (size_t i = 0; i < n; ++i)
                {
                    const int step = (int) std::lround (m.notes[i].startBeat * 4.0) % 16;
                    if (step != 0 && step != 8)
                        continue;
                    ++strong;
                    MelodyNote below = m.notes[i];
                    below.note -= 1;
                    const bool avoid = nct[i] && isChordTone (c.p, below);
                    strongAvoid += avoid ? 1 : 0;
                    // R2 waives the avoid penalty only for an appoggiatura that resolves down to that chord tone.
                    if (avoid && i > 0 && i + 1 < n && std::abs (m.notes[i].note - m.notes[i - 1].note) >= 3)
                    {
                        const int out = m.notes[i + 1].note - m.notes[i].note;
                        if (std::abs (out) >= 1 && std::abs (out) <= 2)
                        {
                            ++appoggiaturas;
                            downToChordTone += out == -1 && ! nct[i + 1] ? 1 : 0;
                        }
                    }
                }
            }
        }
        std::printf ("    %-10s notes in runs of >= 3 non-chord tones %.3f (same pitch %.3f), strong-beat avoid notes %.3f\n",
                     melodyStyleShortName (style), inRun3 / (double) notes, sameRun3 / (double) notes, strongAvoid / (double) strong);
        CHECK (inRun3 <= 0.05 * notes);
        CHECK (sameRun3 <= 0.02 * notes);
        CHECK (strongAvoid <= 0.04 * strong);
    }
    std::printf ("    strong-beat avoid appoggiaturas resolving down to the chord tone: %ld of %ld\n", downToChordTone, appoggiaturas);
    CHECK (appoggiaturas >= 20);
    CHECK (downToChordTone >= 0.8 * appoggiaturas);
}

TEST_CASE ("melody gen: a forced contour is audible")
{
    struct Case { Progression p; Key key; };
    const Case cases[] = { { popAxis(), kCMajor }, { epicMinor(), kAMinor } };
    for (auto style : kAllStyles)
        for (const auto& c : cases)
        {
            const auto ctx = contextFor (c.p);
            int rising = 0, falling = 0, runs = 0;
            for (std::uint32_t seed = 1; seed <= 200; ++seed)
            {
                double d[2] = { 0.0, 0.0 };
                for (int k = 0; k < 2; ++k)
                {
                    const auto m = generateMelody (c.p, c.key, ctx, seed,
                                                   settingsFor (style, 0.5f, 0, k == 0 ? MelodyContour::Rising : MelodyContour::Falling));
                    double sum[4] = {}, cnt[4] = {};
                    for (const auto& n : m.notes)
                    {
                        const int bar = std::min (3, (int) (n.startBeat / 4.0));
                        sum[bar] += n.note;
                        cnt[bar] += 1.0;
                    }
                    int first = -1, last = -1;
                    for (int b = 0; b < 4; ++b)
                        if (cnt[b] > 0)
                        {
                            first = first < 0 ? b : first;
                            last = b;
                        }
                    d[k] = sum[last] / cnt[last] - sum[first] / cnt[first];
                }
                ++runs;
                rising += d[0] >= 3.0 ? 1 : 0;
                falling += d[1] <= -3.0 ? 1 : 0;
            }
            std::printf ("    %-10s last bar - first bar >= +3 with Rising %.2f, <= -3 with Falling %.2f\n", melodyStyleShortName (style),
                         rising / (double) runs, falling / (double) runs);
            CHECK (rising >= 0.7 * runs);
            CHECK (falling >= 0.6 * runs);
        }
}

TEST_CASE ("melody gen: R6 cuts a chord tone held into a b9 over the new bass")
{
    // C held from C into Cmaj7/B, F held from F into Fmaj7/E: both are chord tones of the new chord, a b9 over its bass.
    const auto p = heldIntoInversions();
    const auto ctx = contextFor (p);
    CHECK_EQ (slotBassPitchClass (p.slots[1], ctx.voicings[1]), 11);
    CHECK_EQ (slotBassPitchClass (p.slots[3], ctx.voicings[3]), 4);
    int crossings = 0, b9 = 0;
    for (auto style : { MelodyStyle::Uplifting, MelodyStyle::Driving })
        for (std::uint32_t seed = 1; seed <= 200; ++seed)
            for (const auto& n : generateMelody (p, kCMajor, ctx, seed, settingsFor (style, 0.3f)).notes)
                for (int sl : { 1, 3 })
                {
                    const double boundary = p.slotStartBeat (sl);
                    if (n.startBeat >= boundary || n.startBeat + n.lengthBeats <= boundary + 1e-9)
                        continue;
                    ++crossings;
                    b9 += pc (n.note) == (sl == 1 ? 0 : 5) ? 1 : 0;
                }
    std::printf ("    %d notes ring into the inverted chords, %d of them a b9 over the bass\n", crossings, b9);
    CHECK (crossings > 20);
    CHECK_EQ (b9, 0);
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
