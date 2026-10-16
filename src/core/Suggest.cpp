#include "core/Suggest.h"

#include <algorithm>

// Next-chord suggestions.
//
// transitionScore (from, to, key) =
//     clamp (0.72 * F  +  0.18 * R  +  0.10 * S  +  0.06 * [dominant resolution])  *  E
//
//   F  functional score 0..1 (the main signal), built in four steps:
//        1. degree-to-degree table (kMajorMotion / kMinorMotion below, values from how often the move appears in
//           pop / film music) looked up in the key, or in key.parallel() when the source chord is borrowed (so
//           bVI -> bVII in C major reads as VI -> VII in C minor). Same-root moves use a small rule set
//           (sus -> resolved 0.75, IV -> iv 0.9, I -> I7 0.6, major <-> minor 0.45, other colour changes 0.35).
//           Dorian and melodic minor use their own table (minor ii, major IV).
//        2. dominant bonus +0.12 when a dominant-shaped V chord falls a fifth (+0.10 for moving TO the major V of a
//           minor key); a secondary dominant falling onto its
//           target scores 1.0 (0.7 when the target is not diatonic); any other move from a secondary scores low.
//        3. target factor: borrowed target from a diatonic chord x0.60 (from another borrowed chord x0.92),
//           chromatic target x0.45, secondary-dominant target = 0.75 * avg (how well `from` leads to the
//           secondary's target, how well it leads to the secondary's root degree).
//        4. idioms (max with the result; not for secondary / chromatic targets): bVI->bVII 0.95, bVII->I 0.95, iv->I 0.92, IV->iv 0.9, bVI->V 0.85,
//           bIII->IV 0.75, bVII->IV 0.75, bVI->I 0.72; minor keys: VI->V 0.8, V->I (Picardy) 0.8, IV->i 0.75,
//           VII->VI 0.7, bII->i 0.9 (Phrygian), bII->V 0.85 (Neapolitan); a diminished chord rising a half step
//           0.85 (0.95 onto the tonic).
//   R  root motion: down a fifth 1.0, down a third 0.6-0.65, up a fifth 0.6, up a step 0.55, down a step 0.5,
//      up a half step 0.45, up a third 0.4, down a half step 0.35, same root 0.3, tritone 0.1.
//   S  shared tones: |common pitch classes| / max (|from|, |to|).
//   E  simplicity factor: triads 1.0, seventh chords 0.96, colour chords (sus/add9/6/9) 0.92 - so the plain chord
//      wins a tie with its extensions.
//
// Results in C major: from G -> C 0.99; from Dm -> G 0.93; D7 -> G 0.98; from F -> G 0.78, -> C 0.75;
// from C -> F 0.86, Am 0.78, G 0.77, Dm 0.5, Em 0.39; from Ab -> Bb 0.78.

namespace cb
{
namespace
{
    constexpr float kWeightFunctional = 0.72f;
    constexpr float kWeightRoot = 0.18f;
    constexpr float kWeightShared = 0.10f;
    constexpr float kResolutionBonus = 0.06f;
    constexpr float kDominantBonus = 0.12f;
    constexpr float kMinorVBonus = 0.10f;
    constexpr float kSeventhFactor = 0.96f;
    constexpr float kColourFactor = 0.92f;

    constexpr float kBorrowedFromDiatonic = 0.60f;
    constexpr float kBorrowedFromBorrowed = 0.92f;
    constexpr float kChromaticTarget = 0.45f;
    constexpr float kSecondaryTarget = 0.75f;
    constexpr float kParallelContextFromKey = 0.9f;   // key chord -> chord whose root is only in the parallel key
    constexpr float kKeyContextFromBorrowed = 0.85f;  // borrowed chord read in the key's own context

    // [from degree][to degree], degree 0 = tonic. Diagonal unused (same root handled separately).
    //                                         I     ii    iii   IV    V     vi    vii
    constexpr float kMajorMotion[7][7] = { { 0.00f, 0.55f, 0.35f, 0.90f, 0.90f, 0.85f, 0.20f },   // I
                                           { 0.40f, 0.00f, 0.30f, 0.45f, 1.00f, 0.35f, 0.45f },   // ii
                                           { 0.30f, 0.35f, 0.00f, 0.75f, 0.35f, 0.85f, 0.15f },   // iii
                                           { 0.85f, 0.50f, 0.35f, 0.00f, 0.95f, 0.60f, 0.35f },   // IV
                                           { 0.88f, 0.25f, 0.30f, 0.50f, 0.00f, 0.80f, 0.20f },   // V
                                           { 0.45f, 0.70f, 0.40f, 0.85f, 0.70f, 0.00f, 0.15f },   // vi
                                           { 1.00f, 0.20f, 0.50f, 0.20f, 0.35f, 0.40f, 0.00f } }; // vii°

    //                                         i     ii°   III   iv    v     VI    VII
    constexpr float kMinorMotion[7][7] = { { 0.00f, 0.40f, 0.60f, 0.85f, 0.65f, 0.90f, 0.80f },   // i
                                           { 0.35f, 0.00f, 0.25f, 0.30f, 0.95f, 0.25f, 0.30f },   // ii°
                                           { 0.45f, 0.25f, 0.00f, 0.55f, 0.35f, 0.75f, 0.80f },   // III
                                           { 0.80f, 0.30f, 0.40f, 0.00f, 0.85f, 0.60f, 0.80f },   // iv
                                           { 0.85f, 0.20f, 0.35f, 0.45f, 0.00f, 0.75f, 0.40f },   // v
                                           { 0.60f, 0.40f, 0.70f, 0.75f, 0.60f, 0.00f, 0.95f },   // VI
                                           { 0.90f, 0.20f, 0.80f, 0.45f, 0.35f, 0.50f, 0.00f } }; // VII

    // Dorian and melodic minor: minor ii, major IV, diminished vi.
    //                                          i     ii    III   IV    v     vi°   VII
    constexpr float kDorianMotion[7][7] = { { 0.00f, 0.60f, 0.60f, 0.90f, 0.65f, 0.20f, 0.80f },   // i
                                            { 0.55f, 0.00f, 0.50f, 0.60f, 0.85f, 0.20f, 0.60f },   // ii
                                            { 0.50f, 0.45f, 0.00f, 0.85f, 0.40f, 0.20f, 0.70f },   // III
                                            { 0.90f, 0.45f, 0.45f, 0.00f, 0.70f, 0.20f, 0.70f },   // IV
                                            { 0.85f, 0.30f, 0.40f, 0.55f, 0.00f, 0.20f, 0.50f },   // v
                                            { 0.50f, 0.30f, 0.30f, 0.30f, 0.40f, 0.00f, 0.70f },   // vi°
                                            { 0.85f, 0.30f, 0.70f, 0.80f, 0.40f, 0.20f, 0.00f } }; // VII

    bool isDominantShaped (ChordQuality q)
    {
        return q == ChordQuality::Major || q == ChordQuality::Dominant7 || q == ChordQuality::Dominant9;
    }

    bool isSus (ChordQuality q)
    {
        return q == ChordQuality::Sus2 || q == ChordQuality::Sus4 || q == ChordQuality::Dominant7Sus4;
    }

    bool isMinorish (ChordQuality q)
    {
        return q == ChordQuality::Minor || q == ChordQuality::Minor6 || q == ChordQuality::MinorAdd9
            || q == ChordQuality::Minor7 || q == ChordQuality::MinorMajor7 || q == ChordQuality::Minor9;
    }

    bool isMajorish (ChordQuality q)
    {
        return q == ChordQuality::Major || q == ChordQuality::Major6 || q == ChordQuality::Add9
            || q == ChordQuality::Major7 || q == ChordQuality::Major9 || q == ChordQuality::Dominant7
            || q == ChordQuality::Dominant9;
    }

    bool isDiminishedFamily (ChordQuality q)
    {
        return q == ChordQuality::Diminished || q == ChordQuality::Diminished7 || q == ChordQuality::HalfDiminished7;
    }

    bool isSeventhQuality (ChordQuality q)
    {
        return q == ChordQuality::Major7 || q == ChordQuality::Minor7 || q == ChordQuality::Dominant7
            || q == ChordQuality::HalfDiminished7 || q == ChordQuality::Diminished7 || q == ChordQuality::MinorMajor7;
    }

    bool isTriadQuality (ChordQuality q)
    {
        return q == ChordQuality::Major || q == ChordQuality::Minor || q == ChordQuality::Diminished
            || q == ChordQuality::Augmented;
    }

    float rootMotion (int fromRoot, int toRoot)
    {
        static const float byInterval[12] = { 0.30f, 0.45f, 0.55f, 0.40f, 0.40f, 1.00f,
                                              0.10f, 0.60f, 0.60f, 0.65f, 0.50f, 0.35f };
        return byInterval[pc (toRoot - fromRoot)];
    }

    float sharedTones (const Chord& a, const Chord& b)
    {
        bool inA[12] = {}, inB[12] = {};
        for (int p : a.pitchClasses()) inA[pc (p)] = true;
        for (int p : b.pitchClasses()) inB[pc (p)] = true;

        int na = 0, nb = 0, common = 0;
        for (int i = 0; i < 12; ++i)
        {
            na += inA[i] ? 1 : 0;
            nb += inB[i] ? 1 : 0;
            common += (inA[i] && inB[i]) ? 1 : 0;
        }
        const int denom = std::max (na, nb);
        return denom > 0 ? (float) common / (float) denom : 0.0f;
    }

    // Degree-table motion between two roots in `ctx`, or -1 when either root is outside ctx's scale or they coincide.
    float degreeMotion (const Key& ctx, int fromRoot, int toRoot)
    {
        const int df = ctx.degreeOf (fromRoot);
        const int dt = ctx.degreeOf (toRoot);
        if (df < 0 || dt < 0 || df == dt)
            return -1.0f;

        if (ctx.scale == ScaleType::Dorian || ctx.scale == ScaleType::MelodicMinor)
            return kDorianMotion[df][dt];

        return scaleInfo (ctx.scale).isMinorFamily ? kMinorMotion[df][dt] : kMajorMotion[df][dt];
    }

    float sameRootScore (const Chord& a, const Chord& b, HarmonicFunction toFunction)
    {
        if (isSus (a.quality) && ! isSus (b.quality))     return 0.75f;   // Csus4 -> C
        if (toFunction == HarmonicFunction::Secondary)    return 0.60f;   // C -> C7 (V7/IV)
        if ((isMajorish (a.quality) && isMinorish (b.quality)) || (isMinorish (a.quality) && isMajorish (b.quality)))
            return 0.45f;                                                   // C <-> Cm
        if (isSus (b.quality))                            return 0.45f;   // C -> Csus4
        return 0.35f;                                                       // C -> Cmaj7 / Cadd9 ...
    }

    // Functional motion from a to b, ignoring what kind of chord b is.
    float baseMotion (const Chord& a, const Chord& b, const Key& key, HarmonicFunction fromFunction, HarmonicFunction toFunction)
    {
        if (a.root == b.root)
            return sameRootScore (a, b, toFunction);

        const float inKey = degreeMotion (key, a.root, b.root);
        const float inParallel = degreeMotion (key.parallel(), a.root, b.root);

        if (fromFunction == HarmonicFunction::Borrowed)
        {
            const float best = std::max (inParallel, inKey >= 0.0f ? inKey * kKeyContextFromBorrowed : -1.0f);
            if (best >= 0.0f)
                return best;
        }
        else
        {
            if (inKey >= 0.0f)      return inKey;
            if (inParallel >= 0.0f) return inParallel * kParallelContextFromKey;
        }

        return 0.2f + 0.3f * rootMotion (a.root, b.root);
    }

    // How well `a` leads into the diatonic chord rooted on `targetRoot` (used for secondary-dominant targets).
    float leadInto (const Chord& a, int targetRoot, const Key& key, HarmonicFunction fromFunction)
    {
        if (a.root == targetRoot)
            return 0.5f;   // x -> V7/x -> x vamp

        const float inKey = degreeMotion (key, a.root, targetRoot);
        if (inKey >= 0.0f && fromFunction != HarmonicFunction::Borrowed)
            return inKey;

        const float inParallel = degreeMotion (key.parallel(), a.root, targetRoot);
        const float best = std::max (inKey, inParallel);
        return best >= 0.0f ? best : 0.3f;
    }

    // Well-known moves that the tables alone under-rate (offsets in semitones above the tonic).
    float idiomScore (const Chord& a, const Chord& b, const Key& key, HarmonicFunction toFunction)
    {
        if (toFunction == HarmonicFunction::Secondary || toFunction == HarmonicFunction::Chromatic)
            return 0.0f;

        const int fa = pc (a.root - key.tonic);
        const int fb = pc (b.root - key.tonic);
        const auto qa = a.quality;
        const auto qb = b.quality;
        float best = 0.0f;
        auto offer = [&best] (bool cond, float v) { if (cond) best = std::max (best, v); };

        if (isDiminishedFamily (qa) && pc (b.root - a.root) == 1 && ! isDiminishedFamily (qb))
            offer (true, fb == 0 ? 0.95f : 0.85f);   // leading-tone diminished resolving up a half step

        if (! scaleInfo (key.scale).isMinorFamily)
        {
            offer (fa == 8 && isMajorish (qa) && fb == 10 && isMajorish (qb), 0.95f);   // bVI -> bVII
            offer (fa == 10 && isMajorish (qa) && fb == 0 && isMajorish (qb), 0.95f);   // bVII -> I (incl. backdoor bVII7)
            offer (fa == 5 && isMinorish (qa) && fb == 0 && isMajorish (qb), 0.92f);    // iv -> I
            offer (fa == 5 && fb == 5 && isMajorish (qa) && isMinorish (qb), 0.90f);    // IV -> iv
            offer (fa == 8 && isMajorish (qa) && fb == 7 && isDominantShaped (qb), 0.85f); // bVI -> V
            offer (fa == 3 && isMajorish (qa) && fb == 5, 0.75f);                        // bIII -> IV / iv
            offer (fa == 10 && isMajorish (qa) && fb == 5 && isMajorish (qb), 0.75f);   // bVII -> IV
            offer (fa == 8 && isMajorish (qa) && fb == 0 && isMajorish (qb), 0.72f);    // bVI -> I
        }
        else
        {
            offer (fa == 8 && isMajorish (qa) && fb == 7 && isDominantShaped (qb), 0.80f); // VI -> V (Andalusian)
            offer (fa == 7 && isDominantShaped (qa) && fb == 0 && isMajorish (qb), 0.80f); // V -> I (Picardy)
            offer (fa == 5 && isMajorish (qa) && fb == 0 && isMinorish (qb), 0.75f);    // IV -> i (Dorian)
            offer (fa == 10 && isMajorish (qa) && fb == 8 && isMajorish (qb), 0.70f);   // VII -> VI
            offer (fa == 1 && isMajorish (qa) && fb == 0, 0.90f);                        // bII -> i (Phrygian cadence)
            offer (fa == 1 && isMajorish (qa) && fb == 7 && isDominantShaped (qb), 0.85f); // Neapolitan bII -> V
        }

        return best;
    }
} // namespace

float transitionScore (const Chord& fromChord, const Chord& toChord, const Key& key)
{
    const Chord a { pc (fromChord.root), fromChord.quality };
    const Chord b { pc (toChord.root), toChord.quality };
    if (a == b)
        return 0.0f;

    const auto fa = harmonicFunction (a, key);
    const auto fb = harmonicFunction (b, key);

    float functional = 0.0f;
    bool resolution = false;

    if (fa == HarmonicFunction::Secondary)
    {
        if (b.root == pc (a.root + 5))
        {
            functional = isDiatonic (b, key) ? 1.0f : 0.7f;
            resolution = true;
        }
        else
        {
            functional = 0.15f + 0.25f * baseMotion (a, b, key, fa, fb);
        }
    }
    else
    {
        functional = baseMotion (a, b, key, fa, fb);

        if (fa == HarmonicFunction::Dominant && key.degreeOf (a.root) == 4 && isDominantShaped (a.quality)
            && b.root == pc (a.root + 5))
        {
            functional = std::min (1.0f, functional + kDominantBonus);
            resolution = true;
        }

        // Minor keys: the major V (leading tone) is a stronger destination than the tables' modal v.
        if (scaleInfo (key.scale).isMinorFamily && isDominantShaped (b.quality) && key.degreeOf (b.root) == 4
            && scaleInfo (key.scale).offsets[4] == 7)
            functional = std::min (1.0f, functional + kMinorVBonus);

        switch (fb)
        {
            case HarmonicFunction::Borrowed:
                functional *= (fa == HarmonicFunction::Borrowed ? kBorrowedFromBorrowed : kBorrowedFromDiatonic);
                break;

            case HarmonicFunction::Secondary:
                functional = kSecondaryTarget * 0.5f * (leadInto (a, pc (b.root + 5), key, fa) + functional);
                break;

            case HarmonicFunction::Chromatic:
                functional *= kChromaticTarget;
                break;

            case HarmonicFunction::Tonic:
            case HarmonicFunction::Subdominant:
            case HarmonicFunction::Dominant:
                break;
        }

        functional = std::max (functional, idiomScore (a, b, key, fb));
    }

    float score = kWeightFunctional * functional
                + kWeightRoot * rootMotion (a.root, b.root)
                + kWeightShared * sharedTones (a, b)
                + (resolution ? kResolutionBonus : 0.0f);

    score = std::clamp (score, 0.0f, 1.0f);

    if (isSeventhQuality (b.quality))       score *= kSeventhFactor;
    else if (! isTriadQuality (b.quality))  score *= kColourFactor;

    return std::clamp (score, 0.0f, 1.0f);
}

std::vector<std::vector<float>> suggestionsFor (const Chord& from, const std::vector<PaletteSection>& palette, const Key& key)
{
    std::vector<std::vector<float>> out;
    out.reserve (palette.size());

    for (auto& section : palette)
    {
        std::vector<float> row;
        row.reserve (section.chords.size());
        for (auto& pad : section.chords)
            row.push_back (transitionScore (from, pad.chord, key));
        out.push_back (std::move (row));
    }
    return out;
}
// STUB (round 2) - to be implemented by the core-suggest agent
float openingScore (const Chord& chord, const Key& key)
{
    return key.degreeOf (chord.root) == 0 ? 1.0f : 0.3f;
}

} // namespace cb
