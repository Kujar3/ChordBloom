#include "core/MelodyGen.h"
#include "core/MelodyGenTables.h"

#include <algorithm>
#include <array>
#include <cmath>

// Melody generator (final-spec 4.6 / 5.4). Names, the mood table and MelodyGenSettings::operator== are WP0's
// (frozen behaviour); the generator itself is WP1's.
//
// Pipeline (final-spec 5.4.3; every number lives in MelodyGenTables.h):
//   0. Setup: per slot the local chord-scale (R1: a key tone is replaced by the same-letter out-of-key chord tone),
//      the chord-tone kinds, the bass pitch class; the register window (GP 1.4.6 + registerShift + R8).
//   1. Plan (stream `plan`): unit length (1 or 2 bars), form letters per 4-unit block (repetition tilts the weights,
//      the pick is an inverse CDF over the forms sorted most repetitive first), the gap plan (continuous loop, or
//      rests at unit ends with phrase-start steps and pickups), cadence tags and the Driving pedal mode. Loops longer
//      than 8 units repeat the 8-unit plan. Cadence points: every gap, every half block (unit 2 of 4: the
//      "question"), every block end and the loop end; block ends and the loop end are closed, the rest open (R3).
//   2. Rhythm (one stream per unit): a new letter draws whole-bar templates or density-tilted beat cells (Sparse: a
//      k-of-16 slot draw) under the odd-16th cap; a repeated letter copies its first unit's rhythm (varied: 1-2
//      onsets of its last bar changed); a new letter copies A's rhythm with the style's probability. Gaps, delayed
//      phrase starts and pickups come from the plan. Above 512 onsets the target is lowered once.
//   3. Pitch (stream `pitch`, a fixed number of draws per unit, phrase and note, so a setting that only moves the
//      register or the contour keeps the same "idea"): new notes = repeat-or-move, chord tone or not by position,
//      a non-chord-tone construction with one-note look-ahead, then a weighted pick Wstep x Wreg x Wcontour x Wkind
//      x Wleap x Wnct x Wavoid over the window. Repeated units: exact (adapted to the new chord-scale) / fit-
//      transposed / ending change / re-picked along the original pitches. Driving pedal mode alternates a low root
//      (or fifth) with the upper line. Cadence notes are picked inline with the cadence table x approach x R3; the
//      loop end with resolveEnding uses the key-relative row with the tonic x2; R7 measures the last note to the first.
//   4. Articulation (stream `artic`): IOI x gate class, phrase-final lengths, Lyrical / Sparse holds; R6 cuts a note
//      held into a clashing chord at the boundary (Lyrical / Sparse keep a step-down suspension).
// Not implemented (P2): R4 clash costs, R5 bass awareness, chromatic passing tones.

namespace cb
{
const char* melodyStyleName (MelodyStyle s)
{
    static const char* names[] = { "Uplifting lead", "Lyrical hook", "Driving riff", "Sparse" };
    static_assert (sizeof (names) / sizeof (names[0]) == (size_t) numMelodyStyles, "one name per MelodyStyle");
    return names[std::clamp ((int) s, 0, numMelodyStyles - 1)];
}

const char* melodyStyleShortName (MelodyStyle s)
{
    static const char* names[] = { "Uplifting", "Lyrical", "Driving", "Sparse" };
    static_assert (sizeof (names) / sizeof (names[0]) == (size_t) numMelodyStyles, "one name per MelodyStyle");
    return names[std::clamp ((int) s, 0, numMelodyStyles - 1)];
}

const char* melodyStyleDescription (MelodyStyle s)
{
    static const char* lines[] = {
        "Anthemic lead: chord tones on the beat, rising phrases, a big ending",
        "Singable hook: mostly steps, held notes, question-and-answer phrases",
        "Busy riff: short repeated cells and pedal notes in tight 16ths",
        "A few long notes that leave room for the chords",
    };
    static_assert (sizeof (lines) / sizeof (lines[0]) == (size_t) numMelodyStyles, "one line per MelodyStyle");
    return lines[std::clamp ((int) s, 0, numMelodyStyles - 1)];
}

MelodyStyle melodyStyleForMood (Mood mood)
{
    switch (mood)
    {
        case Mood::Uplifting: return MelodyStyle::Uplifting;
        case Mood::Emotional: return MelodyStyle::Lyrical;
        case Mood::Dark:      return MelodyStyle::Driving;
        case Mood::Dreamy:    return MelodyStyle::Sparse;
        case Mood::Epic:      return MelodyStyle::Uplifting;
        case Mood::LoFi:      return MelodyStyle::Lyrical;
        case Mood::NumMoods:
        default:              break;
    }
    return MelodyStyle::Lyrical;
}

const char* melodyContourName (MelodyContour c)
{
    static const char* names[] = { "Auto", "Rising", "Falling", "Arch", "Wave" };
    static_assert (sizeof (names) / sizeof (names[0]) == (size_t) numMelodyContours, "one name per MelodyContour");
    return names[std::clamp ((int) c, 0, numMelodyContours - 1)];
}

bool MelodyGenSettings::operator== (const MelodyGenSettings& o) const
{
    return style == o.style && density == o.density && repetition == o.repetition && registerShift == o.registerShift
        && contour == o.contour && resolveEnding == o.resolveEnding;
}

namespace
{
using namespace melodygen;

constexpr int kBarSteps = 16;
constexpr int kBeatSteps = 4;
constexpr int kDefaultVoicingTop = 68;   // GP 1.4.6: corpus median lead 70 minus 2
constexpr int kLyrical = 1, kDriving = 2, kSparse = 3;

//==============================================================================
// Deterministic PRNG: xorshift32 behind a murmur3 finaliser (like Spark), one stream per stage.
std::uint32_t fmix (std::uint32_t h)
{
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

std::uint32_t mixIn (std::uint32_t h, std::uint32_t v)
{
    return fmix (h ^ (fmix (v + 0x9E3779B9u) + 0x7F4A7C15u + (h << 6) + (h >> 2)));
}

struct Rng
{
    std::uint32_t state;

    explicit Rng (std::uint32_t seed)
    {
        const std::uint32_t s = fmix (seed);
        state = s != 0 ? s : 0x6D2B79F5u;
    }

    std::uint32_t next()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    double uniform() { return (double) (next() >> 8) * (1.0 / 16777216.0); }   // [0, 1)
};

int floorDiv (int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
int sgn (int v) { return (v > 0) - (v < 0); }
int popcount16 (unsigned m)
{
    int n = 0;
    for (; m != 0; m &= m - 1)
        ++n;
    return n;
}

// Index i with probability w[i] / sum (w) for a uniform u in [0, 1); -1 when no weight is positive.
template <typename Weights>
int pickIndex (const Weights& w, int n, double u)
{
    double total = 0.0;
    for (int i = 0; i < n; ++i)
        total += std::max (0.0, (double) w[i]);

    if (! (total > 0.0))
        return -1;

    double x = u * total;
    int last = -1;

    for (int i = 0; i < n; ++i)
    {
        const double wi = std::max (0.0, (double) w[i]);
        if (wi <= 0.0)
            continue;
        last = i;
        if (x < wi)
            return i;
        x -= wi;
    }

    return last;
}

int styleIndex (MelodyStyle s) { return std::clamp ((int) s, 0, kStyles - 1); }

MelodyGenSettings clampSettings (const MelodyGenSettings& in)
{
    MelodyGenSettings s = in;
    auto unit = [] (float v) { return std::isfinite (v) ? std::clamp (v, 0.0f, 1.0f) : 0.5f; };
    s.density = unit (s.density);
    s.repetition = unit (s.repetition);
    s.registerShift = std::clamp (s.registerShift, -12, 12);
    s.style = (MelodyStyle) styleIndex (s.style);
    s.contour = (MelodyContour) std::clamp ((int) s.contour, 0, numMelodyContours - 1);
    return s;
}

// base = mix (seed, key, style, slots); density, repetition, registerShift, contour and resolveEnding stay out.
std::uint32_t baseHash (const Progression& prog, const Key& key, std::uint32_t seed, int style)
{
    std::uint32_t h = mixIn (0xC0DE5EEDu, seed);
    h = mixIn (h, (std::uint32_t) pc (key.tonic));
    h = mixIn (h, (std::uint32_t) key.scale);
    h = mixIn (h, (std::uint32_t) style);

    std::uint32_t slots = 0x51A7B10Cu;
    for (const auto& s : prog.slots)
    {
        slots = mixIn (slots, (std::uint32_t) pc (s.chord.root));
        slots = mixIn (slots, (std::uint32_t) s.chord.quality);
        slots = mixIn (slots, (std::uint32_t) (s.inversion + 1));
        slots = mixIn (slots, (std::uint32_t) s.lengthBeats);
    }

    return mixIn (h, slots);
}

std::uint32_t streamSeed (std::uint32_t base, std::uint32_t stage) { return mixIn (base, stage); }
std::uint32_t streamSeed (std::uint32_t base, std::uint32_t stage, std::uint32_t index)
{
    return mixIn (mixIn (base, stage), index + 1);
}

int totalStepsOf (const Progression& prog) { return std::max (0, prog.totalBeats()) * kBeatSteps; }

//==============================================================================
// Register window (GP 1.4.6, registerShift, R8).
struct Window
{
    int lo = 60, hi = 84;
    double centre = 72.0;
    int arpFloor = -1;   // R8: candidates below this get Wreg x0.5 (-1 = no arp)
};

int medianOf (std::vector<int> v)
{
    if (v.empty())
        return 0;
    std::sort (v.begin(), v.end());
    return v[(v.size() - 1) / 2];
}

Window computeWindow (const Progression& prog, const MelodyGenContext& ctx, const MelodyGenSettings& s)
{
    const auto& sp = kStyleParams[styleIndex (s.style)];

    std::vector<int> tops;
    const size_t numVoicings = prog.empty() ? ctx.voicings.size() : std::min (ctx.voicings.size(), prog.slots.size());
    for (size_t i = 0; i < numVoicings; ++i)
    {
        int top = -1;
        for (int n : ctx.voicings[i])
            if (n >= 0 && n <= 127)
                top = std::max (top, n);
        if (top >= 0)
            tops.push_back (top);
    }

    const int top = tops.empty() ? kDefaultVoicingTop : medianOf (tops);

    Window w;
    w.lo = top + sp.regLo + s.registerShift;
    w.hi = top + sp.regHi + s.registerShift;
    w.centre = top + sp.regC + s.registerShift;

    std::vector<int> arp;
    for (const auto& e : ctx.accompaniment)
        if (e.channel == 4 && e.note >= 0 && e.note <= 127)
            arp.push_back (e.note);

    if (! arp.empty())
    {
        const int m = medianOf (arp);
        const int up = std::clamp (m + kArpOffset - w.lo, 0, kArpMaxShift);
        w.lo += up;
        w.hi += up;
        w.centre += up;
        w.arpFloor = m + kArpOffset;
    }

    w.lo = std::clamp (w.lo, kMelodyLowestNote, kMelodyHighestNote);
    w.hi = std::clamp (w.hi, kMelodyLowestNote, kMelodyHighestNote);

    if (w.hi - w.lo < 12)   // keep at least an octave (a voicing far up or down the keyboard)
    {
        if (w.hi >= kMelodyHighestNote)
            w.lo = kMelodyHighestNote - 12;
        else
            w.hi = std::min (kMelodyHighestNote, w.lo + 12);
        w.lo = std::min (w.lo, w.hi - 12);
    }

    w.centre = std::clamp (w.centre, (double) w.lo, (double) w.hi);
    return w;
}

//==============================================================================
// Per slot harmony: chord tones, the local chord-scale (R1), kinds and classes.
struct SlotInfo
{
    Chord chord;
    int startStep = 0, endStep = 0, lengthBeats = 4;
    int bassPc = 0;
    int func = 0;                       // Wkind row: 0 tonic, 1 subdominant, 2 dominant
    std::array<int, 7> offset {};       // local chord-scale per degree (letter): semitones above the tonic, ascending
    std::array<int, 12> posOfRel {};    // (pc - tonic) mod 12 -> nearest degree position (-1..7)
    std::array<bool, 12> inScale {};    // pc is in the local chord-scale
    std::array<bool, 12> allowed {};    // local chord-scale or chord tone
    std::array<bool, 12> chordTone {};
    std::array<bool, 12> replaced {};   // a key tone the chord's same-letter out-of-key tone replaces (R1)
    std::array<int, 12> kind {};        // chord tones: 0 root, 1 third, 2 fifth, 3 seventh / sixth, 4 extension; else -1
    std::array<int, 12> cadClass {};    // kCadence column of the pc
    std::array<int, 12> degree {};      // pc -> degree 0..6 (letter) for the key-relative rows and R3
};

bool isDominantQuality (ChordQuality q)
{
    switch (q)
    {
        case ChordQuality::Dominant7: case ChordQuality::Dominant9: case ChordQuality::Dominant13:
        case ChordQuality::Dominant7Flat9: case ChordQuality::Dominant7Sus4:
            return true;
        case ChordQuality::Major: case ChordQuality::Minor: case ChordQuality::Diminished: case ChordQuality::Augmented:
        case ChordQuality::Sus2: case ChordQuality::Sus4: case ChordQuality::Major6: case ChordQuality::Minor6:
        case ChordQuality::Add9: case ChordQuality::MinorAdd9: case ChordQuality::Major7: case ChordQuality::Minor7:
        case ChordQuality::HalfDiminished7: case ChordQuality::Diminished7: case ChordQuality::MinorMajor7:
        case ChordQuality::Major9: case ChordQuality::Minor9: case ChordQuality::Minor11: case ChordQuality::Major69:
        case ChordQuality::Major7Sharp11: case ChordQuality::NumQualities:
            break;
    }
    return false;
}

bool isSusQuality (ChordQuality q)
{
    return q == ChordQuality::Sus2 || q == ChordQuality::Sus4 || q == ChordQuality::Dominant7Sus4;
}

int functionRow (const Chord& chord, const Key& key)
{
    switch (harmonicFunction (chord, key))
    {
        case HarmonicFunction::Tonic:       return 0;
        case HarmonicFunction::Subdominant: return 1;
        case HarmonicFunction::Dominant:    return 2;
        case HarmonicFunction::Borrowed:
        case HarmonicFunction::Secondary:
        case HarmonicFunction::Chromatic:   break;
    }
    return isDominantQuality (chord.quality) ? 2 : 0;
}

SlotInfo makeSlotInfo (const ProgressionSlot& ps, const std::vector<int>& voicing, const Key& key, int startStep)
{
    SlotInfo si;
    si.chord = ps.chord;
    si.chord.root = pc (ps.chord.root);
    si.lengthBeats = std::max (1, ps.lengthBeats);
    si.startStep = startStep;
    si.endStep = startStep + si.lengthBeats * kBeatSteps;
    si.bassPc = slotBassPitchClass (ps, voicing);
    si.func = functionRow (si.chord, key);
    si.kind.fill (-1);

    const int tonic = pc (key.tonic);
    const auto keyPcs = key.pitchClasses();
    std::array<char, 7> letters {};

    for (int d = 0; d < 7; ++d)
    {
        si.offset[(size_t) d] = pc (keyPcs[(size_t) d] - tonic);
        const auto name = key.spell (keyPcs[(size_t) d]);
        letters[(size_t) d] = name.empty() ? '?' : name[0];
    }

    const auto& ivs = qualityIntervals (si.chord.quality);

    for (int iv : ivs)
    {
        const int t = pc (si.chord.root + iv);
        if (si.chordTone[(size_t) t])
            continue;
        si.chordTone[(size_t) t] = true;

        int k = 4;
        if (iv == 0)
            k = 0;
        else if (iv < 12)
        {
            if (iv == 3 || iv == 4 || ((iv == 2 || iv == 5) && isSusQuality (si.chord.quality)))
                k = 1;
            else if (iv >= 6 && iv <= 8)
                k = 2;
            else if (iv >= 9)
                k = 3;
        }
        si.kind[(size_t) t] = k;
    }

    // R1: the scale tone with the same letter as an out-of-key chord tone is replaced by that chord tone.
    std::array<bool, 7> degreeReplaced {};
    for (int iv : ivs)
    {
        const int t = pc (si.chord.root + iv);
        if (key.contains (t))
            continue;

        const auto name = spellChordTone (si.chord, iv, key);
        if (name.empty())
            continue;

        int d = -1;
        for (int j = 0; j < 7; ++j)
            if (letters[(size_t) j] == name[0])
                d = j;

        if (d < 0 || degreeReplaced[(size_t) d] || si.chordTone[(size_t) keyPcs[(size_t) d]])
            continue;

        const int newOffset = pc (t - tonic);
        const int below = d > 0 ? si.offset[(size_t) d - 1] : -1;
        const int above = d < 6 ? si.offset[(size_t) d + 1] : 12;
        if (newOffset <= below || newOffset >= above)
            continue;

        si.replaced[(size_t) keyPcs[(size_t) d]] = true;
        si.offset[(size_t) d] = newOffset;
        degreeReplaced[(size_t) d] = true;
    }

    for (int d = 0; d < 7; ++d)
        si.inScale[(size_t) pc (tonic + si.offset[(size_t) d])] = true;

    for (int r = 0; r < 12; ++r)
    {
        int best = 0, bestDist = 1000;
        for (int d = -7; d < 14; ++d)
        {
            const int o = si.offset[(size_t) ((d % 7 + 7) % 7)] + 12 * floorDiv (d, 7);
            const int dist = std::abs (o - r);
            if (dist < bestDist)
            {
                bestDist = dist;
                best = d;
            }
        }
        si.posOfRel[(size_t) r] = best;
        si.degree[(size_t) pc (tonic + r)] = (best % 7 + 7) % 7;
    }

    for (int p = 0; p < 12; ++p)
    {
        si.allowed[(size_t) p] = si.inScale[(size_t) p] || si.chordTone[(size_t) p];

        const int s = pc (p - si.chord.root);
        int c = 0;
        switch (s)
        {
            case 0: c = 0; break;
            case 1: case 2: c = 1; break;
            case 3: case 4: c = 2; break;
            case 5: c = 3; break;
            case 6: c = si.kind[(size_t) p] == 2 ? 4 : 3; break;
            case 7: c = 4; break;
            case 8: c = si.kind[(size_t) p] == 2 ? 4 : 5; break;
            case 9: c = 5; break;
            default: c = 6; break;
        }
        si.cadClass[(size_t) p] = c;
    }

    return si;
}

//==============================================================================
// Plan (stream 1).
struct UnitPlan
{
    int letter = 0;
    int gapBeats = 0;    // rest at the end of the unit (0 = none)
    int startStep = 0;   // after the gap the next phrase starts this many 16ths into the next unit
    int pickup = 0;      // ... or with this many pickup notes at the end of this unit
    char cad = '-';      // cadence point at the end of the unit: 'o' open, 'c' closed
};

struct Plan
{
    int unitBars = 1, unitSteps = kBarSteps, totalSteps = 0;
    std::vector<UnitPlan> units;
    bool pedal = false;

    int numUnits() const { return (int) units.size(); }
    int unitStart (int k) const { return k * unitSteps; }
    int unitEnd (int k) const { return std::min ((k + 1) * unitSteps, totalSteps); }
};

int distinctLetters (const std::string& letters)
{
    int mask = 0;
    for (char c : letters)
        mask |= 1 << std::clamp (c - 'A', 0, 25);
    return popcount16 ((unsigned) (mask & 0xFFFF)) + popcount16 ((unsigned) (mask >> 16));
}

double formRep (const std::string& letters)
{
    const int n = (int) letters.size();
    return n <= 1 ? 1.0 : (double) (n - distinctLetters (letters)) / (double) (n - 1);
}

// Inverse-CDF pick over the forms (sorted most repetitive first) tilted by repetition.
std::string pickForm (int style, int unitTable, int n, double repetition, double u, bool twoUnitTable)
{
    if (n <= 1)
        return "A";

    std::vector<std::pair<std::string, double>> forms;

    if (twoUnitTable)
    {
        forms.push_back ({ "AA", kForm2AA[style] });
        forms.push_back ({ "AB", 1.0 - kForm2AA[style] });
    }
    else
    {
        const auto& table = kForms4[style][unitTable];
        for (int i = 0; i < table.size; ++i)
        {
            const std::string prefix = std::string (table.forms[i].letters).substr (0, (size_t) n);
            auto it = std::find_if (forms.begin(), forms.end(), [&] (const auto& f) { return f.first == prefix; });
            if (it != forms.end())
                it->second += table.forms[i].weight;
            else
                forms.push_back ({ prefix, table.forms[i].weight });
        }
    }

    std::stable_sort (forms.begin(), forms.end(),
                      [] (const auto& a, const auto& b) { return formRep (a.first) > formRep (b.first); });

    std::vector<double> w;
    for (const auto& f : forms)
        w.push_back (f.second * std::exp (kRepetitionTilt * (repetition - 0.5) * formRep (f.first)));

    const int i = pickIndex (w, (int) w.size(), u);
    return forms[(size_t) std::max (0, i)].first;
}

Plan makePlan (std::uint32_t base, int style, double repetition, int totalSteps)
{
    Plan plan;
    plan.totalSteps = totalSteps;
    if (totalSteps <= 0)
        return plan;

    const auto& sp = kStyleParams[style];
    Rng rng (streamSeed (base, 1));

    const double uUnit = rng.uniform(), uForm1 = rng.uniform(), uCopy = rng.uniform(), uForm2 = rng.uniform();
    const double uContinuous = rng.uniform(), uPedal = rng.uniform();

    struct GapDraws { double present, bucket, length, start, pickup; };
    std::array<GapDraws, kPlanUnits> draws {};
    for (auto& d : draws)
        d = { rng.uniform(), rng.uniform(), rng.uniform(), rng.uniform(), rng.uniform() };

    const int numBars = (totalSteps + kBarSteps - 1) / kBarSteps;
    plan.unitBars = (numBars > 1 && uUnit < sp.unit2Bars) ? 2 : 1;
    plan.unitSteps = plan.unitBars * kBarSteps;
    const int numUnits = (totalSteps + plan.unitSteps - 1) / plan.unitSteps;
    const int planUnits = std::min (numUnits, kPlanUnits);
    const int table = plan.unitBars == 2 ? 1 : 0;

    std::string letters;
    if (planUnits <= 2)
        letters = pickForm (style, table, planUnits, repetition, uForm1, true);
    else
    {
        letters = pickForm (style, table, std::min (planUnits, 4), repetition, uForm1, false);

        if (planUnits > 4)
        {
            const int m = planUnits - 4;
            const double p = kSecondBlockCopy[style];
            const double a = std::exp (kRepetitionTilt * (repetition - 0.5));
            const double pCopy = p * a / (p * a + (1.0 - p));

            if (uCopy < pCopy)
                letters += letters.substr (0, (size_t) m);
            else
            {
                const int offset = distinctLetters (letters) - 1;
                for (char c : pickForm (style, table, m, repetition, uForm2, false))
                    letters += c == 'A' ? 'A' : (char) (c + offset);
            }
        }
    }

    const bool continuous = uContinuous < sp.continuous;
    std::vector<UnitPlan> pu ((size_t) planUnits);
    bool anyGap = false;

    for (int k = 0; k < planUnits; ++k)
    {
        pu[(size_t) k].letter = letters[(size_t) k] - 'A';
        const int place = (k % 4 == 3 || k == planUnits - 1) ? 3 : k % 4;
        const bool phraseEnd = plan.unitBars == 2 || k % 2 == 1 || k == planUnits - 1;   // phrases of >= 2 bars
        if (! continuous && phraseEnd && draws[(size_t) k].present < kGapAtUnitEnd[place])
        {
            pu[(size_t) k].gapBeats = 1;
            anyGap = true;
        }
    }

    if (! continuous && ! anyGap)
        pu[(size_t) planUnits - 1].gapBeats = 1;

    for (int k = 0; k < planUnits; ++k)
    {
        auto& u = pu[(size_t) k];
        if (u.gapBeats == 0)
            continue;

        const auto& d = draws[(size_t) k];
        const auto& buckets = kGapBuckets[style];
        std::array<float, 4> bw {};
        for (int b = 0; b < 4; ++b)
            bw[(size_t) b] = buckets[b].weight;
        const auto& bucket = buckets[std::max (0, pickIndex (bw, 4, d.bucket))];
        u.gapBeats = bucket.lo + std::min (bucket.hi - bucket.lo, (int) (d.length * (bucket.hi - bucket.lo + 1)));

        std::array<float, kNumStartSteps> sw {};
        for (int i = 0; i < kNumStartSteps; ++i)
            sw[(size_t) i] = kStartSteps[style][i].weight;
        const int step = kStartSteps[style][std::max (0, pickIndex (sw, kNumStartSteps, d.start))].step;

        if (step == kPickupStep)
            u.pickup = 1 + std::max (0, pickIndex (kPickupNotes, 3, d.pickup));
        else
            u.startStep = step;
    }

    plan.units.resize ((size_t) numUnits);
    for (int k = 0; k < numUnits; ++k)
        plan.units[(size_t) k] = pu[(size_t) (k % planUnits)];

    // Clip the gaps to the real unit lengths (a unit keeps at least one beat of notes).
    for (int k = 0; k < numUnits; ++k)
    {
        auto& u = plan.units[(size_t) k];
        const int unitBeats = (plan.unitEnd (k) - plan.unitStart (k)) / kBeatSteps;
        u.gapBeats = std::min (u.gapBeats, unitBeats - 1);
        if (u.gapBeats <= 0)
        {
            u = UnitPlan { u.letter, 0, 0, 0, '-' };
            continue;
        }
        while (u.pickup > 0 && u.gapBeats * 2 < u.pickup + 2)
            --u.pickup;
    }

    for (int k = 0; k < numUnits; ++k)
    {
        auto& u = plan.units[(size_t) k];
        if (u.startStep <= 0)
            continue;
        const int next = (k + 1) % numUnits;
        const int room = plan.unitEnd (next) - plan.unitStart (next) - plan.units[(size_t) next].gapBeats * kBeatSteps;
        if (u.startStep > room - kBeatSteps)
            u.startStep = 0;
    }

    for (int k = 0; k < numUnits; ++k)
    {
        auto& u = plan.units[(size_t) k];
        const bool last = k == numUnits - 1;
        const bool cadence = u.gapBeats > 0 || k % 4 == 1 || k % 4 == 3 || last;
        u.cad = ! cadence ? '-' : (k % 4 == 3 || last) ? 'c' : 'o';
    }

    plan.pedal = style == kDriving && uPedal < sp.pedalMode;
    return plan;
}

std::string planSignature (const Plan& plan)
{
    if (plan.units.empty())
        return {};

    std::string letters, gaps, cads;
    for (size_t k = 0; k < plan.units.size(); ++k)
    {
        const auto& u = plan.units[k];
        letters += (char) ('A' + u.letter);
        if (k > 0)
        {
            gaps += ',';
            cads += ',';
        }
        if (u.gapBeats == 0)
            gaps += '-';
        else
        {
            gaps += std::to_string (u.gapBeats);
            if (u.pickup > 0)
                gaps += "p" + std::to_string (u.pickup);
            else if (u.startStep > 0)
                gaps += "s" + std::to_string (u.startStep);
        }
        cads += u.cad;
    }

    return "U" + std::to_string (plan.unitBars) + " " + letters + " gap:" + gaps + " cad:" + cads
         + " pedal:" + (plan.pedal ? "1" : "0");
}

//==============================================================================
// Rhythm (one stream per unit).
struct UnitRhythm
{
    std::vector<int> raw;      // onset steps relative to the unit start, before the gap plan
    int proto = -1;            // earlier unit with the same letter (-1 = this unit introduces its letter)
    int relation = kRelExact;  // GP 1.5.2 relation to proto
};

struct Onset
{
    int step = 0;
    int unit = 0;              // the unit whose time span contains the onset
    int slot = 0;
    int pos = 0;               // PosClass
    bool pickup = false;       // a pickup into the next phrase
    bool phraseFinal = false;  // the last own onset of a unit that ends with a gap
    bool cadence = false, closed = false, loopEnd = false;
    int segment = 0;
    double segX = 0.0;         // position inside the contour segment, 0..1
    int segStart = 0;          // first step of the segment
    int segEnd = 0;            // last onset step of the segment
};

struct OddState
{
    double odd = 0.0, total = 0.0;
    void add (unsigned mask)
    {
        total += popcount16 (mask);
        odd += popcount16 (mask & 0xAAAAu);
    }
};

struct RhythmParams
{
    int style = 0;
    double target = 4.0;                        // onsets per bar
    std::array<std::array<double, 8>, 4> cellW; // density-tilted cell weights per beat
};

double expectedCellOnsets (int style, double g)
{
    double e = 0.0;
    for (int b = 0; b < 4; ++b)
    {
        double sw = 0.0, so = 0.0;
        for (const auto& c : kCells[style][b])
        {
            const int n = popcount16 (c.mask);
            const double w = c.weight * std::exp (g * n);
            sw += w;
            so += w * n;
        }
        e += sw > 0.0 ? so / sw : 0.0;
    }
    return e;
}

RhythmParams makeRhythmParams (int style, double target)
{
    RhythmParams rp;
    rp.style = style;
    rp.target = target;

    double lo = -kDensityTiltRange, hi = kDensityTiltRange;
    for (int it = 0; it < 40; ++it)
    {
        const double mid = 0.5 * (lo + hi);
        if (expectedCellOnsets (style, mid) < target)
            lo = mid;
        else
            hi = mid;
    }
    const double g = 0.5 * (lo + hi);

    for (int b = 0; b < 4; ++b)
        for (int i = 0; i < 8; ++i)
        {
            const auto& c = kCells[style][b][i];
            rp.cellW[(size_t) b][(size_t) i] = c.weight * std::exp (g * popcount16 (c.mask));
        }

    return rp;
}

unsigned drawBar (Rng& rng, const RhythmParams& rp, int barLen, OddState& odd)
{
    const auto& sp = kStyleParams[rp.style];
    const unsigned lenMask = barLen >= 16 ? 0xFFFFu : ((1u << barLen) - 1u);
    const double cap = sp.odd16Cap + kOddCapSlack;
    const bool allowEmpty = rp.style == kLyrical || rp.style == kSparse;

    if (rng.uniform() < sp.templateProb)
    {
        const auto& list = kTemplates[rp.style];
        std::array<double, 16> w {};
        for (int i = 0; i < list.size; ++i)
        {
            const double d = popcount16 (list.items[i].mask) - rp.target;
            w[(size_t) i] = list.items[i].weight * std::exp (-0.5 * d * d);
        }
        const int i = pickIndex (w, list.size, rng.uniform());
        if (i >= 0)
        {
            unsigned m = list.items[i].mask & lenMask;
            if (m == 0 && ! allowEmpty)
                m = 1;
            odd.add (m);
            return m;
        }
    }

    if (sp.cellMode)
    {
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            unsigned m = 0;
            for (int b = 0; b < 4; ++b)
            {
                unsigned cell = 0;
                for (int tries = 0; tries <= 5; ++tries)
                {
                    const int i = std::max (0, pickIndex (rp.cellW[(size_t) b], 8, rng.uniform()));
                    cell = kCells[rp.style][b][i].mask;
                    const int on = popcount16 (cell), oddOn = popcount16 (cell & 0xAu);
                    const double prior = kOddPriorOnsets;
                    const double share = (odd.odd + sp.odd16Cap * prior + oddOn) / (odd.total + prior + on);
                    if (oddOn == 0 || share <= cap)
                        break;
                    if (tries == 5)
                        cell &= 0x5u;
                }
                cell = (cell << (4 * b)) & lenMask;
                odd.add (cell);
                m |= cell;
            }
            if (m != 0)
                return m;
        }
        return allowEmpty ? 0u : 1u;
    }

    // Slot mode: k of the bar's steps, drawn without replacement with the bucket weights.
    const double tri = rng.uniform() + rng.uniform() - 1.0;
    const int k = std::clamp ((int) std::lround (rp.target + sp.spread * tri), 0, barLen);
    const auto& w = kSlotWeights[slotBucket (k)];
    unsigned m = 0;

    for (int j = 0; j < k; ++j)
    {
        int chosen = -1;
        for (int tries = 0; tries <= 5 && chosen < 0; ++tries)
        {
            std::array<double, 16> ww {};
            for (int s = 0; s < barLen; ++s)
                ww[(size_t) s] = (m & (1u << s)) != 0 ? 0.0 : w[s] + 1.0e-3;
            const int s = pickIndex (ww, barLen, rng.uniform());
            if (s < 0)
                break;
            const double share = (odd.odd + sp.odd16Cap * kOddPriorOnsets + 1.0) / (odd.total + kOddPriorOnsets + 1.0);
            if ((s & 1) == 0 || share <= cap)
                chosen = s;
            else if (tries == 5)
                chosen = s & ~1;
        }
        if (chosen < 0 || (m & (1u << chosen)) != 0)
            continue;
        m |= 1u << chosen;
        odd.add (1u << chosen);
    }

    return m;
}

std::vector<int> drawNewUnit (Rng& rng, const RhythmParams& rp, int unitLen)
{
    std::vector<int> steps;
    OddState odd;
    for (int barStart = 0; barStart < unitLen; barStart += kBarSteps)
    {
        const int barLen = std::min (kBarSteps, unitLen - barStart);
        const unsigned m = drawBar (rng, rp, barLen, odd);
        for (int s = 0; s < barLen; ++s)
            if ((m & (1u << s)) != 0)
                steps.push_back (barStart + s);
    }
    return steps;
}

// GP 1.5.2 "varied": 1-2 onsets of the unit's last bar added, removed or shifted by a 16th (an 8th for the styles
// with a low odd-16th cap).
void varyRhythm (Rng& rng, std::vector<int>& raw, int unitLen, int style)
{
    const int barStart = ((unitLen - 1) / kBarSteps) * kBarSteps;
    const int changes = rng.uniform() < 0.5 ? 1 : 2;
    const int shift = kStyleParams[style].odd16Cap < 0.1f ? 2 : 1;

    for (int c = 0; c < changes; ++c)
    {
        const double uType = rng.uniform(), uWhich = rng.uniform(), uDir = rng.uniform();
        std::vector<int> inBar, freeSteps;
        for (size_t i = 0; i < raw.size(); ++i)
            if (raw[i] >= barStart)
                inBar.push_back ((int) i);
        for (int s = barStart; s < unitLen; s += 2)
            if (std::find (raw.begin(), raw.end(), s) == raw.end())
                freeSteps.push_back (s);

        if (uType < 0.34 || inBar.size() <= 1)
        {
            if (! freeSteps.empty())
                raw.push_back (freeSteps[(size_t) std::min ((int) freeSteps.size() - 1, (int) (uWhich * (double) freeSteps.size()))]);
        }
        else if (uType < 0.67)
        {
            const int i = inBar[(size_t) std::min ((int) inBar.size() - 1, (int) (uWhich * (double) inBar.size()))];
            if (raw[(size_t) i] != 0)
                raw.erase (raw.begin() + i);
        }
        else
        {
            const int i = inBar[(size_t) std::min ((int) inBar.size() - 1, (int) (uWhich * (double) inBar.size()))];
            const int to = raw[(size_t) i] + (uDir < 0.5 ? -shift : shift);
            if (raw[(size_t) i] != 0 && to >= barStart && to < unitLen
                && std::find (raw.begin(), raw.end(), to) == raw.end())
                raw[(size_t) i] = to;
        }
        std::sort (raw.begin(), raw.end());
    }
}

struct Rhythm
{
    std::vector<Onset> onsets;
    std::vector<UnitRhythm> units;
};

Rhythm buildRhythmOnce (std::uint32_t base, const Plan& plan, const RhythmParams& rp)
{
    const auto& sp = kStyleParams[rp.style];
    const int numUnits = plan.numUnits();
    Rhythm rh;
    rh.units.resize ((size_t) numUnits);

    for (int k = 0; k < numUnits; ++k)
    {
        Rng rng (streamSeed (base, 2, (std::uint32_t) k));
        const double uRelation = rng.uniform(), uCopyA = rng.uniform();
        const int unitLen = plan.unitEnd (k) - plan.unitStart (k);
        auto& ur = rh.units[(size_t) k];
        const int letter = plan.units[(size_t) k].letter;

        for (int j = 0; j < k; ++j)
            if (plan.units[(size_t) j].letter == letter)
            {
                ur.proto = j;
                break;
            }

        if (ur.proto >= 0)
        {
            ur.relation = std::max (0, pickIndex (kRelations[rp.style], kNumRelations, uRelation));
            for (int s : rh.units[(size_t) ur.proto].raw)
                if (s < unitLen)
                    ur.raw.push_back (s);
            if (ur.relation == kRelVaried && plan.units[(size_t) k].cad == 'c')
                varyRhythm (rng, ur.raw, unitLen, rp.style);   // elsewhere "varied" re-picks the pitches only
        }
        else if (letter != 0 && uCopyA < sp.pCopyARhythm)
        {
            for (int s : rh.units[0].raw)
                if (s < unitLen)
                    ur.raw.push_back (s);
        }
        else
            ur.raw = drawNewUnit (rng, rp, unitLen);
    }

    // Gap plan: rests at unit ends, delayed phrase starts, pickups; every unit keeps at least one onset.
    std::vector<std::pair<int, int>> steps;   // (step, flags: 1 pickup)
    for (int k = 0; k < numUnits; ++k)
    {
        const auto& u = plan.units[(size_t) k];
        const auto& prev = plan.units[(size_t) ((k + numUnits - 1) % numUnits)];
        const int start = plan.unitStart (k), end = plan.unitEnd (k);
        const int gapFrom = end - u.gapBeats * kBeatSteps;
        const bool phraseStart = prev.gapBeats > 0;
        const int delay = phraseStart && prev.pickup == 0 ? prev.startStep : 0;

        std::vector<int> own;
        for (int s : rh.units[(size_t) k].raw)
        {
            const int a = start + s;
            if (a < end && a < gapFrom && a >= start + delay)
                own.push_back (a);
        }

        if (phraseStart && std::find (own.begin(), own.end(), start + delay) == own.end())   // a pickup lands on the downbeat
            own.insert (own.begin(), start + delay);
        if (own.empty())
            own.push_back (start + delay);

        std::sort (own.begin(), own.end());
        for (int a : own)
            steps.push_back ({ a, 0 });

        for (int p = u.pickup; p >= 1; --p)
            steps.push_back ({ end - 2 * p, 1 });
    }

    std::sort (steps.begin(), steps.end());
    steps.erase (std::unique (steps.begin(), steps.end(),
                              [] (const auto& a, const auto& b) { return a.first == b.first; }),
                 steps.end());

    for (const auto& [s, flags] : steps)
    {
        Onset o;
        o.step = s;
        o.unit = std::min (numUnits - 1, s / plan.unitSteps);
        o.pickup = (flags & 1) != 0;
        o.pos = posClassOfStep (s % kBarSteps);
        rh.onsets.push_back (o);
    }

    return rh;
}

Rhythm buildRhythm (std::uint32_t base, const Plan& plan, int style, double density)
{
    const auto& sp = kStyleParams[style];
    const double npb = density < 0.5 ? sp.npbLo + (sp.npbDef - sp.npbLo) * 2.0 * density
                                     : sp.npbDef + (sp.npbHi - sp.npbDef) * (2.0 * density - 1.0);
    double target = npb + sp.npbCompensation;

    Rhythm rh = buildRhythmOnce (base, plan, makeRhythmParams (style, target));

    if ((int) rh.onsets.size() > kMaxMelodyNotes)
    {
        target *= (double) kMaxMelodyNotes / (double) rh.onsets.size() * 0.97;
        rh = buildRhythmOnce (base, plan, makeRhythmParams (style, target));
    }

    if ((int) rh.onsets.size() > kMaxMelodyNotes)   // last resort: keep the even 16ths (at most 8 per bar)
    {
        std::vector<Onset> kept;
        for (const auto& o : rh.onsets)
            if ((o.step & 1) == 0)
                kept.push_back (o);
        rh.onsets = kept;
        if ((int) rh.onsets.size() > kMaxMelodyNotes)
            rh.onsets.resize ((size_t) kMaxMelodyNotes);
    }

    return rh;
}

//==============================================================================
// Pitch (stream 3) and articulation (stream 4).
struct Line
{
    int prev = -1;          // previous pitch of the line (-1 = none yet)
    int prevInterval = 0;   // the interval that led into prev
    int pendType = -1;      // NCT out-constraint on the line's next note
    int pendOrigin = 0;
    int pendDir = 0;
};

struct NoteDraws
{
    double rep, ct, nct, pick, aux;
};

struct Cand
{
    int p;
    double w;
};

class Generator
{
public:
    Generator (const Progression& p, const Key& k, const MelodyGenContext& c, std::uint32_t seed, const MelodyGenSettings& s)
        : prog (p), key (k), ctx (c), set (clampSettings (s)), style (styleIndex (set.style)), sp (kStyleParams[style])
    {
        base = baseHash (prog, key, seed, style);
        tonic = pc (key.tonic);
        minorKey = scaleInfo (key.scale).isMinorFamily;
        totalSteps = totalStepsOf (prog);
        window = computeWindow (prog, ctx, set);

        int start = 0;
        static const std::vector<int> noVoicing;
        for (size_t i = 0; i < prog.slots.size(); ++i)
        {
            const auto& v = i < ctx.voicings.size() ? ctx.voicings[i] : noVoicing;
            slots.push_back (makeSlotInfo (prog.slots[i], v, key, start));
            start = slots.back().endStep;
        }

        slotOfStep.assign ((size_t) std::max (0, totalSteps), 0);
        for (int i = 0; i < (int) slots.size(); ++i)
            for (int st = slots[(size_t) i].startStep; st < std::min (totalSteps, slots[(size_t) i].endStep); ++st)
                slotOfStep[(size_t) st] = i;
    }

    Melody run()
    {
        Melody out;
        if (totalSteps <= 0 || slots.empty())
            return out;

        plan = makePlan (base, style, set.repetition, totalSteps);
        maxLeap = style == kDriving && ! plan.pedal ? kDrivingLineMaxLeap : sp.maxLeap;
        rhythm = buildRhythm (base, plan, style, set.density);
        markOnsets();
        pickPitches();
        articulate (out);
        return sanitiseMelody (out);
    }

private:
    const Progression& prog;
    const Key& key;
    const MelodyGenContext& ctx;
    const MelodyGenSettings set;
    const int style;
    const StyleParams& sp;
    std::uint32_t base = 0;
    int tonic = 0;
    bool minorKey = false;
    int totalSteps = 0;
    Window window;
    std::vector<SlotInfo> slots;
    std::vector<int> slotOfStep;
    Plan plan;
    Rhythm rhythm;
    std::vector<int> pitch;
    int maxLeap = 12;

    std::vector<Onset>& on() { return rhythm.onsets; }
    const SlotInfo& slotAt (int i) const { return slots[(size_t) rhythm.onsets[(size_t) i].slot]; }

    //==========================================================================
    void markOnsets()
    {
        auto& o = on();
        const int n = (int) o.size();

        for (auto& x : o)
            x.slot = slotOfStep[(size_t) std::clamp (x.step, 0, totalSteps - 1)];

        // Cadence notes: the last own onset of each unit with a cadence tag; the last own onset overall is the loop end.
        for (int k = 0; k < plan.numUnits(); ++k)
        {
            int last = -1;
            for (int i = 0; i < n; ++i)
                if (o[(size_t) i].unit == k && ! o[(size_t) i].pickup)
                    last = i;
            if (last < 0)
                continue;
            const auto& u = plan.units[(size_t) k];
            if (u.gapBeats > 0)
                o[(size_t) last].phraseFinal = true;
            if (u.cad != '-')
            {
                o[(size_t) last].cadence = true;
                o[(size_t) last].closed = u.cad == 'c';
            }
        }

        for (int i = n - 1; i >= 0; --i)
            if (! o[(size_t) i].pickup)
            {
                o[(size_t) i].cadence = o[(size_t) i].closed = o[(size_t) i].loopEnd = true;
                break;
            }

        // Contour segments: from one cadence note to the next.
        int seg = 0, segFirst = 0;
        for (int i = 0; i < n; ++i)
        {
            o[(size_t) i].segment = seg;
            if (o[(size_t) i].cadence || i == n - 1)
            {
                const int a = o[(size_t) segFirst].step, b = o[(size_t) i].step;
                for (int j = segFirst; j <= i; ++j)
                {
                    o[(size_t) j].segX = b > a ? (double) (o[(size_t) j].step - a) / (double) (b - a) : 0.0;
                    o[(size_t) j].segStart = a;
                    o[(size_t) j].segEnd = b;
                }
                ++seg;
                segFirst = i + 1;
            }
        }
    }

    //==========================================================================
    // Weights
    int degreePos (const SlotInfo& s, int p) const
    {
        const int rel = p - tonic;
        const int oct = floorDiv (rel, 12);
        return oct * 7 + s.posOfRel[(size_t) (rel - 12 * oct)];
    }

    int pitchAtPos (const SlotInfo& s, int pos) const
    {
        const int oct = floorDiv (pos, 7);
        return tonic + 12 * oct + s.offset[(size_t) (pos - 7 * oct)];
    }

    int scaleSteps (const SlotInfo& s, int from, int to) const
    {
        int d = degreePos (s, to) - degreePos (s, from);
        if (d == 0 && to != from)
            d = to > from ? 1 : -1;
        return d;
    }

    double wStep (const SlotInfo& s, int from, int to) const
    {
        if (from < 0)
            return 1.0;
        const int semis = to - from;
        if (semis == 0)
            return 1.0;
        if (std::abs (semis) > maxLeap)
            return 0.0;
        const int steps = scaleSteps (s, from, to);
        const int idx = std::min (std::abs (steps), 8) - 1;
        double w = steps > 0 ? sp.stepUp[idx] : sp.stepDown[idx];
        if (std::abs (steps) == 1 && std::abs (semis) == 3)
            w *= kAugSecondFactor;
        if (std::abs (semis) <= 2)
            w *= sp.stepBias;
        else if (std::abs (semis) <= 4)
            w *= sp.skipFactor;
        return w;
    }

    double wReg (int p) const
    {
        const double sigma = std::max (1.0, (window.hi - window.lo) / 3.0);
        const double z = (p - window.centre) / sigma;
        double w = std::exp (-0.5 * z * z);
        if (window.arpFloor >= 0 && p < window.arpFloor)
            w *= kBelowArpFactor;
        return w;
    }

    static double wContour (int p, double target) { return std::exp (-std::abs (p - target) / kContourTau); }

    double wContourAt (int i, int p, double target) const
    {
        const double tau = protoTarget[(size_t) i] >= 0 ? kRepickTau : kContourTau;
        return std::exp (-std::abs (p - target) / tau);
    }

    double wLeap (const Line& line, int p) const
    {
        if (line.prev < 0 || std::abs (line.prevInterval) < 5)
            return 1.0;
        const int d = p - line.prev;
        const bool large = std::abs (line.prevInterval) >= 8;
        double w = 1.0;
        if (sgn (d) == -sgn (line.prevInterval))
        {
            w *= large ? sp.kRevLarge : sp.kRevSmall;
            if (std::abs (d) <= 2)
                w *= sp.gapFill;
        }
        else if (std::abs (d) >= 5)
            w *= large ? sp.sameLarge : sp.sameSmall;
        return w;
    }

    double wKindChord (const SlotInfo& s, int p, int pos) const
    {
        if (pos != kDown && pos != kBeat3)
            return 1.0;
        const int k = s.kind[(size_t) pc (p)];
        return k < 0 ? 1.0 : k == 4 ? kWkindExtension : kWkind[s.func][k];
    }

    double wKindNct (const SlotInfo& s, int p) const
    {
        switch (pc (p - s.chord.root))
        {
            case 1: case 2:   return kWnct9;
            case 5: case 6:   return kWnct11;
            case 8: case 9:   return kWnct13;
            case 10: case 11: return kWnct7;
            default:          return kWnctOther;
        }
    }

    // R2: a non-chord tone a semitone above a chord tone on down / beat3.
    double wAvoid (const SlotInfo& s, int p, int pos, bool resolvesDown) const
    {
        if ((pos != kDown && pos != kBeat3) || resolvesDown)
            return 1.0;
        return s.chordTone[(size_t) pc (p - 1)] ? kAvoidFactor : 1.0;
    }

    double firstRow (const SlotInfo& s, int p) const
    {
        const int d = s.degree[(size_t) pc (p)];
        return minorKey ? kFirstMinor[d] : kFirstMajor[d];
    }

    //==========================================================================
    // Contour
    std::vector<int> segShape;   // per segment
    std::vector<int> protoTarget;  // per onset: re-pick target pitch (-1 = the contour path)

    double contourTarget (int i) const
    {
        const auto& o = rhythm.onsets[(size_t) i];
        if (protoTarget[(size_t) i] >= 0)
            return protoTarget[(size_t) i];

        const double bars = std::max (1.0, (o.segEnd - o.segStart) / (double) kBarSteps);
        const double span = std::min ((double) (window.hi - window.lo - 2),
                                      bars <= 4.0 ? sp.phraseSpan
                                                  : bars >= 8.0 ? sp.phraseSpan8
                                                                : sp.phraseSpan + (sp.phraseSpan8 - sp.phraseSpan) * (bars - 4.0) / 4.0);
        const double c = window.centre, x = o.segX, h = 0.5 * span;

        switch (segShape[(size_t) o.segment])
        {
            case kShapeRising:  return c - h + span * x;
            case kShapeFalling: return c + h - span * x;
            case kShapeArch:    return x <= kArchPeak ? c - h + span * (x / kArchPeak) : c + h - span * ((x - kArchPeak) / (1.0 - kArchPeak));
            case kShapeValley:  return x <= 0.5 ? c + h - span * (x / 0.5) : c - h + span * ((x - 0.5) / 0.5);
            case kShapeWave:
            default:
            {
                const double cycles = (o.step - o.segStart) / (double) plan.unitSteps;
                return c + h * std::cos (2.0 * 3.14159265358979323846 * cycles);
            }
        }
    }

    //==========================================================================
    // Picks
    int choose (const std::vector<Cand>& c, double u) const
    {
        std::vector<double> w;
        w.reserve (c.size());
        for (const auto& x : c)
            w.push_back (x.w);
        const int i = pickIndex (w, (int) w.size(), u);
        return i < 0 ? -1 : c[(size_t) i].p;
    }

    int intoWindow (int p, int lo, int hi) const
    {
        while (p > hi)
            p -= 12;
        while (p < lo)
            p += 12;
        return std::clamp (p, lo, hi);
    }

    void accept (Line& line, int p) const
    {
        line.prevInterval = line.prev >= 0 ? p - line.prev : 0;
        line.prev = p;
    }

    bool nextIsResolvable (int i) const
    {
        const auto& o = rhythm.onsets;
        if (i + 1 >= (int) o.size())
            return false;
        const auto& a = o[(size_t) i];
        const auto& b = o[(size_t) i + 1];
        return copyPitch[(size_t) i + 1] < 0 && b.unit == a.unit && ! b.cadence && ! b.pickup && ! a.pickup
            && (b.step - a.step) <= (int) (kNctMaxIoiBeats[style] * kBeatSteps);
    }

    // Chord-tone pick (also the fallback of every construction).
    int pickChordTone (int i, const Line& line, int lo, int hi, double u, bool first) const
    {
        const auto& s = slotAt (i);
        const int pos = rhythm.onsets[(size_t) i].pos;
        const double target = contourTarget (i);
        std::vector<Cand> c;
        for (int p = lo; p <= hi; ++p)
        {
            if (! s.chordTone[(size_t) pc (p)] || p == line.prev)
                continue;
            double w = wStep (s, line.prev, p) * wReg (p) * wContourAt (i, p, target) * wKindChord (s, p, pos) * wLeap (line, p);
            if (first)
                w *= firstRow (s, p);
            if (w > 0.0)
                c.push_back ({ p, w });
        }
        int p = choose (c, u);
        if (p < 0)
            p = line.prev >= 0 ? intoWindow (line.prev, lo, hi) : intoWindow ((int) std::lround (target), lo, hi);
        return p;
    }

    // A non-chord scale tone without a construction (the Driving pedal mode's upper line).
    int pickScaleTone (int i, const Line& line, int lo, int hi, double u) const
    {
        const auto& s = slotAt (i);
        const int pos = rhythm.onsets[(size_t) i].pos;
        const double target = contourTarget (i);
        std::vector<Cand> c;
        for (int p = lo; p <= hi; ++p)
        {
            const int ppc = pc (p);
            if (! s.inScale[(size_t) ppc] || s.chordTone[(size_t) ppc] || p == line.prev)
                continue;
            const double w = wStep (s, line.prev, p) * wReg (p) * wContour (p, target) * wKindNct (s, p)
                           * wLeap (line, p) * wAvoid (s, p, pos, false);
            if (w > 0.0)
                c.push_back ({ p, w });
        }
        return choose (c, u);
    }

    // Resolves a pending NCT out-constraint; -1 when it cannot be met.
    int resolvePending (int i, Line& line, int lo, int hi, const NoteDraws& d) const
    {
        const auto& s = slotAt (i);
        const double target = contourTarget (i);
        const int prev = line.prev;
        const int type = line.pendType;

        if (type == kNctHeld || (type == kNctAnticipation && d.rep < kAnticipationRepeat))
            return s.allowed[(size_t) pc (prev)] ? prev : -1;
        if (type == kNctNeighbour)
            return s.allowed[(size_t) pc (line.pendOrigin)] && line.pendOrigin >= lo && line.pendOrigin <= hi ? line.pendOrigin : -1;

        std::vector<Cand> c;
        for (int p = lo; p <= hi; ++p)
        {
            if (! s.chordTone[(size_t) pc (p)])
                continue;
            const int dd = p - prev, ad = std::abs (dd);
            bool ok = false;
            double w = wReg (p) * wContour (p, target);
            switch (type)
            {
                case kNctPassing:      ok = ad >= 1 && ad <= 2 && sgn (dd) == line.pendDir; break;
                case kNctAppoggiatura: ok = ad >= 1 && ad <= 2;
                                       w *= sgn (dd) == -line.pendDir ? kAppoggiaturaOpposite : 1.0 - kAppoggiaturaOpposite;
                                       break;
                case kNctEscape:
                case kNctFree:         ok = ad >= 3 && ad <= maxLeap; w *= wStep (s, prev, p); break;
                case kNctAnticipation: ok = ad >= 1 && ad <= 2; break;
                default:               break;
            }
            if (ok && w > 0.0)
                c.push_back ({ p, w });
        }
        return choose (c, d.pick);
    }

    // Tries a non-chord-tone construction (GP 1.4.7); -1 when none is feasible.
    int pickNct (int i, Line& line, int lo, int hi, const NoteDraws& d) const
    {
        if (! nextIsResolvable (i))
            return -1;

        const auto& o = rhythm.onsets[(size_t) i];
        const auto& s = slotAt (i);
        const auto& ns = slotAt (i + 1);
        const int prev = line.prev;
        const double target = contourTarget (i);
        const bool sameSlot = rhythm.onsets[(size_t) i + 1].slot == o.slot;
        const bool anticipationSpot = (o.pos == kOff8 || o.pos == kA16) && ! sameSlot && s.endStep - o.step <= kBeatSteps;

        auto hasCt = [&] (int p, int minD, int maxD, int dir) {
            for (int q = std::max (lo, p - maxD); q <= std::min (hi, p + maxD); ++q)
            {
                const int dq = q - p;
                if (std::abs (dq) >= minD && ns.chordTone[(size_t) pc (q)] && (dir == 0 || sgn (dq) == dir))
                    return true;
            }
            return false;
        };

        std::array<std::vector<Cand>, kNumNct> cands;
        for (int p = lo; p <= hi; ++p)
        {
            const int ppc = pc (p);
            if (! s.inScale[(size_t) ppc] || s.chordTone[(size_t) ppc] || p == prev)
                continue;
            const int dd = prev >= 0 ? p - prev : 0, ad = std::abs (dd);
            if (prev >= 0 && ad > maxLeap)
                continue;

            const double w = wStep (s, prev, p) * wReg (p) * wContour (p, target) * wKindNct (s, p) * wLeap (line, p);
            if (w <= 0.0)
                continue;
            const double avoid = wAvoid (s, p, o.pos, false);

            if (prev >= 0 && ad >= 1 && ad <= 2 && hasCt (p, 1, 2, sgn (dd)))
                cands[kNctPassing].push_back ({ p, w * avoid });
            if (prev >= 0 && ad >= 1 && ad <= 2 && s.chordTone[(size_t) pc (prev)] && ns.allowed[(size_t) pc (prev)])
                cands[kNctNeighbour].push_back ({ p, w * avoid });
            if (prev >= 0 && ad >= 3 && hasCt (p, 1, 2, 0))
                cands[kNctAppoggiatura].push_back ({ p, w * wAvoid (s, p, o.pos, ns.chordTone[(size_t) pc (p - 1)]) });
            if (prev >= 0 && ad >= 1 && ad <= 2 && hasCt (p, 3, maxLeap, 0))
                cands[kNctEscape].push_back ({ p, w * avoid });
            if (anticipationSpot && ns.chordTone[(size_t) ppc])
                cands[kNctAnticipation].push_back ({ p, w });
            if (sameSlot)
                cands[kNctHeld].push_back ({ p, w * avoid });
            if (prev >= 0 && ad >= 3 && hasCt (p, 3, maxLeap, 0))
                cands[kNctFree].push_back ({ p, w * avoid });
        }

        std::array<double, kNumNct> tw {};
        for (int t = 0; t < kNumNct; ++t)
            tw[(size_t) t] = cands[(size_t) t].empty() ? 0.0
                                                       : kNctWeights[style][t] * (t == kNctAppoggiatura && o.pos == kDown ? kAppoggiaturaDownFactor : 1.0);
        const int type = pickIndex (tw, kNumNct, d.nct);
        if (type < 0)
            return -1;

        const int p = choose (cands[(size_t) type], d.pick);
        if (p < 0)
            return -1;

        line.pendType = type;
        line.pendOrigin = prev;
        line.pendDir = sgn (p - prev);
        return p;
    }

    // One free note of a line (GP 1.4.1-1.4.7).
    int pickFree (int i, Line& line, int lo, int hi, const NoteDraws& d, bool allowNct)
    {
        const auto& o = rhythm.onsets[(size_t) i];
        const auto& s = slotAt (i);
        const bool first = line.prev < 0 && i == 0;
        bool forceCt = false;

        if (line.pendType >= 0)
        {
            const int p = resolvePending (i, line, lo, hi, d);
            line.pendType = -1;
            if (p >= 0)
                return p;
            forceCt = true;
        }

        if (! forceCt && line.prev >= 0 && d.rep < sp.pRepeat)
        {
            const int ppc = pc (line.prev);
            const bool sameChord = i > 0 && rhythm.onsets[(size_t) i - 1].slot == o.slot;
            if (line.prev >= lo && line.prev <= hi
                && (s.chordTone[(size_t) ppc] || (s.inScale[(size_t) ppc] && sameChord && o.pos != kDown)))
                return line.prev;
        }

        const bool strongPos = o.pos == kDown || o.pos == kBeat3;
        const double pCt = first ? 1.0
                                 : sp.pCT[o.pos] + (s.lengthBeats <= 2 ? kShortSlotCtBonus : 0.0f)
                                       - (strongPos ? sp.strongOffset : sp.nctBoost);
        if (! forceCt && ! allowNct && d.ct >= pCt)
        {
            const int p = pickScaleTone (i, line, lo, hi, d.pick);   // pedal mode: an unconstrained non-chord tone
            if (p >= 0)
                return p;
        }
        if (! forceCt && allowNct && d.ct >= pCt)
        {
            const int p = pickNct (i, line, lo, hi, d);
            if (p >= 0)
                return p;
        }


        return pickChordTone (i, line, lo, hi, d.pick, first);
    }

    // Cadence notes (GP 1.5.5, R3, resolveEnding, R7).
    int pickCadence (int i, const Line& line, double u, int firstPitch) const
    {
        const auto& o = rhythm.onsets[(size_t) i];
        const auto& s = slotAt (i);
        const int lo = window.lo, hi = window.hi;
        const bool resolve = o.loopEnd && set.resolveEnding;
        const bool seam = i == (int) rhythm.onsets.size() - 1 && firstPitch >= 0;
        std::vector<Cand> c;

        for (int pass = 0; pass < 2 && c.empty(); ++pass)
            for (int p = lo; p <= hi; ++p)
            {
                const int ppc = pc (p);
                if (! s.allowed[(size_t) ppc])
                    continue;
                if (line.prev >= 0 && std::abs (p - line.prev) > maxLeap)
                    continue;
                if (seam && pass == 0 && std::abs (p - firstPitch) > kMaxSeam)
                    continue;

                const int deg = s.degree[(size_t) ppc];
                double w = wReg (p) * wStep (s, line.prev, p);

                if (resolve)
                    w *= (minorKey ? kFinalMinor[deg] : kFinalMajor[deg]) * (deg == 0 ? kResolveTonic : 1.0f)
                       * (s.chordTone[(size_t) ppc] ? kResolveChordTone : 1.0f);
                else
                    w *= kCadence[style][s.cadClass[(size_t) ppc]] * (s.chordTone[(size_t) ppc] ? kCadenceChordTone : 1.0f);

                if (line.prev >= 0)
                {
                    if (! s.inScale[(size_t) ppc] && std::abs (p - line.prev) == 1)
                        w *= kApproachChromatic[style];
                    else
                    {
                        const int st = p == line.prev ? 0 : scaleSteps (s, line.prev, p);
                        w *= std::abs (st) <= 4 ? kApproach[style][st + 4] : kApproachFloor;
                    }
                }

                w *= o.closed ? kClosedFactor[deg] : kOpenFactor[deg];
                if (seam)
                    w *= std::max (0.05, wStep (s, p, firstPitch));

                if (w > 0.0)
                    c.push_back ({ p, w });
            }

        const int p = choose (c, u);
        return p >= 0 ? p : pickChordTone (i, line, lo, hi, u, false);
    }

    //==========================================================================
    std::vector<int> copyPitch;   // per onset: the pitch a repeated unit copies (-1 = picked fresh)

    void pickPitches()
    {
        auto& o = on();
        const int n = (int) o.size();
        pitch.assign ((size_t) n, 0);
        copyPitch.assign ((size_t) n, -1);
        protoTarget.assign ((size_t) n, -1);

        Rng rng (streamSeed (base, 3));
        const int numUnits = plan.numUnits();

        std::vector<double> uTreat ((size_t) numUnits);
        for (auto& u : uTreat)
            u = rng.uniform();

        const int numSegments = n > 0 ? o.back().segment + 1 : 0;
        segShape.assign ((size_t) numSegments, kShapeWave);
        for (int g = 0; g < numSegments; ++g)
        {
            const double u = rng.uniform();
            switch (set.contour)
            {
                case MelodyContour::Rising:  segShape[(size_t) g] = kShapeRising; break;
                case MelodyContour::Falling: segShape[(size_t) g] = kShapeFalling; break;
                case MelodyContour::Arch:    segShape[(size_t) g] = kShapeArch; break;
                case MelodyContour::Wave:    segShape[(size_t) g] = kShapeWave; break;
                case MelodyContour::Auto:
                case MelodyContour::NumContours:
                default:                     segShape[(size_t) g] = std::max (0, pickIndex (kContourWeights[style], kNumShapes, u)); break;
            }
        }

        std::vector<NoteDraws> draws ((size_t) n);
        for (auto& d : draws)
            d = { rng.uniform(), rng.uniform(), rng.uniform(), rng.uniform(), rng.uniform() };

        // Own onsets per unit and the prototype correspondence.
        std::vector<std::vector<int>> own ((size_t) numUnits);
        for (int i = 0; i < n; ++i)
            if (! o[(size_t) i].pickup)
                own[(size_t) o[(size_t) i].unit].push_back (i);

        std::vector<int> protoOf ((size_t) n, -1);
        std::vector<char> unitResolved ((size_t) numUnits, 0);

        Line upper, pedalLine;
        int firstPitch = -1;

        for (int i = 0; i < n; ++i)
        {
            const auto& oi = o[(size_t) i];
            const int k = oi.unit;

            if (! oi.pickup && ! unitResolved[(size_t) k])
            {
                unitResolved[(size_t) k] = 1;
                resolveUnit (k, own, protoOf, uTreat[(size_t) k]);
            }

            const auto& d = draws[(size_t) i];
            const bool isLast = i == n - 1;

            // Driving pedal mode: the 2nd, 4th, ... onset of each bar plays the pedal.
            bool pedalPos = false;
            if (plan.pedal && ! oi.pickup)
            {
                int j = 0;
                for (int b = i - 1; b >= 0 && o[(size_t) b].step / kBarSteps == oi.step / kBarSteps; --b)
                    ++j;
                pedalPos = (j % 2) == 1 && ! oi.loopEnd;
            }

            int p = -1;
            const bool copied = copyPitch[(size_t) i] >= 0 && ! pedalPos;   // the pedal always follows the chord

            if (copied)
            {
                p = copyPitch[(size_t) i];
                const int pi = protoOf[(size_t) i];
                const bool keepCadence = pi >= 0 && o[(size_t) pi].cadence == oi.cadence && o[(size_t) pi].closed == oi.closed
                                      && o[(size_t) pi].slot >= 0 && slots[(size_t) o[(size_t) pi].slot].chord == slotAt (i).chord;
                if ((oi.cadence && ! keepCadence) || oi.loopEnd || isLast)
                    p = -1;
            }

            if (p < 0 && pedalPos)
                p = pedalPitch (i, d.aux);

            if (p < 0)
            {
                if (oi.cadence || (isLast && firstPitch >= 0))
                    p = pickCadenceOrSeam (i, upper, d, firstPitch);
                else if (plan.pedal)
                    p = pickFree (i, upper, std::min (window.hi - 7, pedalFloor (i) + kPedalUpperGap), window.hi, d, false);
                else
                    p = pickFree (i, upper, window.lo, window.hi, d, true);
            }

            p = intoWindow (p, window.lo, window.hi);
            pitch[(size_t) i] = p;
            if (pedalPos)
                accept (pedalLine, p);
            else
                accept (upper, p);
            if (i == 0)
                firstPitch = p;
        }

        // R7: the loop seam never jumps more than an octave.
        if (n > 1)
        {
            int& last = pitch[(size_t) n - 1];
            while (last - firstPitch > kMaxSeam && last - 12 >= window.lo)
                last -= 12;
            while (firstPitch - last > kMaxSeam && last + 12 <= window.hi)
                last += 12;
        }
    }

    int pickCadenceOrSeam (int i, Line& line, const NoteDraws& d, int firstPitch)
    {
        line.pendType = -1;
        const auto& oi = rhythm.onsets[(size_t) i];
        if (oi.cadence)
            return pickCadence (i, line, d.pick, firstPitch);

        // The last note in time is a pickup into the loop start: a free pick weighted towards the first note (R7).
        const auto& s = slotAt (i);
        const double target = contourTarget (i);
        std::vector<Cand> c;
        for (int pass = 0; pass < 2 && c.empty(); ++pass)
            for (int p = window.lo; p <= window.hi; ++p)
            {
                if (! s.allowed[(size_t) pc (p)] || (pass == 0 && std::abs (p - firstPitch) > kMaxSeam))
                    continue;
                const double w = wStep (s, line.prev, p) * wReg (p) * wContour (p, target)
                               * std::max (0.05, wStep (s, p, firstPitch)) * (s.chordTone[(size_t) pc (p)] ? 1.5 : 1.0);
                if (w > 0.0)
                    c.push_back ({ p, w });
            }
        const int p = choose (c, d.pick);
        return p >= 0 ? p : pickChordTone (i, line, window.lo, window.hi, d.pick, false);
    }

    int pedalFloor (int i) const
    {
        const int root = slotAt (i).chord.root;
        int p = window.lo;
        while (pc (p) != root)
            ++p;
        return p;
    }

    int pedalPitch (int i, double u) const
    {
        const auto& s = slotAt (i);
        int target = s.chord.root;
        if (u < kPedalFifth)
            for (int iv : s.chord.intervals())
                if (iv >= 6 && iv <= 8)
                    target = pc (s.chord.root + iv);
        int p = window.lo;
        while (pc (p) != target)
            ++p;
        return p;
    }

    // Decides how a unit's pitches come about (GP 1.5.2 / 1.5.4) and fills copyPitch for its copied onsets.
    void resolveUnit (int k, const std::vector<std::vector<int>>& own, std::vector<int>& protoOf, double uTreat)
    {
        const auto& ur = rhythm.units[(size_t) k];
        const auto& o = rhythm.onsets;
        if (ur.proto < 0 || ur.proto == k)
            return;

        const int proto = ur.proto;
        const int shiftSteps = plan.unitStart (k) - plan.unitStart (proto);
        const auto& mine = own[(size_t) k];
        const auto& theirs = own[(size_t) proto];

        // Correspondence by the offset into the unit; an unmatched onset re-picks towards the nearest earlier one.
        bool changed = false;
        std::vector<int> match (mine.size(), -1);
        for (size_t a = 0; a < mine.size(); ++a)
        {
            const int rel = o[(size_t) mine[a]].step - shiftSteps;
            int nearest = theirs.empty() ? -1 : theirs.front();
            for (int t : theirs)
            {
                if (o[(size_t) t].step == rel)
                    match[a] = t;
                if (o[(size_t) t].step <= rel)
                    nearest = t;
            }
            protoOf[(size_t) mine[a]] = match[a];
            if (match[a] < 0 || ! (slots[(size_t) o[(size_t) match[a]].slot].chord == slots[(size_t) o[(size_t) mine[a]].slot].chord))
                changed = true;
            if (nearest >= 0)
                protoTarget[(size_t) mine[a]] = pitch[(size_t) nearest];
        }

        auto clearTargets = [&] {
            for (int i : mine)
                protoTarget[(size_t) i] = -1;
        };

        if (ur.relation == kRelVaried)
            return;   // re-pick along the prototype's pitches (protoTarget)

        int t = std::max (0, pickIndex (kTreatments[style][changed ? 0 : 1], kNumTreatments, uTreat));

        auto adapted = [&] (int src, int dst, int shift) {
            const auto& from = slots[(size_t) o[(size_t) src].slot];
            const auto& to = slots[(size_t) o[(size_t) dst].slot];
            const int p = pitch[(size_t) src];
            if (shift == 0 && to.allowed[(size_t) pc (p)])
                return p;
            return pitchAtPos (to, degreePos (from, p) + shift);
        };

        if (t == kTreatExact && changed)
        {
            int strong = 0, ct = 0;
            for (size_t a = 0; a < mine.size(); ++a)
            {
                const auto& x = o[(size_t) mine[a]];
                if (match[a] < 0 || (x.pos != kDown && x.pos != kBeat3))
                    continue;
                ++strong;
                if (slots[(size_t) x.slot].chordTone[(size_t) pc (adapted (match[a], mine[a], 0))])
                    ++ct;
            }
            if (strong > 0 && ct < kExactAcceptChordToneShare * strong)
                t = kTreatFit;
        }

        int shift = 0;
        if (t == kTreatFit)
        {
            double bestScore = -1.0;
            int best = -100;
            static constexpr int order[] = { 0, -1, 1, -2, 2, -3, 3 };
            for (int sh : order)
            {
                if (std::abs (sh) > kFitShiftMax || (sh == 0 && changed))
                    continue;
                double score = 0.0;
                bool inside = true;
                for (size_t a = 0; a < mine.size() && inside; ++a)
                {
                    if (match[a] < 0)
                        continue;
                    const int q = adapted (match[a], mine[a], sh);
                    inside = q >= window.lo && q <= window.hi;
                    const auto& x = o[(size_t) mine[a]];
                    if (slots[(size_t) x.slot].chordTone[(size_t) pc (q)])
                        score += (x.pos == kDown || x.pos == kBeat3) ? 1.0 : x.pos == kOff8 ? 0.5 : 0.0;
                }
                if (inside && score > bestScore)
                {
                    bestScore = score;
                    best = sh;
                }
            }
            if (best == -100)
                return;   // nothing fits the window: re-pick
            shift = best;
        }

        if (t == kTreatRepick)
            return;

        const size_t keep = t == kTreatEnding ? mine.size() / 2 : mine.size();
        clearTargets();
        for (size_t a = 0; a < keep; ++a)
            if (match[a] >= 0)
                copyPitch[(size_t) mine[a]] = intoWindow (adapted (match[a], mine[a], shift), window.lo, window.hi);
    }

    //==========================================================================
    // Articulation (GP 1.5.7) and R6.
    double drawLength (const LengthWeight* table, int size, double u) const
    {
        std::vector<double> w;
        for (int i = 0; i < size; ++i)
            w.push_back (table[i].weight);
        return table[std::max (0, pickIndex (w, size, u))].beats;
    }

    bool clashesWith (const SlotInfo& s, int p) const
    {
        const int ppc = pc (p);
        if (s.chordTone[(size_t) ppc])
            return false;
        return s.chordTone[(size_t) pc (p + 1)] || s.chordTone[(size_t) pc (p - 1)] || ppc == pc (s.bassPc + 1);
    }

    void articulate (Melody& out)
    {
        const auto& o = rhythm.onsets;
        const int n = (int) o.size();
        Rng rng (streamSeed (base, 4));
        constexpr double tick = kMelodyTick;

        for (int i = 0; i < n; ++i)
        {
            const double uGate = rng.uniform(), uLen = rng.uniform(), uHold = rng.uniform();
            const int step = o[(size_t) i].step;
            const int nextStep = i + 1 < n ? o[(size_t) i + 1].step : totalSteps;
            const double ioi = (nextStep - step) / (double) kBeatSteps;
            double len = ioi * kGateValue[std::max (0, pickIndex (sp.gateW, 4, uGate))];

            if (sp.allSustain)
                len = ioi;

            const int pos = o[(size_t) i].pos;
            const int beatEnd = (step / kBeatSteps + 2) * kBeatSteps;
            if (sp.pLongHold > 0.0f && (pos == kDown || pos == kBeat3) && nextStep >= beatEnd && uHold < sp.pLongHold)
                len = ioi;

            if (o[(size_t) i].phraseFinal)
            {
                if (style == kSparse)
                    len = std::min (kSparseFinalMax, ioi);
                else if (style == kLyrical)
                    len = drawLength (kFinalLenLyrical, (int) (sizeof (kFinalLenLyrical) / sizeof (LengthWeight)), uLen);
                else
                    len = drawLength (kFinalLenUplifting, (int) (sizeof (kFinalLenUplifting) / sizeof (LengthWeight)), uLen);
                if (i + 1 < n)
                    len = std::min (len, std::max (0.25, ioi - kMinRestAfterPhrase));
            }

            // A non-chord tone inside a phrase leads into the next note instead of hanging before a rest.
            if (! o[(size_t) i].phraseFinal && i + 1 < n && ! slotAt (i).chordTone[(size_t) pc (pitch[(size_t) i])])
                len = std::max (len, ioi - kNctMaxRest);

            // A phrase of a single note is held (a lone 16th before a rest sounds like a mistake).
            if (o[(size_t) i].phraseFinal && (i == 0 || o[(size_t) i - 1].phraseFinal))
                len = std::max (len, std::min (kLonePhraseHold, ioi - (i + 1 < n ? kMinRestAfterPhrase : 0.0)));

            len = std::round (len / kLengthGrid) * kLengthGrid;
            len = std::clamp (std::round (len / tick) * tick, std::min (kLengthGrid, ioi), ioi);

            // R6: cut a note held into a clashing chord at the boundary (Lyrical / Sparse keep a step-down suspension).
            const double startBeat = step / (double) kBeatSteps;
            const int p = pitch[(size_t) i];
            for (int sl = o[(size_t) i].slot + 1; sl < (int) slots.size(); ++sl)
            {
                const double boundary = slots[(size_t) sl].startStep / (double) kBeatSteps;
                if (boundary >= startBeat + len - 1.0e-9)
                    break;
                if (! clashesWith (slots[(size_t) sl], p))
                    continue;

                bool suspension = false;
                if ((style == kLyrical || style == kSparse) && i + 1 < n)
                {
                    const double nb = nextStep / (double) kBeatSteps;
                    const int q = pitch[(size_t) i + 1];
                    const int nslot = o[(size_t) i + 1].slot;
                    suspension = nb <= boundary + 1.0 && p - q >= 1 && p - q <= 2
                              && slots[(size_t) nslot].chordTone[(size_t) pc (q)];
                }
                if (! suspension)
                    len = boundary - startBeat;
                break;
            }

            MelodyNote note;
            note.note = p;
            note.startBeat = startBeat;
            note.lengthBeats = len;
            note.velocity = kDefaultMelodyVelocity;
            out.notes.push_back (note);
        }
    }
};

//==============================================================================
// Analysis helpers.
int stepOf (double beat)
{
    const double s = beat * kBeatSteps;
    const double r = std::round (s);
    return std::abs (s - r) < 1.0e-6 ? (int) r : -1;
}

} // namespace

//==============================================================================
Melody generateMelody (const Progression& progression, const Key& key, const MelodyGenContext& context,
                       std::uint32_t seed, const MelodyGenSettings& settings)
{
    if (progression.empty() || progression.totalBeats() <= 0)
        return {};

    Generator gen (progression, key, context, seed, settings);
    return gen.run();
}

std::pair<int, int> melodyRegisterWindow (const Progression& progression, const MelodyGenContext& context,
                                          const MelodyGenSettings& settings)
{
    const auto w = computeWindow (progression, context, clampSettings (settings));
    return { w.lo, w.hi };
}

std::string melodyPlanSignature (const Progression& progression, const Key& key, std::uint32_t seed,
                                 const MelodyGenSettings& settings)
{
    if (progression.empty() || progression.totalBeats() <= 0)
        return {};

    const auto s = clampSettings (settings);
    const int style = styleIndex (s.style);
    return planSignature (makePlan (baseHash (progression, key, seed, style), style, s.repetition, totalStepsOf (progression)));
}

MelodyStats analyseMelody (const Melody& melody, const Progression& progression, const Key& key,
                           const std::vector<NoteEvent>& accompaniment)
{
    MelodyStats st;
    const double total = progression.totalBeats();

    std::vector<MelodyNote> notes;
    for (const auto& n : melody.notes)
        if (n.startBeat >= 0.0 && n.startBeat < total)   // parked notes (at or after the loop end) are ignored
            notes.push_back (n);
    std::stable_sort (notes.begin(), notes.end(), [] (const MelodyNote& a, const MelodyNote& b) {
        return a.startBeat < b.startBeat || (a.startBeat == b.startBeat && a.note < b.note);
    });

    st.notes = (int) notes.size();
    if (notes.empty())
        return st;

    const int numBars = (int) std::ceil (total / 4.0 - 1.0e-9);
    std::vector<unsigned> barMask ((size_t) std::max (1, numBars), 0u);
    std::vector<char> barActive ((size_t) std::max (1, numBars), 0);

    st.lowest = st.highest = notes.front().note;
    int ct = 0, down = 0, downCt = 0, finals = 0, finalCt = 0, odd = 0, strong = 0, strongClash = 0, outOfScale = 0;

    // Slot chord-scales for outOfChordScaleShare (R1).
    std::vector<std::array<bool, 12>> replacedBySlot;
    {
        int start = 0;
        for (const auto& s : progression.slots)
        {
            const auto info = makeSlotInfo (s, {}, key, start);
            replacedBySlot.push_back (info.replaced);
            start = info.endStep;
        }
    }

    for (size_t i = 0; i < notes.size(); ++i)
    {
        const auto& n = notes[i];
        st.lowest = std::min (st.lowest, n.note);
        st.highest = std::max (st.highest, n.note);

        const int bar = std::clamp ((int) std::floor (n.startBeat / 4.0), 0, (int) barMask.size() - 1);
        barActive[(size_t) bar] = 1;
        const int step = stepOf (n.startBeat);
        if (step >= 0)
            barMask[(size_t) bar] |= 1u << (step % kBarSteps);

        const auto chord = chordAtBeat (progression, n.startBeat);
        bool isCt = false;
        if (chord)
            for (int t : chord->pitchClasses())
                isCt = isCt || pc (t) == pc (n.note);
        ct += isCt ? 1 : 0;

        const bool downbeat = step >= 0 && step % kBarSteps == 0;
        if (downbeat)
        {
            ++down;
            downCt += isCt ? 1 : 0;
        }

        const bool last = i + 1 == notes.size();
        if (last || notes[i + 1].startBeat - (n.startBeat + n.lengthBeats) >= 1.0 - 1.0e-9)
        {
            ++finals;
            finalCt += isCt ? 1 : 0;
        }

        if (step >= 0 && (step & 1) != 0)
            ++odd;

        if (step >= 0 && (step % kBarSteps == 0 || step % kBarSteps == 8))
        {
            ++strong;
            if (! isCt)
            {
                bool clash = false;
                for (const auto& e : accompaniment)
                    if (e.startBeat <= n.startBeat + 1.0e-9 && n.startBeat < e.startBeat + e.lengthBeats - 1.0e-9)
                    {
                        const int d = pc (e.note - n.note);
                        clash = clash || d == 1 || d == 11;
                    }
                strongClash += clash ? 1 : 0;
            }
        }

        const int slot = progression.slotAtBeat (n.startBeat);
        if (slot >= 0 && slot < (int) replacedBySlot.size() && replacedBySlot[(size_t) slot][(size_t) pc (n.note)] && ! isCt)
            ++outOfScale;
    }

    int active = 0;
    for (char a : barActive)
        active += a ? 1 : 0;

    const double count = (double) notes.size();
    st.notesPerActiveBar = active > 0 ? count / active : 0.0;
    st.chordToneShare = ct / count;
    st.downbeatChordToneShare = down > 0 ? (double) downCt / down : 0.0;
    st.phraseFinalChordToneShare = finals > 0 ? (double) finalCt / finals : 0.0;
    st.odd16Share = odd / count;
    st.strongBeatClashShare = strong > 0 ? (double) strongClash / strong : 0.0;
    st.outOfChordScaleShare = outOfScale / count;
    st.seamInterval = std::abs (notes.back().note - notes.front().note);

    // Intervals.
    int intervals = 0, repeats = 0, moving = 0, steps = 0, leaps = 0, leapsWithNext = 0, reversals = 0;
    for (size_t i = 1; i < notes.size(); ++i)
    {
        const int d = notes[i].note - notes[i - 1].note;
        ++intervals;
        if (d == 0)
        {
            ++repeats;
            continue;
        }
        ++moving;
        if (std::abs (d) <= 2)
            ++steps;
        if (std::abs (d) >= 5)
        {
            ++leaps;
            for (size_t j = i + 1; j < notes.size(); ++j)
            {
                const int e = notes[j].note - notes[j - 1].note;
                if (e == 0)
                    continue;
                ++leapsWithNext;
                reversals += sgn (e) == -sgn (d) ? 1 : 0;
                break;
            }
        }
    }
    st.repeatShare = intervals > 0 ? (double) repeats / intervals : 0.0;
    st.stepShareMoving = moving > 0 ? (double) steps / moving : 0.0;
    st.leapShareMoving = moving > 0 ? (double) leaps / moving : 0.0;
    st.reverseAfterLeap = leapsWithNext > 0 ? (double) reversals / leapsWithNext : 0.0;

    // Exact rhythm repeats at a lag of 4 bars (bars where either bar has an onset).
    if (numBars >= 8)
    {
        int pairs = 0, same = 0;
        for (int b = 4; b < numBars; ++b)
        {
            if (! barActive[(size_t) b] && ! barActive[(size_t) b - 4])
                continue;
            ++pairs;
            same += barMask[(size_t) b] == barMask[(size_t) b - 4] && barActive[(size_t) b] == barActive[(size_t) b - 4] ? 1 : 0;
        }
        st.rhythmRepeatLag4 = pairs > 0 ? (double) same / pairs : 0.0;
    }

    return st;
}
} // namespace cb
