#pragma once

// Private to MelodyGen.cpp (owner: WP1). constexpr transcriptions of the generator parameters (GP sections 1.3-1.6:
// style params, templates, cells, slot tables, pCT, step weights, leap recovery, contour, NCT, forms, relations, gap
// plan, cadence and key-relative rows, gate classes) and the factors of the final-spec 5.4.4 rules. All tuning happens
// in this file. Only aggregate weights and generic shapes cleared in GP may appear here: never a melody, file name or
// any other material from the corpus (final-spec 8.5 privacy review).
//
// Every per-style table is indexed by MelodyStyle (Uplifting, Lyrical, Driving, Sparse). Values GP tags [D] are design
// choices; fields documented as "tuned" were set against the CoreTests bands (GP 1.8).

#include <cstdint>

namespace cb
{
namespace melodygen
{

constexpr int kStyles = 4;

// 16-character rhythm masks ('1' = onset on that 16th) -> bit i = step i.
constexpr std::uint16_t mask16 (const char* s)
{
    std::uint16_t m = 0;
    for (int i = 0; i < 16 && s[i] != 0; ++i)
        if (s[i] == '1')
            m = (std::uint16_t) (m | (1u << i));
    return m;
}

// 4-character beat cells -> bit i = 16th i of the beat.
constexpr std::uint8_t mask4 (const char* s)
{
    std::uint8_t m = 0;
    for (int i = 0; i < 4 && s[i] != 0; ++i)
        if (s[i] == '1')
            m = (std::uint8_t) (m | (1u << i));
    return m;
}

//==============================================================================
// Metric position classes (GP 0): down = step 0, beat3 = 8, beat2/4 = 4 / 12, off8 = 2 / 6 / 10 / 14,
// e16 = 1 / 5 / 9 / 13, a16 = 3 / 7 / 11 / 15.
enum PosClass { kDown = 0, kBeat3, kBeat24, kOff8, kE16, kA16, kNumPos };

constexpr int posClassOfStep (int stepInBar)
{
    return stepInBar == 0 ? kDown
         : stepInBar == 8 ? kBeat3
         : (stepInBar == 4 || stepInBar == 12) ? kBeat24
         : (stepInBar % 4 == 2) ? kOff8
         : (stepInBar % 4 == 1) ? kE16
                                : kA16;
}

//==============================================================================
// Style presets (GP 1.6, 1.3.1, 1.3.5, 1.4.1, 1.4.2, 1.4.3, 1.4.4, 1.4.6, 1.5.1, 1.5.3, 1.5.7).
struct StyleParams
{
    float npbLo, npbDef, npbHi;       // notes per active bar at density 0 / 0.5 / 1 (GP 1.3.1)
    float npbCompensation;            // tuned: added to the rhythm target to make up for gap / copy losses
    bool cellMode;                    // false = k-of-16 slot draw (Sparse)
    float templateProb;
    float odd16Cap;
    float spread;                     // slot mode: per-bar spread of k (triangular variate)
    float pRepeat;
    float pCT[kNumPos];               // down, beat3, beat2/4, off8, e16, a16
    float stepUp[8], stepDown[8];     // scale steps 1..7, 8+ (moving only)
    float kRevSmall, kRevLarge;       // leap recovery: opposite direction after a 5-7 / >= 8 leap
    float sameSmall, sameLarge;       // same direction >= 5 semitones after a 5-7 / >= 8 leap
    float gapFill;                    // extra factor for opposite 1-2 semitone candidates after a leap
    int maxLeap;                      // largest interval in semitones
    float unit2Bars;                  // P(unit = 2 bars)
    float continuous;                 // P(the loop is one continuous phrase)
    int regC, regLo, regHi;           // register window around the median voicing top
    float phraseSpan;                 // contour target span (semitones) of a phrase of up to 4 bars
    float phraseSpan8;                // ... of an 8-bar phrase
    float gateW[4];                   // staccato, half, detached, legato
    float pedalMode;                  // Driving pedal-alternation probability per melody
    float pCopyARhythm;               // a new letter copies the rhythm of A
    float pLongHold;                  // a down / beat3 note before an empty beat sustains to the next onset
    bool allSustain;                  // Sparse: every note sustains to the next onset
    float stepBias;                   // tuned: extra factor on 1-2 semitone moves (balances the chord-tone pull)
    float nctBoost;                   // tuned: subtracted from pCT off the strong positions (constructions that
                                      // cannot resolve fall back to chord tones, so the raw pCT overshoots)
    float skipFactor;                 // tuned: factor on 3-4 semitone moves (chord-tone picks overshoot thirds)
    float strongOffset;               // tuned: subtracted from pCT on down / beat3 (the first note, the cadences and
                                      // the copies already add chord tones there)
};

constexpr StyleParams kStyleParams[kStyles] = {
    // Uplifting lead (ext mid)
    { 4.5f, 5.8f, 7.5f, 0.35f, true, 0.25f, 0.25f, 1.6f, 0.38f,
      { .75f, .74f, .65f, .64f, .67f, .62f },
      { .223f, .091f, .067f, .051f, .029f, .007f, .015f, .030f }, { .196f, .110f, .059f, .046f, .024f, .007f, .014f, .032f },
      2.5f, 3.5f, 0.35f, 0.2f, 1.0f, 19,
      0.6f, 0.79f, 0, -5, 7, 7.0f, 10.0f, { .21f, .41f, .08f, .30f }, 0.0f, 0.59f, 0.0f, false, 2.0f, 0.10f, 0.5f, 0.06f },
    // Lyrical hook (ext slow)
    { 2.0f, 3.4f, 4.5f, 0.25f, true, 0.40f, 0.08f, 1.4f, 0.28f,
      { .77f, .65f, .63f, .66f, .59f, .64f },
      { .251f, .106f, .055f, .032f, .019f, .007f, .012f, .007f }, { .263f, .127f, .059f, .032f, .016f, .006f, .004f, .004f },
      2.5f, 3.5f, 0.35f, 0.2f, 1.5f, 14,
      0.5f, 0.52f, 1, -5, 8, 7.0f, 10.0f, { .13f, .32f, .10f, .45f }, 0.0f, 0.37f, 0.5f, false, 4.5f, 0.12f, 0.5f, 0.06f },
    // Driving riff (ext fast)
    { 8.0f, 10.9f, 16.0f, 1.0f, true, 0.34f, 0.46f, 3.0f, 0.45f,
      { .81f, .83f, .75f, .77f, .77f, .75f },
      { .096f, .055f, .038f, .062f, .023f, .016f, .064f, .170f }, { .078f, .051f, .033f, .040f, .021f, .016f, .052f, .184f },
      2.5f, 3.5f, 0.6f, 0.45f, 1.0f, 24,
      0.2f, 0.92f, -2, -14, 7, 12.5f, 15.0f, { .10f, .33f, .18f, .39f }, 0.5f, 0.77f, 0.0f, false, 6.0f, 0.12f, 1.0f, 0.04f },
    // Sparse ([X-tail] rhythm, [D] pitch / register / gate)
    { 1.0f, 1.5f, 2.5f, 0.1f, false, 0.65f, 0.05f, 0.5f, 0.22f,
      { .85f, .80f, .70f, .65f, .60f, .60f },
      { .24f, .12f, .06f, .03f, .015f, .005f, .01f, 0.f }, { .26f, .13f, .06f, .03f, .015f, .005f, .005f, 0.f },
      2.5f, 3.5f, 0.35f, 0.2f, 1.5f, 12,
      0.7f, 0.20f, 3, -4, 9, 7.0f, 10.0f, { .05f, .15f, .10f, .70f }, 0.0f, 0.37f, 0.5f, true, 2.0f, 0.08f, 1.0f, 0.02f },
};

// [D] Register anchor floor: the window sits over max (median voicing top, this). Chordbloom's default voicings in
// minor keys top out at 62-65, which put the Uplifting lead at B3-B4 inside the pad; the corpus lead median is 70.
// 0 = no floor (Lyrical and Sparse already sit above the voicing top).
constexpr int kMinVoicingTop[kStyles] = { 67, 0, 67, 0 };

//==============================================================================
// Whole-bar templates (GP 1.3.2; every mask occurs in >= 20 distinct files). The pickup masks are not listed: the
// plan places pickups itself (GP: only in the bar before a phrase start).
struct BarTemplate
{
    std::uint16_t mask;
    float weight;
};

constexpr BarTemplate kTemplatesUplifting[] = {
    { mask16 ("1001001001001010"), .054f }, { mask16 ("1001001010010010"), .050f }, { mask16 ("1010101010101010"), .035f },
    { mask16 ("1000100010001000"), .025f }, { mask16 ("0010001000100010"), .025f }, { mask16 ("1001001001001001"), .016f },
    { mask16 ("0010010010010010"), .016f }, { mask16 ("1000100010001010"), .013f }, { mask16 ("1000000000000000"), .013f },
};
constexpr BarTemplate kTemplatesLyrical[] = {
    { mask16 ("1000000000000000"), .146f }, { mask16 ("0010001000100010"), .085f }, { mask16 ("1000000010000000"), .047f },
    { mask16 ("1000100010001000"), .042f }, { mask16 ("1000001000001000"), .040f }, { mask16 ("1000000000001000"), .015f },
    { mask16 ("1000000000001010"), .009f },
};
constexpr BarTemplate kTemplatesDriving[] = {
    { mask16 ("1111111111111111"), .189f }, { mask16 ("1010101010101010"), .151f },
};
constexpr BarTemplate kTemplatesSparse[] = {
    { mask16 ("1000000000000000"), .40f }, { mask16 ("1000000010000000"), .135f }, { mask16 ("1000001000000000"), .049f },
    { mask16 ("1000000000001000"), .039f },
};

struct TemplateList
{
    const BarTemplate* items;
    int size;
};

constexpr TemplateList kTemplates[kStyles] = {
    { kTemplatesUplifting, (int) (sizeof (kTemplatesUplifting) / sizeof (BarTemplate)) },
    { kTemplatesLyrical, (int) (sizeof (kTemplatesLyrical) / sizeof (BarTemplate)) },
    { kTemplatesDriving, (int) (sizeof (kTemplatesDriving) / sizeof (BarTemplate)) },
    { kTemplatesSparse, (int) (sizeof (kTemplatesSparse) / sizeof (BarTemplate)) },
};

//==============================================================================
// Cell mode (GP 1.3.3): per beat the top-8 onset cells (renormalised when drawn). Sparse uses slot mode; its row is
// Lyrical's and only serves as a fallback.
struct Cell
{
    std::uint8_t mask;
    float weight;
};

constexpr Cell kCells[kStyles][4][8] = {
    // Uplifting
    { { { mask4 ("1001"), .24f }, { mask4 ("1000"), .23f }, { mask4 ("0010"), .21f }, { mask4 ("1010"), .21f },
        { mask4 ("0000"), .04f }, { mask4 ("0100"), .03f }, { mask4 ("0011"), .03f }, { mask4 ("0101"), .02f } },
      { { mask4 ("0010"), .36f }, { mask4 ("1010"), .24f }, { mask4 ("1000"), .15f }, { mask4 ("1001"), .10f },
        { mask4 ("0000"), .06f }, { mask4 ("0100"), .06f }, { mask4 ("1011"), .02f }, { mask4 ("0101"), .02f } },
      { { mask4 ("0010"), .27f }, { mask4 ("1010"), .17f }, { mask4 ("1001"), .16f }, { mask4 ("0100"), .13f },
        { mask4 ("1000"), .13f }, { mask4 ("0000"), .09f }, { mask4 ("0101"), .04f }, { mask4 ("1011"), .02f } },
      { { mask4 ("1010"), .34f }, { mask4 ("0010"), .26f }, { mask4 ("1000"), .14f }, { mask4 ("0000"), .10f },
        { mask4 ("0100"), .06f }, { mask4 ("1001"), .04f }, { mask4 ("0110"), .03f }, { mask4 ("0101"), .02f } } },
    // Lyrical
    { { { mask4 ("1000"), .47f }, { mask4 ("0000"), .21f }, { mask4 ("0010"), .18f }, { mask4 ("1010"), .08f },
        { mask4 ("1001"), .04f }, { mask4 ("0001"), .01f }, { mask4 ("0011"), .01f }, { mask4 ("0100"), .01f } },
      { { mask4 ("0000"), .46f }, { mask4 ("0010"), .27f }, { mask4 ("1000"), .14f }, { mask4 ("1010"), .09f },
        { mask4 ("1001"), .01f }, { mask4 ("0100"), .01f }, { mask4 ("0001"), .005f }, { mask4 ("1100"), .005f } },
      { { mask4 ("0000"), .44f }, { mask4 ("1000"), .24f }, { mask4 ("0010"), .21f }, { mask4 ("1010"), .06f },
        { mask4 ("1001"), .02f }, { mask4 ("0001"), .02f }, { mask4 ("0100"), .01f }, { mask4 ("0101"), .01f } },
      { { mask4 ("0000"), .40f }, { mask4 ("0010"), .23f }, { mask4 ("1000"), .20f }, { mask4 ("1010"), .14f },
        { mask4 ("0100"), .01f }, { mask4 ("0001"), .01f }, { mask4 ("0011"), .01f }, { mask4 ("0101"), .005f } } },
    // Driving
    { { { mask4 ("1111"), .30f }, { mask4 ("1010"), .25f }, { mask4 ("1011"), .15f }, { mask4 ("1101"), .13f },
        { mask4 ("1110"), .08f }, { mask4 ("1001"), .04f }, { mask4 ("0111"), .03f }, { mask4 ("0101"), .02f } },
      { { mask4 ("1111"), .26f }, { mask4 ("1010"), .21f }, { mask4 ("1011"), .15f }, { mask4 ("0110"), .14f },
        { mask4 ("1110"), .08f }, { mask4 ("1101"), .06f }, { mask4 ("0011"), .05f }, { mask4 ("0111"), .04f } },
      { { mask4 ("1111"), .28f }, { mask4 ("1010"), .21f }, { mask4 ("1011"), .12f }, { mask4 ("1101"), .10f },
        { mask4 ("0110"), .10f }, { mask4 ("1110"), .08f }, { mask4 ("0101"), .06f }, { mask4 ("0111"), .05f } },
      { { mask4 ("1010"), .30f }, { mask4 ("1111"), .29f }, { mask4 ("0110"), .12f }, { mask4 ("1110"), .09f },
        { mask4 ("1011"), .05f }, { mask4 ("1101"), .05f }, { mask4 ("0111"), .05f }, { mask4 ("0011"), .04f } } },
    // Sparse (fallback only; = Lyrical)
    { { { mask4 ("1000"), .47f }, { mask4 ("0000"), .21f }, { mask4 ("0010"), .18f }, { mask4 ("1010"), .08f },
        { mask4 ("1001"), .04f }, { mask4 ("0001"), .01f }, { mask4 ("0011"), .01f }, { mask4 ("0100"), .01f } },
      { { mask4 ("0000"), .46f }, { mask4 ("0010"), .27f }, { mask4 ("1000"), .14f }, { mask4 ("1010"), .09f },
        { mask4 ("1001"), .01f }, { mask4 ("0100"), .01f }, { mask4 ("0001"), .005f }, { mask4 ("1100"), .005f } },
      { { mask4 ("0000"), .44f }, { mask4 ("1000"), .24f }, { mask4 ("0010"), .21f }, { mask4 ("1010"), .06f },
        { mask4 ("1001"), .02f }, { mask4 ("0001"), .02f }, { mask4 ("0100"), .01f }, { mask4 ("0101"), .01f } },
      { { mask4 ("0000"), .40f }, { mask4 ("0010"), .23f }, { mask4 ("1000"), .20f }, { mask4 ("1010"), .14f },
        { mask4 ("0100"), .01f }, { mask4 ("0001"), .01f }, { mask4 ("0011"), .01f }, { mask4 ("0101"), .005f } } },
};

// Slot mode (GP 1.3.4): onset probability per step, by the bucket of the bar's onset count k (relative weights).
constexpr float kSlotWeights[5][16] = {
    { .69f, .00f, .07f, .01f, .06f, .00f, .09f, .00f, .19f, .00f, .04f, .01f, .12f, .01f, .12f, .03f },   // 1-2
    { .48f, .04f, .38f, .06f, .32f, .02f, .49f, .04f, .35f, .03f, .40f, .07f, .43f, .03f, .46f, .02f },   // 3-4
    { .69f, .08f, .44f, .32f, .47f, .09f, .64f, .13f, .45f, .21f, .44f, .22f, .56f, .10f, .67f, .08f },   // 5-6
    { .80f, .18f, .71f, .30f, .75f, .22f, .76f, .31f, .67f, .24f, .71f, .32f, .66f, .24f, .82f, .19f },   // 7-9
    { .92f, .78f, .81f, .84f, .72f, .76f, .92f, .70f, .81f, .83f, .82f, .79f, .76f, .82f, .89f, .67f },   // 10+
};

constexpr int slotBucket (int k) { return k <= 2 ? 0 : k <= 4 ? 1 : k <= 6 ? 2 : k <= 9 ? 3 : 4; }

// [D] tuned: density above the default (0.5). Sparse's whole-bar templates hold at most 2 onsets and Lyrical's at
// most 4, so with them a Busy bar cannot reach GP 1.3.1's npbHi: above 0.5 the template share fades by this fraction at
// density 1 (linearly from 0.5) and the slot / cell draws, which follow the target, carry Busy. A gain on the target's
// distance above npbDef makes up for the gap / copy losses there (Driving saturates near 16 onsets a bar and
// overshoots at gain 1). Density 0.5 and below are unchanged.
constexpr float kTemplateFadeBusy[kStyles] = { 0.0f, 1.0f, 0.0f, 1.0f };
constexpr float kNpbGainBusy[kStyles] = { 1.0f, 1.4f, 0.85f, 1.25f };

// [D] Short loops (<= 2 bars): a gap takes at most a quarter of its unit (at least 1 beat), and a new Sparse / Lyrical
// unit keeps at least this many onsets in its sounding part (missing ones are added on the 8th grid; 3 when the unit
// is the whole loop, so a note sits between the phrase start and the loop end, which the seam and resolveEnding pull
// back to the first note). Otherwise most short Sparse loops were one held note or a repeated tonic, a few dozen
// outcomes in all, and seed + 1 often gave the same melody (final-spec 9 WP1 determinism: >= 95% differ).
constexpr int kShortLoopMaxBeats = 8;
constexpr int kShortLoopMinOnsets = 2;
constexpr int kShortLoopMinOnsetsOneUnit = 3;

constexpr float kOddCapSlack = 0.05f;     // GP 1.3.5: reject a draw that pushes the running share above cap + 0.05
constexpr float kOddPriorOnsets = 8.0f;   // [D] pseudo-onsets (at the cap share) that start each unit's running share
constexpr double kDensityTiltRange = 4.0; // GP 1.3.3: g in [-4, 4]

//==============================================================================
// Forms (GP 1.5.1): letters per unit of a 4-unit block, "other" spread uniformly over ABCA, AABB, ABBA, AABA.
struct FormWeight
{
    const char* letters;
    float weight;
};

constexpr int kMaxForms = 12;

struct FormTable
{
    FormWeight forms[kMaxForms];
    int size;
};

// [style][0] = 4 x 1-bar units, [style][1] = 4 x 2-bar units. Sparse 1-bar = Lyrical.
constexpr FormTable kForms4[kStyles][2] = {
    { { { { "AAAA", .37f }, { "ABAB", .17f }, { "ABCD", .11f }, { "ABAC", .07f }, { "AAAB", .07f }, { "ABBB", .03f },
          { "ABCB", .03f }, { "ABCC", .02f }, { "ABCA", .0325f }, { "AABB", .0325f }, { "ABBA", .0325f }, { "AABA", .0325f } }, 12 },
      { { { "AAAA", .47f }, { "ABAB", .17f }, { "ABCD", .07f }, { "AAAB", .06f }, { "ABBB", .06f }, { "ABAC", .04f },
          { "ABCA", .0325f }, { "AABB", .0325f }, { "ABBA", .0325f }, { "AABA", .0325f } }, 10 } },
    { { { { "AAAA", .26f }, { "ABCD", .18f }, { "ABAB", .14f }, { "ABAC", .11f }, { "AAAB", .06f },
          { "ABCA", .0625f }, { "AABB", .0625f }, { "ABBA", .0625f }, { "AABA", .0625f } }, 9 },
      { { { "AAAA", .32f }, { "ABAB", .24f }, { "ABCD", .14f }, { "ABAC", .08f }, { "AAAB", .05f },
          { "ABCA", .0425f }, { "AABB", .0425f }, { "ABBA", .0425f }, { "AABA", .0425f } }, 9 } },
    { { { { "AAAA", .72f }, { "ABAB", .09f }, { "AAAB", .04f },
          { "ABCA", .0375f }, { "AABB", .0375f }, { "ABBA", .0375f }, { "AABA", .0375f } }, 7 },
      { { { "AAAA", .76f }, { "ABAB", .05f }, { "ABBB", .05f }, { "AAAB", .04f },
          { "ABCA", .025f }, { "AABB", .025f }, { "ABBA", .025f }, { "AABA", .025f } }, 8 } },
    { { { { "AAAA", .26f }, { "ABCD", .18f }, { "ABAB", .14f }, { "ABAC", .11f }, { "AAAB", .06f },
          { "ABCA", .0625f }, { "AABB", .0625f }, { "ABBA", .0625f }, { "AABA", .0625f } }, 9 },
      { { { "AAAA", .32f }, { "ABAB", .24f }, { "ABCD", .14f }, { "ABAC", .08f },
          { "ABCA", .055f }, { "AABB", .055f }, { "ABBA", .055f }, { "AABA", .055f } }, 8 } },
};

constexpr float kForm2AA[kStyles] = { .56f, .38f, .82f, .38f };          // 2-unit loops: P(AA), else AB
constexpr float kSecondBlockCopy[kStyles] = { .69f, .58f, .81f, .58f };  // the second 4-unit block copies the first
constexpr float kRepetitionTilt = 4.0f;                                    // weight x exp (4 (r - 0.5) rep (form))
constexpr int kPlanUnits = 8;                                              // longer loops repeat the 8-unit plan

//==============================================================================
// What a repeated letter copies (GP 1.5.2; Sparse = Lyrical): exact, transposed, ending change, varied.
enum Relation { kRelExact = 0, kRelTransposed, kRelEnding, kRelVaried, kNumRelations };
constexpr float kRelations[kStyles][kNumRelations] = {
    { .25f, .05f, .11f, .58f }, { .30f, .11f, .09f, .50f }, { .21f, .13f, .07f, .59f }, { .30f, .11f, .09f, .50f },
};
// [D] tuned: P(a "varied" copy really changes 1-2 onsets of its last bar), else it keeps the rhythm and its pitches
// take the GP 1.5.4 treatment like any rhythm copy. GP 1.5.2's own lag-4 targets (.80 / .73 / .90 exact bar-rhythm
// repeats) leave room for only about this many changed bars next to its .50-.59 "varied" share.
constexpr float kVariedRhythmChange[kStyles] = { 0.35f, 0.35f, 0.25f, 0.35f };

// Pitch treatment of a rhythm-copied unit (GP 1.5.4; Sparse = Lyrical): exact, fit-transpose, ending change, re-pick.
// [style][0] = the chords under the copy changed, [style][1] = the same chords.
enum Treatment { kTreatExact = 0, kTreatFit, kTreatEnding, kTreatRepick, kNumTreatments };
constexpr float kTreatments[kStyles][2][kNumTreatments] = {
    { { .20f, .16f, .06f, .58f }, { .53f, .08f, .07f, .33f } },
    { { .26f, .37f, .02f, .34f }, { .53f, .33f, .01f, .13f } },
    { { .08f, .19f, .05f, .69f }, { .37f, .04f, .10f, .49f } },
    { { .26f, .37f, .02f, .34f }, { .53f, .33f, .01f, .13f } },
};
// GP 1.5.4: an exact copy over changed chords is kept only when this share of its down / beat3 notes stays chord
// tones (a strong-position R2 avoid tone counts -1); otherwise it becomes a fit-transpose.
constexpr float kExactAcceptChordToneShare = 0.5f;
// [D] Repetition (r) also tilts the copies (final-spec 5.4.2 amendment, review: Loop-like must be audible): the relation
// of a repeated letter (exact / transposed x exp (k (r - 0.5)), varied x exp (-k (r - 0.5))) and the GP 1.5.4 pitch
// treatment (exact x exp (k (r - 0.5)), re-pick x exp (-k (r - 0.5))). r = 0.5 keeps the corpus weights. Both draws
// come from the rhythm / pitch streams, so the plan signature is unaffected.
constexpr float kRelationTilt = 4.0f;
constexpr float kTreatmentTilt = 4.0f;
// [D] With a user-forced contour an exact copy whose place on the contour path lies this many semitones away from its
// prototype's becomes a fit-transpose, scored with kFitContourCost per semitone of missed path movement.
constexpr double kForcedExactMaxDrift = 1.0;
constexpr double kFitContourCost = 1.0;
constexpr int kFitShiftMax = 3;                      // GP 1.5.4: diatonic shifts -3..+3

//==============================================================================
// Phrase / gap plan (GP 1.5.3).
struct GapBucket
{
    int lo, hi;      // whole beats, inclusive
    float weight;
};

constexpr GapBucket kGapBuckets[kStyles][4] = {
    { { 1, 2, .64f }, { 2, 4, .19f }, { 4, 8, .09f }, { 8, 12, .08f } },
    { { 2, 4, .55f }, { 4, 8, .31f }, { 8, 12, .14f }, { 2, 2, 0.0f } },
    { { 1, 2, .69f }, { 2, 4, .10f }, { 4, 8, .06f }, { 8, 12, .16f } },
    { { 2, 4, .40f }, { 4, 8, .45f }, { 8, 12, .15f }, { 2, 2, 0.0f } },
};

// [D] P(gap at a unit end) when the loop is not one continuous phrase, by the unit's place in its 4-unit block
// (0, 1 = half block, 2, 3 = block end).
constexpr float kGapAtUnitEnd[4] = { 0.25f, 0.6f, 0.25f, 0.9f };

// Phrase start step after a gap (the first onset of the next phrase; kPickupStep = a pickup into the downbeat).
// "other" entries of GP are spread over the even steps listed; Lyrical's step-15 share is merged into the pickup.
struct StartStep
{
    int step;
    float weight;
};

constexpr int kPickupStep = 14;
constexpr int kNumStartSteps = 8;
constexpr StartStep kStartSteps[kStyles][kNumStartSteps] = {
    { { 0, .58f }, { 2, .19f }, { kPickupStep, .04f }, { 4, .07f }, { 8, .08f }, { 6, .04f }, { 0, 0.f }, { 0, 0.f } },
    { { 0, .38f }, { 8, .10f }, { kPickupStep, .14f }, { 4, .09f }, { 2, .07f }, { 10, .07f }, { 6, .07f }, { 12, .05f } },
    { { 0, .87f }, { 2, .05f }, { 8, .04f }, { kPickupStep, .04f }, { 0, 0.f }, { 0, 0.f }, { 0, 0.f }, { 0, 0.f } },
    { { 0, .60f }, { 8, .20f }, { kPickupStep, .10f }, { 4, .10f }, { 0, 0.f }, { 0, 0.f }, { 0, 0.f }, { 0, 0.f } },
};

// Pickup shapes (GP 1.3.2 pickup masks ...0010, ...1010, ...101010): 1, 2 or 3 notes into the downbeat. A pickup of
// n notes needs a gap of at least n / 2 + 1 beats, so >= 1 beat of rest stays before it.
constexpr float kPickupNotes[3] = { .45f, .40f, .15f };

//==============================================================================
// Pitch: chord-tone kind on strong positions (GP 1.4.3 Wkind): root, 3rd, 5th, 7th, by function T / S / D.
constexpr float kWkind[3][4] = {
    { .42f, .26f, .24f, .08f },
    { .25f, .25f, .375f, .125f },
    { .41f, .26f, .28f, .05f },
};
constexpr float kWkindExtension = .08f;   // [D] chord-tone 9ths / 11ths / 13ths on strong positions
// Non-chord tones by interval above the chord root (GP 1.4.3): 9th, 11th, 13th; [D] 7th and the rest.
constexpr float kWnct9 = .42f, kWnct11 = .38f, kWnct13 = .20f, kWnct7 = .25f, kWnctOther = .15f;

constexpr float kShortSlotCtBonus = 0.05f;   // GP 5.4.3: pCT + 0.05 when the slot is 2 beats or shorter
constexpr float kAugSecondFactor = 0.5f;     // R1: an augmented 2nd counts as one step with x0.5
constexpr double kContourTau = 4.0;          // Wcontour = exp (-|p - target| / tau)
constexpr double kRepickTau = 2.5;           // [D] re-picked units follow the prototype's pitches more tightly
constexpr double kForcedContourTau = 1.0;    // [D] tuned: a user-forced contour steers harder (Wreg also centres on it)
constexpr double kForcedSpanScale = 1.3;     // [D] tuned: ... over a wider span than the corpus median (GP 1.4.5 p75 9-12)
constexpr float kAvoidFactor = 0.3f;         // R2
constexpr int kNctMaxRun = 2;                // [D] at most this many non-chord tones in a row (a held NCT and its repeat),
                                             // then a chord tone; the note after a held pair steps to a chord tone

// Contour draws per phrase (GP 1.4.5): Rising, Falling, Arch, Valley, Wave.
enum ContourShape { kShapeRising = 0, kShapeFalling, kShapeArch, kShapeValley, kShapeWave, kNumShapes };
constexpr float kContourWeights[kStyles][kNumShapes] = {
    { .28f, .14f, .13f, .05f, .40f }, { .29f, .24f, .13f, .07f, .27f }, { .31f, .13f, .04f, .04f, .48f },
    { .30f, .30f, .20f, .05f, .15f },
};
constexpr double kArchPeak = 0.4;

// Non-chord-tone constructions (GP 1.4.7): passing, neighbour, appoggiatura, escape, anticipation, held, free.
enum NctType { kNctPassing = 0, kNctNeighbour, kNctAppoggiatura, kNctEscape, kNctAnticipation, kNctHeld, kNctFree, kNumNct };
constexpr float kNctWeights[kStyles][kNumNct] = {
    { .11f, .09f, .09f, .10f, .06f, .40f, .15f },
    { .17f, .10f, .15f, .13f, .11f, .21f, .13f },
    { .07f, .05f, .12f, .09f, .02f, .28f, .37f },
    { .20f, .15f, .20f, .05f, .10f, .25f, .05f },
};
constexpr float kAppoggiaturaDownFactor = 2.0f;
constexpr float kAppoggiaturaOpposite = 0.57f;
constexpr float kAnticipationRepeat = 0.6f;
constexpr double kNctMaxIoiBeats[kStyles] = { 2.0, 3.0, 2.0, 4.0 };   // [D] the resolving note follows within this

// Driving pedal alternation (GP 1.4.8). The pedal is the chord root at the window floor (+ 0..11) and, in a unit
// that alternates, the 5th a fifth above it on every second pedal note; the choice is made once per unit (copies
// inherit it), so a repeated bar repeats its pedal. 0.6 of the units alternate -> GP's 5th share of .3.
constexpr float kPedalFifthPattern = 0.6f;
constexpr int kPedalUpperGap = 7;
constexpr double kPedalAnchor = 3.0;      // [D] a forced contour moves the pedal's anchor (floor + this) along its path
constexpr int kDrivingLineMaxLeap = 12;   // [D] tuned: without the pedal a Driving line leaps at most an octave (the
                                          // corpus' 8+ step leaps are the pedal figure, which the pedal mode plays)

//==============================================================================
// Cadences (GP 1.5.5): interval class above the chord root: root, 2nd / 9th, 3rd, 4th / 11th, 5th, 6th / 13th, 7th.
constexpr float kCadence[kStyles][7] = {
    { .25f, .08f, .20f, .05f, .23f, .08f, .09f },
    { .21f, .14f, .23f, .04f, .25f, .06f, .07f },
    { .31f, .05f, .28f, .03f, .07f, .21f, .05f },
    { .25f, .10f, .25f, .02f, .28f, .05f, .05f },
};
constexpr float kCadenceChordTone = 2.0f;   // [D] tuned: phrase-final chord tones (corpus .69-.72)

// Approach into the phrase-final note by signed scale steps -4..+4 (index step + 4; 4 = repeat). Driving uses
// Uplifting's row and Sparse Lyrical's ([U] / [D]); unlisted moves get the floor; a chromatic semitone gets its own.
constexpr float kApproach[kStyles][9] = {
    { .07f, .06f, .14f, .12f, .34f, .21f, .02f, .02f, .02f },
    { .06f, .06f, .16f, .15f, .14f, .18f, .06f, .04f, .02f },
    { .07f, .06f, .14f, .12f, .34f, .21f, .02f, .02f, .02f },
    { .06f, .06f, .16f, .15f, .14f, .18f, .06f, .04f, .02f },
};
constexpr float kApproachFloor = .02f;
constexpr float kApproachChromatic[kStyles] = { .02f, .11f, .02f, .11f };

// Key-relative rows by scale degree 1..7 (GP 1.5.5): phrase-final and the melody's first note, minor / major.
constexpr float kFinalMinor[7] = { .24f, .12f, .16f, .08f, .26f, .06f, .06f };
constexpr float kFinalMajor[7] = { .24f, .11f, .28f, .02f, .15f, .13f, .05f };
constexpr float kFirstMinor[7] = { .46f, .04f, .17f, .06f, .18f, .04f, .04f };
constexpr float kFirstMajor[7] = { .28f, .08f, .15f, .06f, .22f, .13f, .07f };
constexpr float kResolveTonic = 2.0f;        // GP 1.5.6: the tonic x2 on the loop's final note
constexpr float kResolveFamily = 1.5f;       // [D] tuned: ... and the tonic family (1, 3, 5) x1.5 (the clash cut costs
                                             // the tonic over V / bVII; final-spec 9 WP1 wants the loop end home in
                                             // >= 65% of seeds, MelodyStats::loopEndHome)
constexpr float kResolveChordTone = 2.0f;    // [D] tuned: the final note also leans to a tone of the last chord
constexpr float kResolveClash = 0.01f;       // [D] ... and avoids a non-chord tone a semitone from a tone of that chord
                                             // (or a b9 above its bass); a pickup into the loop start also uses it
// [D] When no tonic-family tone (1, 3, 5) is a tone of the last chord (VII in minor, V or IV in major), a chord tone
// 1-2 semitones from the melody's first note counts as "home" (at least the tonic's row weight x2 x this): it resolves
// through the seam. Tuned toward chord fit: these seam endings count as home (MelodyStats::loopEndHome, final-spec 9
// WP1), so the loop end no longer has to land on a non-chord tonic over such a chord to pass (0.25 did, for the old
// "tonic family in >= 60%" rule: about half of all loop ends were non-chord tonic-family tones).
constexpr float kResolveHome = 0.7f;
// [D] tuned: when the tonic itself clashes with the last chord but the chord holds another tonic-family tone (V or iii
// in major), the clash-free tonic-family candidates get this extra factor, so resolveEnding changes V-ending loops too.
constexpr float kResolveFamilyStrong = 5.0f;

// R3 open / closed cadence factors by key degree 1..7. [D] A closed factor above 1 lifts chord tones only (a non-chord
// tone gets at most x1.0), except at a resolveEnding loop end, which keeps its own tuning above; otherwise a closed
// block end over V / bVII favoured the non-chord tonic over its own chord tones.
constexpr float kOpenFactor[7] = { 0.6f, 1.4f, 0.9f, 1.0f, 1.4f, 1.0f, 1.2f };
constexpr float kClosedFactor[7] = { 1.6f, 0.6f, 1.3f, 1.0f, 1.0f, 1.0f, 0.5f };

// R7 loop seam.
constexpr int kMaxSeam = 12;

// R8 above the arp (soft).
constexpr int kArpOffset = 3;
constexpr int kArpMaxShift = 3;
constexpr float kBelowArpFactor = 0.5f;

//==============================================================================
// Articulation (GP 1.5.7).
constexpr float kGateValue[4] = { 0.30f, 0.55f, 0.82f, 0.97f };

struct LengthWeight
{
    double beats;
    float weight;
};

// Phrase-final lengths ("other" spread over a few nearby lengths); Driving uses Uplifting's row ([U]); Sparse holds
// into the gap (clipped at 4 beats).
constexpr LengthWeight kFinalLenUplifting[] = {
    { 0.25, .31f }, { 0.5, .23f }, { 0.375, .18f }, { 0.75, .09f }, { 1.0, .05f }, { 1.5, .04f }, { 1.25, .05f }, { 2.0, .05f },
};
constexpr LengthWeight kFinalLenLyrical[] = {
    { 0.5, .37f }, { 0.25, .21f }, { 1.0, .08f }, { 1.5, .06f }, { 0.75, .05f }, { 2.0, .05f }, { 4.0, .03f }, { 6.0, .03f },
    { 0.375, .04f }, { 1.25, .04f }, { 3.0, .04f },
};
constexpr double kSparseFinalMax = 4.0;
constexpr double kMinRestAfterPhrase = 1.0;   // [D] a phrase-final note before a gap leaves >= 1 beat of rest
constexpr double kNctMaxRest = 0.5;           // [D] a non-chord tone inside a phrase leads on: at most this much rest
constexpr double kLengthGrid = 0.125;         // [D] note lengths snap to 1/32 notes (tidy in the roll)
constexpr double kLonePhraseHold = 2.0;       // [D] a one-note phrase holds up to this long

} // namespace melodygen
} // namespace cb
