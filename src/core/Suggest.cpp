#include "core/Suggest.h"

#include <algorithm>

// Next-chord suggestions (round 2: recalibrated against out/midi_stats.md, 349 producer MIDI files, 221 of them minor).
//
// transitionScore (from, to, key) =
//     clamp (0.78 * F  +  0.10 * R  +  0.08 * S  +  0.06 * [dominant resolution]  +  0.02 * [colour chain])  *  E
//
//   F  functional score 0..1, the main signal. It is a blend of textbook function and the data: each table value
//      was set from the theory ranking first, then moved towards the share the move has in the data (aggregated over
//      all colour variants of a degree, e.g. VI + VImaj7 + VIadd9 count as VI). Built in five steps:
//        1. Degree-to-degree table looked up in the key. There are separate major-family (Ionian) and minor-family
//           (Aeolian) tables plus a Dorian / melodic-minor table, and per-mode overrides for the signature moves:
//             minor:  i -> VI .97 / VII 1.0 / III .85 / v .66 / iv .62     (data: VI 20%, VII 17%, III 12%, v 8%, iv 4%)
//                     VII -> i 1.0 (40%), VI -> VII 1.0 (22%), III -> VI 1.0 (30%), v -> VI 1.0 / i .88,
//                     iv -> i .98 / VI .85, ii° -> v .9
//             major:  I -> V 1.0 / IV .92 / vi .85 (V 29%), V -> I .88 (+ dominant bonus) / vi .92 (40%),
//                     IV -> I .95 / V .92, vi -> IV .92 / V .82, ii -> V 1.0, iii -> vi .88
//             modes:  Mixolydian I -> bVII 1.0, bVII -> I .92 / IV .9;  Dorian i -> IV .95, IV -> i .95;
//                     Phrygian i -> bII 1.0, bII -> i 1.0;  Lydian I -> II 1.0 (I -> V .9), II -> I .9;
//                     harmonic minor V -> i 1.0, VI -> V .95, vii° -> i 1.0, III+ -> VI .85 / i .8 (as a target
//                     the augmented III+ scores low)
//           When a root is outside the key the move is read in the related scale (natural minor for the other
//           minor-family scales, major for Lydian / Mixolydian, x0.95) or in key.parallel() (x0.90). A borrowed source
//           chord is read in the scale it was borrowed from (bVI -> bVII in C major reads as VI -> VII in C minor).
//           Same-root moves use a small rule set: sus -> resolved .75, colour/7th -> its plain triad .72 (Am7 -> Am,
//           data i7 -> i 17%), colour -> colour .5 (Fmaj7 -> Fmaj9), major <-> minor .45, plain -> colour .35.
//        2. Dominant bonus +0.12 when a dominant-shaped V (V, V7, V9, V13, V7b9) falls a fifth onto the tonic
//           (+0.06 for Vsus4 / V7sus4); +0.05 for moving TO the major V of a minor key from a non-tonic chord (straight
//           after i the data prefers the modal v). A secondary dominant falling
//           onto its target scores 1.0 (0.7 when the target is not diatonic); other moves from a secondary score low.
//        3. Target factor: borrowed target from a diatonic chord x0.50..0.92 (major keys: iv .70, bVI / bVII .68,
//           i .60, others .50; minor keys: natural-minor chords in harmonic / melodic minor, Dorian, Phrygian,
//           Locrian .92, parallel-major IV .60, others .50), from another borrowed chord x0.92; chromatic target x0.45;
//           secondary-dominant target = 0.75 * avg (how well `from` leads to the secondary's target, the plain motion).
//        4. Idioms (max with the result; not for secondary / chromatic targets). Major family: bVI -> bVII .95,
//           bVII -> I .95, iv -> I .92, IV -> iv .85, bVI -> V .85, bVII -> IV .8, i -> bVI .8, v -> bVI .8,
//           iv -> bVI .75, bIII -> IV .75, bVI -> I .72, bVII -> bVI .7, bVI -> iv .7. Minor family: bII -> i .9,
//           bII -> V .85, VI -> V .8, IV -> i .75, V -> I .7, i -> IV .58. Leading-tone diminished chord rising a half
//           step .85 (.95 onto the tonic).
//        5. Colour-chain idioms the data favours, when both chords are colour chords (7ths / 9ths / add9 / 6, not
//           dominant 7ths): minor VImaj7 -> v7 .95 (20%), IIImaj7 -> iv7 .9 (39%), v7 -> IIImaj9 .8; major
//           Imaj7 -> ii7 .75, iii7 -> Imaj9 .75, IVmaj7 -> iii7 .72, ii7 -> iii7 .65, Imaj9 -> iii7 .62.
//   R  root motion (pop-neutral, not just "down a fifth"): down a fifth 1.0, up a fifth .8, down a major third .7,
//      down a minor third .7, up a step .65, down a step .65, up a minor third .55, up a major third .45,
//      up a half step .45, down a half step .4, same root .3, tritone .1.
//   S  shared tones between the two chords' plain triads: |common pitch classes| / 3.
//   E  extension factor so that the plain chord wins a tie with its own colour variants: triads 1.0, sevenths .95,
//      colour chords (add9 / 6 / 9ths / 11 / sus) .91, exotic (maj7#11, 7b9, 13, m(maj7)) .88, augmented .95;
//      1.0 (exotic .97) when both chords are colour chords (keeping the colour going is what the data does).
//
// Colour chords inherit their base chord's function: harmonicFunction of the chord, or of its seventh / triad core
// when the full chord is chromatic (Cmaj7#11 in C major counts as Imaj7), and R and S use the plain triads. So Am9
// after F scores like Am after F times E. Results in C major: from C: G .89, F .84, Am .79, Dm .49, Em .37, Cmaj7 .36;
// from G: C .97, Am .78; from F: C .85, G .78; from Am: F .84, G .70; Dm -> G .91; D7 -> G .97; Ab -> Bb .81;
// Bb -> C .84; Fm -> C .87. A minor: from Am: F .88, G .84, C .77, Em .62, Dm .61; G -> Am .84; F -> G .84;
// C -> F .91; Em -> F .82 / Am .81; Dm -> Am .87 / F .77; E -> Am .97; Fmaj7 -> Em7 .80; Am7 -> Am .67.
// Modes: G Mixolydian G -> F .84, F -> C / G .81; D Dorian Dm -> G .87; E Phrygian Em -> F .82, F -> Em .82;
// C Lydian C -> D .84, D -> C .77; A harmonic minor F -> E .82 (G#dim .41), Am -> G .75.
//
// openingScore: tonic triad 1.0, tonic colour / seventh .85-.88; minor keys VImaj7 .80, VI .78 (data first chords:
// i 46%, VI-family 21%), iv .5, III .45, VII .42, v .32; major keys vi .72, IV .70, bVI .62 (data: I 25%, then bVI,
// vi, IV), borrowed i .5, ii .48, V .45, bVII .45; modal signature chords (Mixolydian bVII, Phrygian bII, Lydian II)
// .6, Dorian IV .7, and in the modes borrowed chords x.85; secondary dominants .12, chromatic and diminished chords
// .08-.1 (Locrian's diminished tonic .8).

namespace cb
{
namespace
{
    constexpr float kWeightFunctional = 0.78f;
    constexpr float kWeightRoot = 0.10f;
    constexpr float kWeightShared = 0.08f;
    constexpr float kResolutionBonus = 0.06f;
    constexpr float kColourChainBonus = 0.02f;

    constexpr float kDominantBonus = 0.12f;
    constexpr float kSusDominantBonus = 0.06f;
    constexpr float kMinorVBonus = 0.05f;

    constexpr float kSeventhFactor = 0.95f;
    constexpr float kColourFactor = 0.91f;
    constexpr float kExoticFactor = 0.88f;
    constexpr float kAugmentedFactor = 0.95f;

    constexpr float kBorrowedFromBorrowed = 0.92f;
    constexpr float kChromaticTarget = 0.45f;
    constexpr float kSecondaryTarget = 0.75f;
    constexpr float kRelatedContext = 0.95f;          // e.g. natural-minor reading of a move in harmonic minor
    constexpr float kParallelContext = 0.90f;         // key chord -> chord whose root is only in the parallel key
    constexpr float kKeyContextFromBorrowed = 0.85f;  // borrowed chord read in the key's own context

    //==========================================================================
    // [from degree][to degree], degree 0 = tonic. Diagonal unused (same root handled separately).
    //                                         I     ii    iii   IV    V     vi    vii°
    constexpr float kMajorMotion[7][7] = { { 0.00f, 0.55f, 0.35f, 0.92f, 1.00f, 0.85f, 0.20f },   // I
                                           { 0.45f, 0.00f, 0.45f, 0.50f, 1.00f, 0.35f, 0.45f },   // ii
                                           { 0.45f, 0.40f, 0.00f, 0.80f, 0.35f, 0.88f, 0.15f },   // iii
                                           { 0.95f, 0.50f, 0.40f, 0.00f, 0.92f, 0.75f, 0.35f },   // IV
                                           { 0.88f, 0.25f, 0.30f, 0.50f, 0.00f, 0.92f, 0.20f },   // V
                                           { 0.60f, 0.65f, 0.40f, 0.92f, 0.82f, 0.00f, 0.15f },   // vi
                                           { 1.00f, 0.20f, 0.50f, 0.20f, 0.35f, 0.40f, 0.00f } }; // vii°

    //                                         i     ii°   III   iv    v     VI    VII
    constexpr float kMinorMotion[7][7] = { { 0.00f, 0.30f, 0.85f, 0.62f, 0.66f, 0.97f, 1.00f },   // i
                                           { 0.40f, 0.00f, 0.30f, 0.30f, 0.90f, 0.25f, 0.30f },   // ii°
                                           { 0.55f, 0.25f, 0.00f, 0.65f, 0.45f, 1.00f, 0.80f },   // III
                                           { 0.98f, 0.30f, 0.40f, 0.00f, 0.70f, 0.85f, 0.72f },   // iv
                                           { 0.88f, 0.20f, 0.50f, 0.55f, 0.00f, 1.00f, 0.70f },   // v
                                           { 0.70f, 0.40f, 0.65f, 0.75f, 0.68f, 0.00f, 1.00f },   // VI
                                           { 1.00f, 0.20f, 0.50f, 0.62f, 0.48f, 0.80f, 0.00f } }; // VII

    // Dorian (and the base for melodic minor): minor ii, major IV, diminished vi.
    //                                          i     ii    III   IV    v     vi°   VII
    constexpr float kDorianMotion[7][7] = { { 0.00f, 0.60f, 0.70f, 0.95f, 0.65f, 0.20f, 0.90f },   // i
                                            { 0.65f, 0.00f, 0.55f, 0.60f, 0.80f, 0.20f, 0.50f },   // ii
                                            { 0.50f, 0.45f, 0.00f, 0.85f, 0.45f, 0.20f, 0.75f },   // III
                                            { 0.95f, 0.45f, 0.50f, 0.00f, 0.65f, 0.20f, 0.70f },   // IV
                                            { 0.85f, 0.35f, 0.45f, 0.65f, 0.00f, 0.20f, 0.55f },   // v
                                            { 0.50f, 0.30f, 0.30f, 0.30f, 0.40f, 0.00f, 0.70f },   // vi°
                                            { 0.90f, 0.30f, 0.70f, 0.80f, 0.45f, 0.20f, 0.00f } }; // VII

    struct Override
    {
        int from, to;
        float value;
    };

    // Lydian: I II iii #iv° V vi vii (major table base).
    constexpr Override kLydianOverrides[] = {
        { 0, 1, 1.00f }, { 0, 4, 0.90f }, { 1, 0, 0.90f }, { 1, 4, 0.60f }, { 1, 5, 0.55f }, { 5, 1, 0.70f },
        { 2, 1, 0.60f }, { 4, 1, 0.45f }, { 0, 3, 0.15f }, { 3, 4, 0.60f }, { 6, 0, 0.75f }, { 0, 6, 0.45f },
    };

    // Mixolydian: I ii iii° IV v vi bVII (major table base).
    constexpr Override kMixolydianOverrides[] = {
        { 0, 6, 1.00f }, { 0, 5, 0.70f }, { 6, 3, 0.90f }, { 6, 0, 0.92f }, { 0, 4, 0.55f }, { 4, 3, 0.75f },
        { 4, 0, 0.70f }, { 4, 6, 0.65f }, { 4, 5, 0.55f }, { 3, 6, 0.65f }, { 3, 4, 0.55f }, { 0, 2, 0.15f },
        { 5, 6, 0.65f }, { 1, 6, 0.65f }, { 1, 3, 0.70f }, { 1, 0, 0.55f }, { 6, 4, 0.45f }, { 1, 4, 0.65f },
    };

    // Phrygian: i bII bIII iv v° bVI bvii (minor table base).
    constexpr Override kPhrygianOverrides[] = {
        { 0, 1, 1.00f }, { 0, 5, 0.85f }, { 1, 0, 1.00f }, { 1, 4, 0.30f }, { 1, 2, 0.60f }, { 1, 6, 0.55f }, { 1, 5, 0.50f },
        { 1, 3, 0.50f }, { 5, 1, 0.80f }, { 2, 1, 0.70f }, { 6, 0, 0.85f }, { 6, 1, 0.65f }, { 0, 4, 0.20f },
        { 3, 1, 0.65f }, { 0, 6, 0.75f }, { 4, 0, 0.50f }, { 6, 5, 0.60f },
    };

    // Harmonic minor: i ii° III+ iv V VI vii° (minor table base).
    constexpr Override kHarmonicMinorOverrides[] = {
        { 4, 0, 1.00f }, { 4, 5, 0.75f }, { 4, 3, 0.45f }, { 4, 2, 0.20f }, { 4, 6, 0.35f }, { 4, 1, 0.20f },
        { 5, 4, 0.95f }, { 5, 6, 0.45f }, { 5, 2, 0.35f },
        { 6, 0, 1.00f }, { 6, 1, 0.20f }, { 6, 2, 0.30f }, { 6, 3, 0.30f }, { 6, 4, 0.40f }, { 6, 5, 0.40f },
        { 0, 4, 0.75f }, { 0, 2, 0.25f }, { 0, 6, 0.45f },
        { 2, 0, 0.80f }, { 2, 3, 0.60f }, { 2, 4, 0.65f }, { 2, 5, 0.85f }, { 2, 6, 0.30f }, { 2, 1, 0.20f },
        { 3, 4, 0.88f }, { 3, 6, 0.45f }, { 3, 2, 0.25f }, { 1, 4, 0.95f }, { 1, 2, 0.20f },
    };

    // Melodic minor: i ii III+ IV V vi° vii° (Dorian table base).
    constexpr Override kMelodicMinorOverrides[] = {
        { 4, 0, 1.00f }, { 3, 4, 0.90f }, { 1, 4, 0.95f }, { 0, 4, 0.75f }, { 6, 0, 1.00f }, { 0, 6, 0.45f },
        { 0, 2, 0.25f }, { 2, 3, 0.50f }, { 2, 5, 0.30f }, { 3, 6, 0.45f }, { 5, 6, 0.60f }, { 6, 2, 0.20f },
        { 4, 3, 0.45f }, { 4, 5, 0.40f }, { 4, 6, 0.30f }, { 1, 2, 0.25f }, { 2, 0, 0.45f },
    };

    // Locrian: i° bII biii iv bV bVI bvii (minor table base). Rarely used; keep the unstable tonic moving.
    constexpr Override kLocrianOverrides[] = {
        { 0, 1, 0.85f }, { 1, 0, 0.60f }, { 1, 2, 0.70f }, { 2, 1, 0.60f }, { 0, 5, 0.80f }, { 5, 1, 0.75f },
        { 6, 1, 0.60f }, { 4, 5, 0.65f }, { 0, 4, 0.30f }, { 1, 5, 0.60f }, { 5, 0, 0.55f }, { 5, 2, 0.40f },
        { 5, 3, 0.45f }, { 5, 4, 0.35f },
    };

    template <size_t N>
    bool findOverride (const Override (&list)[N], int df, int dt, float& out)
    {
        for (auto& o : list)
            if (o.from == df && o.to == dt)
            {
                out = o.value;
                return true;
            }
        return false;
    }

    float tableValue (ScaleType scale, int df, int dt)
    {
        float v = 0.0f;
        switch (scale)
        {
            case ScaleType::Lydian:        if (findOverride (kLydianOverrides, df, dt, v)) return v;        break;
            case ScaleType::Mixolydian:    if (findOverride (kMixolydianOverrides, df, dt, v)) return v;    break;
            case ScaleType::Phrygian:      if (findOverride (kPhrygianOverrides, df, dt, v)) return v;      break;
            case ScaleType::HarmonicMinor: if (findOverride (kHarmonicMinorOverrides, df, dt, v)) return v; break;
            case ScaleType::MelodicMinor:  if (findOverride (kMelodicMinorOverrides, df, dt, v)) return v;  break;
            case ScaleType::Locrian:       if (findOverride (kLocrianOverrides, df, dt, v)) return v;       break;
            case ScaleType::Major:
            case ScaleType::NaturalMinor:
            case ScaleType::Dorian:
            case ScaleType::NumScales:
                break;
        }

        if (scale == ScaleType::Dorian || scale == ScaleType::MelodicMinor)
            return kDorianMotion[df][dt];

        return scaleInfo (scale).isMinorFamily ? kMinorMotion[df][dt] : kMajorMotion[df][dt];
    }

    //==========================================================================
    // Chord shape: the plain triad underneath a chord, the triad / seventh core that carries its function, and how
    // it is extended.
    enum class Ext
    {
        Triad,
        Seventh,
        Colour,
        Sus
    };

    struct Shape
    {
        ChordQuality triad;   // Major / Minor / Diminished / Augmented, or Sus2 / Sus4 (no third)
        ChordQuality core;    // Cmaj9 -> Cmaj7, Cadd9 -> C, Am11 -> Am7, D7b9 -> D7
        Ext ext;
        bool dominant;        // major third + minor seventh (7, 9, 13, 7b9), or 7sus4
        bool exotic;          // maj7#11, 7b9, 13, m(maj7)
    };

    Shape shapeOf (ChordQuality q)
    {
        using Q = ChordQuality;
        switch (q)
        {
            case Q::Major:           return { Q::Major,      Q::Major,           Ext::Triad,   false, false };
            case Q::Minor:           return { Q::Minor,      Q::Minor,           Ext::Triad,   false, false };
            case Q::Diminished:      return { Q::Diminished, Q::Diminished,      Ext::Triad,   false, false };
            case Q::Augmented:       return { Q::Augmented,  Q::Augmented,       Ext::Triad,   false, false };
            case Q::Sus2:            return { Q::Sus2,       Q::Sus2,            Ext::Sus,     false, false };
            case Q::Sus4:            return { Q::Sus4,       Q::Sus4,            Ext::Sus,     false, false };
            case Q::Major6:          return { Q::Major,      Q::Major,           Ext::Colour,  false, false };
            case Q::Minor6:          return { Q::Minor,      Q::Minor,           Ext::Colour,  false, false };
            case Q::Add9:            return { Q::Major,      Q::Major,           Ext::Colour,  false, false };
            case Q::MinorAdd9:       return { Q::Minor,      Q::Minor,           Ext::Colour,  false, false };
            case Q::Major7:          return { Q::Major,      Q::Major7,          Ext::Seventh, false, false };
            case Q::Minor7:          return { Q::Minor,      Q::Minor7,          Ext::Seventh, false, false };
            case Q::Dominant7:       return { Q::Major,      Q::Dominant7,       Ext::Seventh, true,  false };
            case Q::HalfDiminished7: return { Q::Diminished, Q::HalfDiminished7, Ext::Seventh, false, false };
            case Q::Diminished7:     return { Q::Diminished, Q::Diminished7,     Ext::Seventh, false, false };
            case Q::MinorMajor7:     return { Q::Minor,      Q::MinorMajor7,     Ext::Seventh, false, true };
            case Q::Dominant7Sus4:   return { Q::Sus4,       Q::Dominant7Sus4,   Ext::Sus,     true,  false };
            case Q::Major9:          return { Q::Major,      Q::Major7,          Ext::Colour,  false, false };
            case Q::Minor9:          return { Q::Minor,      Q::Minor7,          Ext::Colour,  false, false };
            case Q::Dominant9:       return { Q::Major,      Q::Dominant7,       Ext::Colour,  true,  false };
            case Q::Minor11:         return { Q::Minor,      Q::Minor7,          Ext::Colour,  false, false };
            case Q::Major69:         return { Q::Major,      Q::Major6,          Ext::Colour,  false, false };
            case Q::Dominant13:      return { Q::Major,      Q::Dominant7,       Ext::Colour,  true,  true };
            case Q::Major7Sharp11:   return { Q::Major,      Q::Major7,          Ext::Colour,  false, true };
            case Q::Dominant7Flat9:  return { Q::Major,      Q::Dominant7,       Ext::Colour,  true,  true };
            case Q::NumQualities:    break;
        }
        return { Q::Major, Q::Major, Ext::Triad, false, false };
    }

    bool isMajorish (ChordQuality q)  { return shapeOf (q).triad == ChordQuality::Major; }
    bool isMinorish (ChordQuality q)  { return shapeOf (q).triad == ChordQuality::Minor; }
    bool isSus (ChordQuality q)       { return shapeOf (q).ext == Ext::Sus; }
    bool isDiminishedFamily (ChordQuality q) { return shapeOf (q).triad == ChordQuality::Diminished; }

    // A major triad or a dominant 7 / 9 / 13 / 7b9: can act as a V.
    bool isDominantShaped (ChordQuality q)
    {
        const auto s = shapeOf (q);
        return q == ChordQuality::Major || (s.dominant && s.triad == ChordQuality::Major);
    }

    // 7ths / 9ths / add9 / 6 chords that are not dominant-type or diminished: the "keep the colour going" family.
    bool isColourful (const Shape& s)
    {
        return (s.ext == Ext::Seventh || s.ext == Ext::Colour) && ! s.dominant
            && (s.triad == ChordQuality::Major || s.triad == ChordQuality::Minor);
    }

    bool isMinorKey (const Key& key) { return scaleInfo (key.scale).isMinorFamily; }

    // Function of a chord, inheriting the function of its core / triad when the full colour chord is chromatic.
    HarmonicFunction functionOf (const Chord& c, const Key& key)
    {
        auto f = harmonicFunction (c, key);
        if (f != HarmonicFunction::Chromatic)
            return f;

        const auto s = shapeOf (c.quality);
        if (s.core != c.quality)
        {
            f = harmonicFunction ({ c.root, s.core }, key);
            if (f != HarmonicFunction::Chromatic)
                return f;
        }
        if (s.triad != c.quality && s.triad != s.core)
            f = harmonicFunction ({ c.root, s.triad }, key);
        return f;
    }

    bool isDiatonicCore (const Chord& c, const Key& ctx)
    {
        const auto s = shapeOf (c.quality);
        return isDiatonic (c, ctx) || isDiatonic ({ c.root, s.core }, ctx) || isDiatonic ({ c.root, s.triad }, ctx);
    }

    // The scale the other minor-family scales borrow from first (natural minor), or major for Lydian / Mixolydian.
    // Returns the key itself for Major and Natural Minor.
    Key relatedKey (const Key& key)
    {
        if (key.scale == ScaleType::Major || key.scale == ScaleType::NaturalMinor)
            return key;
        return { key.tonic, isMinorKey (key) ? ScaleType::NaturalMinor : ScaleType::Major };
    }

    //==========================================================================
    float rootMotion (int fromRoot, int toRoot)
    {
        //                                    0      +1     +2     +3     +4     +5     tt     +7     +8     +9     +10    +11
        static const float byInterval[12] = { 0.30f, 0.45f, 0.65f, 0.55f, 0.45f, 1.00f, 0.10f, 0.80f, 0.70f, 0.70f, 0.65f, 0.40f };
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

    // The plain triad a chord is heard as: its own triad, or for sus chords the key's diatonic triad on that root
    // (Csus2 in C major shares tones like C, not like a bare fifth).
    Chord plainTriad (const Chord& c, const Key& key)
    {
        const auto s = shapeOf (c.quality);
        if (s.ext == Ext::Sus && key.contains (c.root))
        {
            const auto t = diatonicChord (key, key.degreeOf (c.root), false);
            if (t.quality == ChordQuality::Major || t.quality == ChordQuality::Minor)
                return t;
        }
        return { c.root, s.triad };
    }

    // Degree-table motion between two roots in `ctx`, or -1 when either root is outside ctx's scale or they coincide.
    float degreeMotion (const Key& ctx, int fromRoot, int toRoot)
    {
        const int df = ctx.degreeOf (fromRoot);
        const int dt = ctx.degreeOf (toRoot);
        if (df < 0 || dt < 0 || df == dt)
            return -1.0f;
        return tableValue (ctx.scale, df, dt);
    }

    float sameRootScore (const Chord& a, const Chord& b, HarmonicFunction toFunction)
    {
        const auto sa = shapeOf (a.quality);
        const auto sb = shapeOf (b.quality);
        const bool sameThird = sa.triad == sb.triad;

        if (sa.ext == Ext::Sus && sb.ext != Ext::Sus)                return 0.75f;   // Csus4 -> C, G7sus4 -> G7
        if (toFunction == HarmonicFunction::Secondary)                return 0.60f;   // C -> C7 (V7/IV)
        if (sameThird && sa.ext != Ext::Triad && sb.ext == Ext::Triad) return 0.72f;  // Am7 -> Am, Cadd9 -> C
        if (sameThird && sa.ext != Ext::Triad)                        return 0.50f;   // Fmaj7 -> Fmaj9, Am7 -> Am9
        if ((isMajorish (a.quality) && isMinorish (b.quality)) || (isMinorish (a.quality) && isMajorish (b.quality)))
            return 0.45f;                                                              // C <-> Cm
        if (sb.ext == Ext::Sus)                                       return 0.45f;   // C -> Csus4
        return 0.35f;                                                                  // C -> Cmaj7 / Cadd9 ...
    }

    // Functional motion from a to b in the contexts described in the header.
    float baseMotion (const Chord& a, const Chord& b, const Key& key, HarmonicFunction fromFunction, HarmonicFunction toFunction)
    {
        if (a.root == b.root)
            return sameRootScore (a, b, toFunction);

        const Key related = relatedKey (key);
        const Key par = key.parallel();
        const bool hasRelated = related != key;

        const float inKey = degreeMotion (key, a.root, b.root);
        const float inRelated = hasRelated ? degreeMotion (related, a.root, b.root) : -1.0f;
        const float inParallel = degreeMotion (par, a.root, b.root);

        if (fromFunction == HarmonicFunction::Borrowed)
        {
            // Read the move in the scale the chord was borrowed from.
            float best = -1.0f;
            if (isDiatonicCore (a, par))                  best = std::max (best, inParallel);
            if (hasRelated && isDiatonicCore (a, related)) best = std::max (best, inRelated);
            if (best < 0.0f)                              best = std::max (inParallel, inRelated * kRelatedContext);
            if (inKey >= 0.0f)                            best = std::max (best, inKey * kKeyContextFromBorrowed);
            if (best >= 0.0f)
                return best;
        }
        else
        {
            if (inKey >= 0.0f)      return inKey;
            if (inRelated >= 0.0f)  return inRelated * kRelatedContext;
            if (inParallel >= 0.0f) return inParallel * kParallelContext;
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

        const Key related = relatedKey (key);
        const float inRelated = related != key ? degreeMotion (related, a.root, targetRoot) : -1.0f;
        const float inParallel = degreeMotion (key.parallel(), a.root, targetRoot);
        const float best = std::max ({ inKey, inRelated, inParallel });
        return best >= 0.0f ? best : 0.3f;
    }

    // Factor for moving from a diatonic chord to a borrowed one (see header).
    float borrowedTargetFactor (const Chord& b, const Key& key)
    {
        const int fb = pc (b.root - key.tonic);
        const bool major = isMajorish (b.quality), minor = isMinorish (b.quality);

        if (! isMinorKey (key))
        {
            if (fb == 5 && minor)                       return 0.70f;   // iv
            if ((fb == 8 || fb == 10) && major)         return 0.68f;   // bVI, bVII
            if (fb == 0 && minor)                       return 0.60f;   // i
            return 0.50f;
        }

        const Key related = relatedKey (key);
        if (related != key && isDiatonicCore (b, related))
            return 0.92f;                                                // natural-minor chord in harmonic minor etc.
        if (fb == 5 && major)                           return 0.60f;   // Dorian IV in natural minor
        return 0.50f;
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
        const bool aMaj = isMajorish (qa), aMin = isMinorish (qa), bMaj = isMajorish (qb), bMin = isMinorish (qb);
        const bool chain = isColourful (shapeOf (qa)) && isColourful (shapeOf (qb));

        float best = 0.0f;
        auto offer = [&best] (bool cond, float v) { if (cond) best = std::max (best, v); };

        if (isDiminishedFamily (qa) && pc (b.root - a.root) == 1 && ! isDiminishedFamily (qb))
            offer (true, fb == 0 ? 0.95f : 0.85f);   // leading-tone diminished resolving up a half step

        if (! isMinorKey (key))
        {
            offer (fa == 8 && aMaj && fb == 10 && bMaj, 0.95f);              // bVI -> bVII
            offer (fa == 10 && aMaj && fb == 0 && bMaj, 0.95f);              // bVII -> I (incl. backdoor bVII7)
            offer (fa == 5 && aMin && fb == 0 && bMaj, 0.92f);               // iv -> I
            offer (fa == 5 && fb == 5 && aMaj && bMin, 0.85f);               // IV -> iv
            offer (fa == 8 && aMaj && fb == 7 && isDominantShaped (qb), 0.85f); // bVI -> V
            offer (fa == 10 && aMaj && fb == 5 && bMaj, 0.80f);              // bVII -> IV
            offer (fa == 0 && aMin && fb == 8 && bMaj, 0.80f);               // i -> bVI
            offer (fa == 7 && aMin && fb == 8 && bMaj, 0.80f);               // v -> bVI
            offer (fa == 5 && aMin && fb == 8 && bMaj, 0.75f);               // iv -> bVI
            offer (fa == 3 && aMaj && fb == 5, 0.75f);                       // bIII -> IV / iv
            offer (fa == 8 && aMaj && fb == 0 && bMaj, 0.72f);               // bVI -> I
            offer (fa == 10 && aMaj && fb == 8 && bMaj, 0.70f);              // bVII -> bVI
            offer (fa == 8 && aMaj && fb == 5 && bMin, 0.70f);               // bVI -> iv

            if (chain)
            {
                offer (fa == 0 && aMaj && fb == 2 && bMin, 0.75f);           // Imaj7 -> ii7
                offer (fa == 4 && aMin && fb == 0 && bMaj, 0.75f);           // iii7 -> Imaj9
                offer (fa == 5 && aMaj && fb == 4 && bMin, 0.72f);           // IVmaj7 -> iii7
                offer (fa == 2 && aMin && fb == 4 && bMin, 0.65f);           // ii7 -> iii7
                offer (fa == 0 && aMaj && fb == 4 && bMin, 0.62f);           // Imaj9 -> iii7
            }
        }
        else
        {
            offer (fa == 1 && aMaj && fb == 0, 0.90f);                       // bII -> i (Phrygian cadence)
            offer (fa == 1 && aMaj && fb == 7 && isDominantShaped (qb), 0.85f); // Neapolitan bII -> V
            offer (fa == 8 && aMaj && fb == 7 && isDominantShaped (qb), 0.80f); // VI -> V (Andalusian)
            offer (fa == 5 && aMaj && fb == 0 && bMin, 0.75f);               // IV -> i (Dorian)
            offer (fa == 7 && isDominantShaped (qa) && fb == 0 && bMaj, 0.70f); // V -> I (Picardy)
            offer (fa == 0 && aMin && fb == 5 && bMaj, 0.58f);               // i -> IV (Dorian borrowing)

            if (chain)
            {
                offer (fa == 8 && aMaj && fb == 7 && bMin, 0.95f);           // VImaj7 -> v7 (lo-fi favourite)
                offer (fa == 3 && aMaj && fb == 5 && bMin, 0.90f);           // IIImaj7 -> iv7
                offer (fa == 7 && aMin && fb == 3 && bMaj, 0.80f);           // v7 -> IIImaj9
            }
        }

        return best;
    }

    // Extension factor E (see header).
    float extensionFactor (const Shape& sa, const Shape& sb)
    {
        if (sb.ext == Ext::Triad)
            return sb.triad == ChordQuality::Augmented ? kAugmentedFactor : 1.0f;
        if (isColourful (sa) && isColourful (sb))
            return sb.exotic ? 0.97f : 1.0f;
        if (sb.exotic)
            return kExoticFactor;
        return sb.ext == Ext::Seventh ? kSeventhFactor : kColourFactor;
    }
} // namespace

float transitionScore (const Chord& fromChord, const Chord& toChord, const Key& key)
{
    const Chord a { pc (fromChord.root), fromChord.quality };
    const Chord b { pc (toChord.root), toChord.quality };
    if (a == b)
        return 0.0f;

    const auto fa = functionOf (a, key);
    const auto fb = functionOf (b, key);
    const auto sa = shapeOf (a.quality);
    const auto sb = shapeOf (b.quality);

    float functional = 0.0f;
    bool resolution = false;

    if (fa == HarmonicFunction::Secondary)
    {
        if (b.root == pc (a.root + 5))
        {
            functional = isDiatonicCore (b, key) ? 1.0f : 0.7f;
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

        // V -> I: the dominant resolution.
        if (key.degreeOf (a.root) == 4 && scaleInfo (key.scale).offsets[4] == 7 && b.root == pc (key.tonic)
            && fa == HarmonicFunction::Dominant)
        {
            if (isDominantShaped (a.quality))
            {
                functional = std::min (1.0f, functional + kDominantBonus);
                resolution = true;
            }
            else if (sa.triad == ChordQuality::Sus4)
            {
                functional = std::min (1.0f, functional + kSusDominantBonus);
            }
        }

        // Minor keys: the major V (leading tone) is a stronger destination than the tables' modal v (but straight
        // from the tonic the data prefers the modal v).
        if (isMinorKey (key) && isDominantShaped (b.quality) && key.degreeOf (b.root) == 4
            && scaleInfo (key.scale).offsets[4] == 7 && a.root != pc (key.tonic))
            functional = std::min (1.0f, functional + kMinorVBonus);

        switch (fb)
        {
            case HarmonicFunction::Borrowed:
                functional *= (fa == HarmonicFunction::Borrowed ? kBorrowedFromBorrowed : borrowedTargetFactor (b, key));
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

    const bool colourChain = isColourful (sa) && isColourful (sb);

    float score = kWeightFunctional * functional
                + kWeightRoot * rootMotion (a.root, b.root)
                + kWeightShared * sharedTones (plainTriad (a, key), plainTriad (b, key))
                + (resolution ? kResolutionBonus : 0.0f)
                + (colourChain ? kColourChainBonus : 0.0f);

    score = std::clamp (score, 0.0f, 1.0f) * extensionFactor (sa, sb);
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

float openingScore (const Chord& chord, const Key& key)
{
    const Chord c { pc (chord.root), chord.quality };
    const auto s = shapeOf (c.quality);
    const auto f = functionOf (c, key);
    const int off = pc (c.root - key.tonic);
    const bool minorKey = isMinorKey (key);
    const bool maj = s.triad == ChordQuality::Major, min = s.triad == ChordQuality::Minor;
    const auto tonicTriad = shapeOf (diatonicChord (key, 0, false).quality).triad;

    if (f == HarmonicFunction::Secondary)
        return 0.12f;
    if (f == HarmonicFunction::Chromatic)
        return 0.08f;
    if (s.triad == ChordQuality::Diminished || s.triad == ChordQuality::Augmented)
        return off != 0 ? 0.1f : (s.triad == tonicTriad ? 0.8f : 0.35f);   // Locrian's i° is still home

    float base = 0.3f;
    if (off == 0)
    {
        if (s.triad == tonicTriad)    base = 1.0f;
        else if (s.ext == Ext::Sus)   base = 0.8f;
        else                          base = minorKey ? 0.3f : 0.5f;   // Picardy I in minor / borrowed i in major
    }
    else if (minorKey)
    {
        if      (off == 8 && maj)  base = 0.78f;                                          // VI
        else if (off == 5 && min)  base = 0.50f;                                          // iv
        else if (off == 5 && maj)  base = key.scale == ScaleType::Dorian ? 0.70f : 0.30f; // IV
        else if (off == 3 && maj)  base = 0.45f;                                          // III
        else if (off == 10 && maj) base = 0.42f;                                          // VII
        else if (off == 1 && maj)  base = key.scale == ScaleType::Phrygian ? 0.60f : 0.20f; // bII
        else if (off == 7 && min)  base = 0.32f;                                          // v
        else if (off == 7 && maj)  base = 0.25f;                                          // V
        else if (off == 2 && min)  base = 0.30f;                                          // Dorian ii
        else                       base = 0.20f;
    }
    else
    {
        if      (off == 9 && min)  base = 0.72f;                                           // vi
        else if (off == 5 && maj)  base = 0.70f;                                           // IV
        else if (off == 8 && maj)  base = 0.62f;                                           // bVI
        else if (off == 2 && min)  base = 0.48f;                                           // ii
        else if (off == 2 && maj)  base = key.scale == ScaleType::Lydian ? 0.60f : 0.25f;  // Lydian II
        else if (off == 7 && maj)  base = 0.45f;                                           // V
        else if (off == 10 && maj) base = key.scale == ScaleType::Mixolydian ? 0.60f : 0.45f; // bVII
        else if (off == 5 && min)  base = 0.40f;                                           // iv
        else if (off == 4 && min)  base = 0.35f;                                           // iii
        else if (off == 3 && maj)  base = 0.35f;                                           // bIII
        else if (off == 7 && min)  base = 0.30f;                                           // Mixolydian v
        else                       base = 0.20f;
    }

    // Extensions: the data's minor first chords favour VImaj7 over plain VI, and i7 is the second-commonest opener.
    float ext = 1.0f;
    if (s.ext != Ext::Triad)
    {
        if (off == 0)
            ext = s.ext == Ext::Sus ? 1.0f : (s.ext == Ext::Seventh ? 0.88f : 0.85f);
        else if (minorKey && off == 8 && maj)
            ext = c.quality == ChordQuality::Major7 ? 1.03f : 0.92f;
        else
            ext = s.ext == Ext::Seventh ? 0.95f : 0.9f;
        if (s.exotic || s.dominant)
            ext *= 0.85f;
    }

    if (f == HarmonicFunction::Borrowed && ! (off == 8 || off == 10 || off == 5 || off == 0))
        base *= 0.8f;
    else if (f == HarmonicFunction::Borrowed && off != 0 && key.scale != ScaleType::Major && key.scale != ScaleType::NaturalMinor)
        base *= 0.85f;   // a mode's own signature chords open before what it borrows (D Dorian: G before Bb)

    return std::clamp (base * ext, 0.0f, 1.0f);
}

} // namespace cb
