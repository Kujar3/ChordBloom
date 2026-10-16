#include "core/Voicing.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

//==============================================================================
// Voicing rules (how a Chord becomes MIDI notes)
//
// Chord tones are split into the "core" (root, 3rd/sus tone, 5th, 6th/7th: every interval below 12) and an optional
// added 9th (interval 14). A voicing is built as an ORDER of chord tones from bottom to top; the notes are then
// stacked so every tone sits at the next higher occurrence of its pitch class above the tone below it. The shape is
// finally placed in the octave whose mean is closest to the (effective) centre note.
//
// Inversion k always means "chord tone k (in interval order) is the lowest note", in every style:
//   0 = root, 1 = 3rd (or sus tone), 2 = 5th, 3 = 6th/7th (or the 9th of an add9 chord), 4 = 9th of a 9th chord.
//
// Close   Core tones in close position starting from the bass tone (a rotation of root-3-5-7), packed within an
//         octave. The 9th is never jammed a whole tone above the root at the bottom: it is always voiced ABOVE the
//         3rd, as the top voice (Cadd9 = C E G D, Cmaj9 = C E G B D, Cadd9/G = G C E D). Only when the 9th itself
//         is the requested bass does it sit at the bottom, with the core packed above it (D E G B C).
//         Consequently 9th chords may span up to a 12th; everything else spans less than an octave.
// Open    Drop-2: the close voicing whose second-highest voice, dropped an octave, lands on the requested bass
//         tone (C G E, C G B E, E B C G, ...). For 9th chords the drop-2 is applied to the core and the 9th stays
//         on top (Cmaj9 = C G B E D). 9th in the bass: 9th, then the core in drop-2 from its 5th (D G C E B).
// Spread  Wide pad voicing over ~2 octaves: bass tone low, then every other core tone first (in 5ths/6ths), then the
//         rest an octave up, 9th on top (C G E B D, C G E A). Three-note chords double their bass tone two octaves
//         up so pads stay full (C G E C). Below MIDI 52 no two adjacent notes are closer than a perfect 4th (any
//         such upper note is moved up an octave), so low settings stay clear rather than muddy.
//
// Voice leading (inversion == -1 with a previous voicing): candidates are every inversion of the style at every
// octave placement whose mean lies within +-7 semitones of the effective centre. The 9th-in-bass inversion is only
// used when asked for explicitly (an automatic choice should not put the 9th under the 3rd). Cost = sum of
// |movement|, pairing notes in sorted order; when the note counts differ the best order-preserving pairing is used
// and every unpaired note costs its distance to the nearest note of the other voicing. Ties: mean closest to the
// centre, then lower mean, then lower inversion. Because every candidate is inside the window there is no drift.
//
// octaveShift: the voicing is placed around centerNote + 12 * octaveShift (identical to shifting after placement,
// but lets the low-register Spread rule see the real register). Every note always ends up in [21, 108].
//==============================================================================

namespace cb
{
const char* voicingStyleName (VoicingStyle s)
{
    static const char* names[] = { "Close", "Open", "Spread" };
    const int i = (int) s;
    return names[i < 0 || i >= numVoicingStyles ? 0 : i];
}

namespace
{
constexpr int kLowestNote = 21;
constexpr int kHighestNote = 108;
constexpr double kVoiceLeadingWindow = 7.0;
constexpr int kMudThreshold = 52;      // below this note, Spread keeps adjacent notes at least a perfect 4th apart
constexpr int kMinClearInterval = 5;   // perfect 4th
constexpr double kEps = 1.0e-9;

struct ChordTones
{
    std::vector<int> pcs;    // pitch class of every chord tone, in interval order
    std::vector<int> core;   // indices (into pcs) of the tones below the octave: root, 3rd, 5th, 6th/7th
    int ninth = -1;          // index of the added 9th (interval >= 12), or -1
};

ChordTones analyse (const Chord& chord)
{
    ChordTones t;
    const auto& intervals = qualityIntervals (chord.quality);

    for (int i = 0; i < (int) intervals.size(); ++i)
    {
        t.pcs.push_back (pc (chord.root + intervals[(size_t) i]));

        if (intervals[(size_t) i] >= 12 && t.ninth < 0)
            t.ninth = i;
        else
            t.core.push_back (i);
    }

    return t;
}

VoicingStyle sanitiseStyle (VoicingStyle s)
{
    const int i = (int) s;
    return (i < 0 || i >= numVoicingStyles) ? VoicingStyle::Close : s;
}

// Positions within the core (0 .. nc-1), bottom to top, with core position `b` in the bass.
std::vector<int> coreOrder (VoicingStyle style, int nc, int b)
{
    std::vector<int> order;
    if (nc <= 0)
        return order;

    b = ((b % nc) + nc) % nc;

    if (style == VoicingStyle::Open && nc >= 3)
    {
        // Drop-2 of the close rotation starting at (b + 2): its second-highest voice is b, which drops to the bottom.
        const int j = (b + 2) % nc;
        order.push_back (b);
        for (int i = 0; i < nc; ++i)
        {
            const int t = (j + i) % nc;
            if (t != b)
                order.push_back (t);
        }
    }
    else if (style == VoicingStyle::Spread && nc >= 3)
    {
        // Bass, then every other tone (5th, 7th...), then the in-between tones (3rd...) an octave up.
        order.push_back (b);
        for (int off = 2; off < nc; off += 2)
            order.push_back ((b + off) % nc);
        for (int off = 1; off < nc; off += 2)
            order.push_back ((b + off) % nc);
    }
    else
    {
        for (int i = 0; i < nc; ++i)
            order.push_back ((b + i) % nc);
    }

    return order;
}

// Chord-tone indices (into ChordTones::pcs), bottom to top, for inversion `inv` (already clamped to 0..n-1).
std::vector<int> toneOrder (const ChordTones& t, VoicingStyle style, int inv)
{
    const int nc = (int) t.core.size();
    std::vector<int> order;

    if (nc == 0)
    {
        for (int i = 0; i < (int) t.pcs.size(); ++i)
            order.push_back (i);
        return order;
    }

    if (inv == t.ninth)
    {
        // 9th in the bass: Close packs the core above it from the 3rd (D E G B C); Open/Spread continue from the
        // 5th, a 4th above the 9th (D G C E B), which keeps the low end clear.
        const int nextBass = std::min (style == VoicingStyle::Close ? 1 : 2, nc - 1);
        order.push_back (t.ninth);
        for (int p : coreOrder (style, nc, nextBass))
            order.push_back (t.core[(size_t) p]);
        return order;
    }

    int bassPos = 0;
    for (int p = 0; p < nc; ++p)
        if (t.core[(size_t) p] == inv)
            bassPos = p;

    for (int p : coreOrder (style, nc, bassPos))
        order.push_back (t.core[(size_t) p]);

    if (t.ninth >= 0)
        order.push_back (t.ninth);   // the 9th always sits on top, above the 3rd
    else if (style == VoicingStyle::Spread && t.pcs.size() == 3)
        order.push_back (order.front());   // triad pads: double the bass tone two octaves up

    return order;
}

// Relative offsets (bass = 0), each tone at the next higher occurrence of its pitch class.
std::vector<int> stackOffsets (const ChordTones& t, const std::vector<int>& order)
{
    std::vector<int> offsets;
    int current = 0;

    for (size_t i = 0; i < order.size(); ++i)
    {
        if (i == 0)
        {
            offsets.push_back (0);
            continue;
        }

        int step = pc (t.pcs[(size_t) order[i]] - t.pcs[(size_t) order[i - 1]]);
        if (step == 0)
            step = 12;
        current += step;
        offsets.push_back (current);
    }

    return offsets;
}

// Spread only: below kMudThreshold, raise any note closer than a 4th to the note under it by an octave.
void clearLowRegister (std::vector<int>& notes)
{
    for (int guard = 0; guard < 64; ++guard)
    {
        bool changed = false;

        for (size_t i = 1; i < notes.size(); ++i)
        {
            if (notes[i - 1] < kMudThreshold && notes[i] - notes[i - 1] < kMinClearInterval)
            {
                const int raised = notes[i] + 12;
                notes.erase (notes.begin() + (std::ptrdiff_t) i);

                // A collision can only be with the same pitch class, so the chord tone is still present.
                if (std::find (notes.begin(), notes.end(), raised) == notes.end())
                    notes.insert (std::upper_bound (notes.begin(), notes.end(), raised), raised);

                changed = true;
                break;
            }
        }

        if (! changed)
            return;
    }
}

double meanOf (const std::vector<int>& notes)
{
    if (notes.empty())
        return 0.0;

    double sum = 0.0;
    for (int n : notes)
        sum += n;
    return sum / (double) notes.size();
}

bool inRange (const std::vector<int>& notes)
{
    for (int n : notes)
        if (n < kLowestNote || n > kHighestNote)
            return false;
    return true;
}

struct Candidate
{
    std::vector<int> notes;
    double mean = 0.0;
    int inversion = 0;
};

// Every octave placement of one inversion whose notes all lie in [21, 108].
std::vector<Candidate> placements (const ChordTones& t, VoicingStyle style, int inv)
{
    std::vector<Candidate> result;
    const auto order = toneOrder (t, style, inv);
    if (order.empty())
        return result;

    const auto offsets = stackOffsets (t, order);
    const int bassPc = t.pcs[(size_t) order.front()];

    for (int bass = bassPc; bass <= kHighestNote; bass += 12)
    {
        std::vector<int> notes;
        notes.reserve (offsets.size());
        for (int o : offsets)
            notes.push_back (bass + o);

        if (style == VoicingStyle::Spread)
            clearLowRegister (notes);

        if (! inRange (notes))
            continue;

        result.push_back ({ notes, meanOf (notes), inv });
    }

    return result;
}

// Last-resort fallback (unreachable for real chords): fold every note into range by octaves.
std::vector<int> foldIntoRange (std::vector<int> notes)
{
    for (auto& n : notes)
    {
        while (n < kLowestNote)
            n += 12;
        while (n > kHighestNote)
            n -= 12;
    }

    std::sort (notes.begin(), notes.end());
    notes.erase (std::unique (notes.begin(), notes.end()), notes.end());
    return notes;
}

std::vector<int> placeNearest (const ChordTones& t, VoicingStyle style, int inv, double center)
{
    const auto all = placements (t, style, inv);

    const Candidate* best = nullptr;
    double bestDist = std::numeric_limits<double>::max();

    for (auto& c : all)
    {
        const double d = std::abs (c.mean - center);
        if (best == nullptr || d < bestDist - kEps)   // ties keep the lower placement (iterated low -> high)
        {
            best = &c;
            bestDist = d;
        }
    }

    if (best != nullptr)
        return best->notes;

    // Unreachable in practice: build the shape near the centre and fold it into range.
    const auto order = toneOrder (t, style, inv);
    const auto offsets = stackOffsets (t, order);
    std::vector<int> notes;
    const int base = (int) std::lround (center) - 6;
    const int bass = base + pc (t.pcs[(size_t) order.front()] - base);
    for (int o : offsets)
        notes.push_back (bass + o);
    return foldIntoRange (notes);
}

// Sum of |movement| between two sorted voicings. Equal sizes: pair by sorted order. Different sizes: the best
// order-preserving pairing of the smaller voicing into the larger one, unpaired notes cost their distance to the
// nearest note of the smaller voicing.
int movementCost (const std::vector<int>& a, const std::vector<int>& b)
{
    if (a.empty() || b.empty())
        return 0;

    if (a.size() == b.size())
    {
        int cost = 0;
        for (size_t i = 0; i < a.size(); ++i)
            cost += std::abs (a[i] - b[i]);
        return cost;
    }

    const auto& longer = a.size() > b.size() ? a : b;
    const auto& shorter = a.size() > b.size() ? b : a;
    const size_t L = longer.size();
    const size_t S = shorter.size();

    auto nearest = [&shorter] (int note)
    {
        int best = std::numeric_limits<int>::max();
        for (int s : shorter)
            best = std::min (best, std::abs (note - s));
        return best;
    };

    constexpr int inf = std::numeric_limits<int>::max() / 4;
    // dp[i][j]: first i notes of `longer` used, first j notes of `shorter` paired.
    std::vector<std::vector<int>> dp (L + 1, std::vector<int> (S + 1, inf));
    dp[0][0] = 0;

    for (size_t i = 1; i <= L; ++i)
    {
        for (size_t j = 0; j <= std::min (i, S); ++j)
        {
            int best = inf;

            if (dp[i - 1][j] < inf)   // longer[i-1] stays unpaired
                best = std::min (best, dp[i - 1][j] + nearest (longer[i - 1]));

            if (j > 0 && dp[i - 1][j - 1] < inf)   // longer[i-1] pairs with shorter[j-1]
                best = std::min (best, dp[i - 1][j - 1] + std::abs (longer[i - 1] - shorter[j - 1]));

            dp[i][j] = best;
        }
    }

    return dp[L][S];
}
} // namespace

//==============================================================================
std::vector<int> voiceChord (const Chord& chord, const VoicingSettings& settings, int inversion, int octaveShift,
                             const std::vector<int>& previous)
{
    const auto tones = analyse (chord);
    const int n = (int) tones.pcs.size();
    if (n == 0)
        return {};

    const auto style = sanitiseStyle (settings.style);
    const double center = (double) settings.centerNote + 12.0 * (double) octaveShift;

    if (inversion >= 0)
        return placeNearest (tones, style, std::min (inversion, n - 1), center);

    std::vector<int> prev (previous);
    std::sort (prev.begin(), prev.end());
    prev.erase (std::unique (prev.begin(), prev.end()), prev.end());

    if (settings.voiceLeading && ! prev.empty())
    {
        Candidate best;
        bool found = false;
        int bestCost = 0;
        double bestDist = 0.0;

        for (int inv = 0; inv < n; ++inv)
        {
            if (inv == tones.ninth && n > 1)
                continue;   // 9th in the bass only on explicit request

            for (auto& c : placements (tones, style, inv))
            {
                const double dist = std::abs (c.mean - center);
                if (dist > kVoiceLeadingWindow + kEps)
                    continue;

                const int cost = movementCost (c.notes, prev);

                bool better = ! found;
                if (! better)
                {
                    if (cost != bestCost)
                        better = cost < bestCost;
                    else if (std::abs (dist - bestDist) > kEps)
                        better = dist < bestDist;
                    else if (std::abs (c.mean - best.mean) > kEps)
                        better = c.mean < best.mean;
                    else
                        better = c.inversion < best.inversion;
                }

                if (better)
                {
                    best = c;
                    bestCost = cost;
                    bestDist = dist;
                    found = true;
                }
            }
        }

        if (found)
            return best.notes;
    }

    return placeNearest (tones, style, 0, center);
}

std::vector<std::vector<int>> voiceProgression (const Progression& progression, const VoicingSettings& settings)
{
    std::vector<std::vector<int>> out;
    out.reserve (progression.slots.size());

    std::vector<int> previous;
    for (auto& slot : progression.slots)
    {
        previous = voiceChord (slot.chord, settings, slot.inversion, slot.octaveShift, previous);
        out.push_back (previous);
    }

    return out;
}
} // namespace cb
