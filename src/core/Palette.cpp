#include "core/Palette.h"

#include <algorithm>

// Chord palette generation.
//
//  Triads / Sevenths : the 7 diatonic chords stacked in thirds, degree order.
//  Color             : up to 8 diatonic colour chords (add9, 6, 6/9, maj9/m9/m11, maj7#11, 13, 7b9, sus). Chosen from a
//                      per-family wish list ordered by what producers actually use (out/midi_stats.md: in minor keys
//                      VII(add9), VI(add9), VII6, III6 and iv(m9) lead; in major keys IV(add9), IV6, I(add9), maj9),
//                      plus the extended colours of R&B / neo-soul (i11, I6/9, the Lydian IVmaj7#11 / VImaj7#11, V13,
//                      harmonic minor's V7b9). Filtered to chords whose tones are all in the key, topped up from a
//                      generic list when a mode rules many out, and finally shown in scale-degree order (tonic colours
//                      first) so the row reads like the Triads row.
//                      C major:  Cadd9, C6/9, Cmaj9, Dm9, Fadd9, F6, Fmaj7#11, G13
//                      A minor:  Am11, C6, Dm9, Fadd9, Fmaj7#11, Gadd9, G6, Gsus4
//  Borrowed          : up to 8 chords that are not diatonic in the key, ordered by how often they turn up in the data.
//                      Major-family keys borrow from the parallel minor: bVI, bVII, iv, bIII, iv7, bVImaj7, v, i7 (...).
//                      Natural Minor borrows from the parallel major: V, IV, I, V7, ii, IVmaj7, vii°, ii7 (...).
//                      The other minor-family scales borrow the natural-minor chords they lose first (A Harmonic Minor:
//                      G, C, Am7, Em, Em7, Cmaj7, then D, A), then parallel-major chords.
//  Secondary         : V7/x for every diatonic, non-diminished, non-tonic target x, in target-degree order, skipping
//                      chords already shown earlier in the palette (and augmented III+ targets of harmonic/melodic minor).
//                      Minor-family keys also get the darker V7b9/x when the target is minor or the b9 is in the key
//                      (A minor: A7, A7b9, B7, B7b9, C7, D7), never more than 6 pads, plain V7s first.

namespace cb
{
namespace
{
    //==========================================================================
    int wrapDegree (int degree) { return ((degree % 7) + 7) % 7; }

    ChordQuality triadQualityFor (int third, int fifth)
    {
        if (fifth == 6) return ChordQuality::Diminished;
        if (fifth == 8) return ChordQuality::Augmented;
        return third >= 4 ? ChordQuality::Major : ChordQuality::Minor;
    }

    bool contains (const std::vector<PaletteChord>& pads, const Chord& chord)
    {
        for (auto& p : pads)
            if (p.chord == chord)
                return true;
        return false;
    }

    bool containsAnywhere (const std::vector<PaletteSection>& sections, const Chord& chord)
    {
        for (auto& s : sections)
            if (contains (s.chords, chord))
                return true;
        return false;
    }

    //==========================================================================
    // Hint helpers. Hints are one friendly sentence; chord names are spelled in the key.
    struct HintNames
    {
        std::string tonic;        // "C" / "Am"
        std::string dominant;     // name of the diatonic triad on degree 5 when it is major or minor, else ""
        std::string subdominant;  // diatonic triad on degree 4 when it is major or minor, else ""
        std::string submediant;   // diatonic triad on degree 6
        std::string seventhDeg;   // diatonic triad on degree 7
        std::string majorDominant;// the major V on the tonic's fifth ("G" for C, "E" for A minor)
    };

    HintNames makeNames (const Key& key)
    {
        HintNames n;
        n.tonic = diatonicChord (key, 0, false).name (key);

        const auto v = diatonicChord (key, 4, false);
        if (v.quality == ChordQuality::Major || v.quality == ChordQuality::Minor)
            n.dominant = v.name (key);

        const auto iv = diatonicChord (key, 3, false);
        if (iv.quality == ChordQuality::Major || iv.quality == ChordQuality::Minor)
            n.subdominant = iv.name (key);

        n.submediant = diatonicChord (key, 5, false).name (key);
        n.seventhDeg = diatonicChord (key, 6, false).name (key);
        n.majorDominant = Chord { pc (key.tonic + 7), ChordQuality::Major }.name (key);
        return n;
    }

    // Full sentence for a diatonic triad.
    std::string triadHint (const Key& key, int degree, ChordQuality triad, const HintNames& n)
    {
        const int offset = scaleInfo (key.scale).offsets[(size_t) wrapDegree (degree)];

        switch (wrapDegree (degree))
        {
            case 0:
                if (triad == ChordQuality::Major) return "Home base - everything resolves here.";
                if (triad == ChordQuality::Minor) return "Home base - dark, settled, everything resolves here.";
                return "Locrian's restless home - unstable even at rest.";

            case 1:
                if (triad == ChordQuality::Diminished)
                    return n.dominant.empty() ? "Tense setup chord - leans away from home."
                                              : "Tense setup chord - leans hard into " + n.dominant + ".";
                if (triad == ChordQuality::Minor)
                    return n.dominant.empty() ? "Gentle setup chord - leads smoothly away from home."
                                              : "Gentle setup chord - leads beautifully into " + n.dominant + ".";
                if (offset == 1)
                    return "Dark half-step above home - slides straight back down to " + n.tonic + ".";
                return "Bright Lydian lift - the dreamy, floating sound of the mode.";

            case 2:
                if (triad == ChordQuality::Augmented) return "Augmented shimmer - eerie, cinematic tension.";
                if (triad == ChordQuality::Major)     return "The relative major - an instant, brighter lift.";
                if (triad == ChordQuality::Minor)     return "Soft and wistful - a gentle bridge toward " + n.submediant + ".";
                return "Unstable passing colour - keep it moving.";

            case 3:
                if (triad == ChordQuality::Major)
                    return n.dominant.empty() ? "Open and hopeful - glides back to " + n.tonic + "."
                                              : "Open and hopeful - moves on to " + n.dominant + " or back to " + n.tonic + ".";
                if (triad == ChordQuality::Minor)
                    return n.dominant.empty() ? "Melancholy and soulful - sinks gently back to " + n.tonic + "."
                                              : "Melancholy and soulful - beautiful before " + n.dominant + " or back to " + n.tonic + ".";
                return n.dominant.empty() ? "Tense, bright colour - keep it moving."
                                          : "Tense, bright Lydian colour - leans up to " + n.dominant + ".";

            case 4:
                if (offset != 7)                  return "A tritone from home - dark, unstable tension.";
                if (triad == ChordQuality::Major) return "Dominant tension - wants to resolve to " + n.tonic + ".";
                if (triad == ChordQuality::Minor) return "Soft minor dominant - a moody, gentle pull back to " + n.tonic + ".";
                return "Shadowy and unstable - drifts back toward " + n.tonic + ".";

            case 5:
                if (triad == ChordQuality::Minor)
                    return n.subdominant.empty() ? "The relative minor - emotional, perfect after " + n.tonic + "."
                                                 : "The relative minor - emotional, perfect after " + n.tonic + " or " + n.subdominant + ".";
                if (triad == ChordQuality::Major) return "Warm and epic - the big lift of minor-key pop.";
                return "Tense passing chord - leans toward " + n.seventhDeg + " or home.";

            default:
                if (triad == ChordQuality::Diminished) return "Leading-tone tension - pushes right back to " + n.tonic + ".";
                if (triad == ChordQuality::Major)      return "Rising energy - lifts straight back to " + n.tonic + ".";
                return "Gentle passing colour - steps back up to " + n.tonic + ".";
        }
    }

    // Short role phrase for a degree (used after a flavour phrase: "Lush, dreamy maj7 - <role>.").
    std::string rolePhrase (const Key& key, int degree, ChordQuality triad, const HintNames& n)
    {
        const int d = wrapDegree (degree);
        const int offset = scaleInfo (key.scale).offsets[(size_t) d];

        switch (d)
        {
            case 0:  return "a richer way to land home";
            case 2:  return "a gentle step away from home";
            case 5:  return "an emotional stand-in for home";
            case 4:  return "pulls back to " + n.tonic;
            case 6:  return triad == ChordQuality::Diminished ? "pushes back to " + n.tonic
                                                              : "lifts back up to " + n.tonic;
            default: break;
        }

        if (d == 1 && offset == 1)
            return "slides back down to " + n.tonic;

        return n.dominant.empty() ? "leads nicely back home" : "sets up " + n.dominant + " nicely";
    }

    const char* flavourPhrase (ChordQuality q)
    {
        switch (q)
        {
            case ChordQuality::Major7:          return "Lush, dreamy maj7";
            case ChordQuality::Minor7:          return "Smooth, mellow m7";
            case ChordQuality::Dominant7:       return "Bluesy, funky dominant 7th";
            case ChordQuality::HalfDiminished7: return "Dark, moody half-diminished";
            case ChordQuality::Diminished7:     return "Maximum-tension diminished 7th";
            case ChordQuality::MinorMajor7:     return "Mysterious, film-noir minor-major 7th";
            case ChordQuality::Augmented:       return "Eerie augmented shimmer";
            case ChordQuality::Add9:            return "Bright, sparkly add9";
            case ChordQuality::MinorAdd9:       return "Sparkly, emotional minor add9";
            case ChordQuality::Major9:          return "Lush, jazzy maj9";
            case ChordQuality::Minor9:          return "Smooth, soulful m9";
            case ChordQuality::Dominant9:       return "Funky, soulful dominant 9th";
            case ChordQuality::Sus2:            return "Open, airy sus2";
            case ChordQuality::Sus4:            return "Suspended, yearning sus4";
            case ChordQuality::Dominant7Sus4:   return "Soft, gospel-style 7sus4";
            case ChordQuality::Major6:          return "Sweet, vintage 6th";
            case ChordQuality::Minor6:          return "Bittersweet, noir minor 6th";
            case ChordQuality::Minor11:         return "Lush, neo-soul m11";
            case ChordQuality::Major69:         return "Smooth, open 6/9";
            case ChordQuality::Dominant13:      return "Rich, soulful 13th";
            case ChordQuality::Major7Sharp11:   return "Dreamy, floating Lydian maj7#11";
            case ChordQuality::Dominant7Flat9:  return "Dark, dramatic 7b9";
            case ChordQuality::Diminished:      return "Tense diminished";
            case ChordQuality::Minor:           return "Plain minor";
            default:                            return "Plain major";
        }
    }

    // Hint for a diatonic seventh or colour chord on `degree`.
    std::string extendedHint (const Key& key, const Chord& chord, int degree, const HintNames& n)
    {
        const auto triad = diatonicChord (key, degree, false);

        if (chord.quality == ChordQuality::Dominant7 && wrapDegree (degree) == 4)
            return "The classic V7 - the strongest pull back to " + n.tonic + ".";

        if (chord.quality == ChordQuality::Dominant7Flat9 && wrapDegree (degree) == 4)
            return "Dark, dramatic 7b9 - the most intense pull back to " + n.tonic + ".";

        if (chord.quality == ChordQuality::Dominant13 && wrapDegree (degree) == 4)
            return "Rich, soulful 13th - a smooth, jazzy pull back to " + n.tonic + ".";

        if (chord.quality == ChordQuality::Sus4)
            return std::string (flavourPhrase (chord.quality)) + " - resolve it to " + triad.name (key) + ".";

        return std::string (flavourPhrase (chord.quality)) + " - " + rolePhrase (key, degree, triad.quality, n) + ".";
    }

    // `source` is the key the chord is borrowed from: key.parallel(), or the natural minor on the same tonic for the
    // other minor-family scales ("Borrowed from A natural minor: ...").
    std::string borrowedHint (const Key& key, const Key& source, int parallelDegree, bool seventh, const HintNames& n)
    {
        const bool fromMinor = scaleInfo (source.scale).isMinorFamily;
        const bool keyIsMinor = scaleInfo (key.scale).isMinorFamily;
        const std::string sourceName = "Borrowed from " + key.tonicName()
                                     + (source.scale == ScaleType::NaturalMinor && keyIsMinor ? " natural minor: "
                                                                                               : (fromMinor ? " minor: " : " major: "));

        std::string feel;
        if (fromMinor && keyIsMinor && wrapDegree (parallelDegree) == 0)
        {
            // Natural-minor tonic chords inside another minor scale (Harmonic Minor's Am7, Locrian's Am).
            feel = seventh ? "a softer, mellow minor-7th home" : "a steadier, darker home";
        }
        else if (fromMinor)
        {
            switch (wrapDegree (parallelDegree))
            {
                case 0:  feel = seventh ? "a moody minor-7th shadow over home" : "the minor tonic - a sudden, dramatic shadow"; break;
                case 1:  feel = seventh ? "a moody, jazzy lead-in to " + n.majorDominant : "dark and tense, leaning into " + n.majorDominant; break;
                case 2:  feel = seventh ? "a lush, dreamy detour" : "a bold, rock-flavoured lift"; break;
                case 3:  feel = seventh ? "soulful and nostalgic, melting back into " + n.tonic : "bittersweet and nostalgic, gorgeous right before " + n.tonic; break;
                case 4:  feel = seventh ? "a mellow, jazzy twist on the dominant" : "a soft, moody stand-in for " + n.majorDominant; break;
                case 5:  feel = seventh ? "lush, cinematic and dreamy" : "big, cinematic and epic"; break;
                default: feel = seventh ? "the soulful 'backdoor' slide home to " + n.tonic : "an anthemic, heroic lift back to " + n.tonic; break;
            }
        }
        else
        {
            switch (wrapDegree (parallelDegree))
            {
                case 0:  feel = seventh ? "a bright, dreamy major-7th home" : "the Picardy major - a bright, hopeful arrival"; break;
                case 1:  feel = seventh ? "a smooth, jazzy lead-in to " + n.majorDominant : "a brighter setup chord that leans into " + n.majorDominant; break;
                case 2:  feel = seventh ? "a soft, surprising colour" : "an unexpected bright twist"; break;
                case 3:  feel = seventh ? "warm, lush and hopeful" : "a warm, hopeful ray of light"; break;
                case 4:  feel = seventh ? "the V7 - maximum pull back to " + n.tonic : "the major V - the strongest pull back to " + n.tonic; break;
                case 5:  feel = seventh ? "a mellow, unexpected colour" : "a bright, surprising lift"; break;
                default: feel = seventh ? "dark leading-tone tension resolving to " + n.tonic : "leading-tone tension that snaps back to " + n.tonic; break;
            }
        }

        return sourceName + feel + ".";
    }

    PaletteChord makePad (const Chord& chord, const Key& key, std::string hint)
    {
        return { chord, romanNumeral (chord, key), harmonicFunction (chord, key), std::move (hint) };
    }

    //==========================================================================
    struct ColourSlot
    {
        int degree;
        std::vector<ChordQuality> qualities;   // first one that is diatonic (and new) wins
    };

    const std::vector<ColourSlot>& colourWishList (bool minorFamily)
    {
        using Q = ChordQuality;

        // Major family (Major, Lydian, Mixolydian), most wanted first (the row is re-sorted by degree afterwards).
        // C major:      Cadd9, Fadd9, F6, C6/9, Fmaj7#11, G13, Dm9, Cmaj9
        // C Lydian:     Cmaj7#11, Cadd9, C6/9, G6/9, Dadd9, Cmaj9, Am9, Gsus4
        // C Mixolydian: Cadd9, Fadd9, F6, C6/9, Fmaj9, Dm9, C13, Bbadd9
        static const std::vector<ColourSlot> major {
            { 0, { Q::Major7Sharp11 } },                          // Lydian's signature tonic (only diatonic there)
            { 0, { Q::Add9 } },                                   // I(add9)
            { 3, { Q::Add9 } },                                   // IV(add9) - the most used major-key colour
            { 3, { Q::Major6 } },                                 // IV6
            { 0, { Q::Major69 } },                                // I6/9
            { 3, { Q::Major7Sharp11, Q::Major9 } },               // IVmaj7#11 - Lydian colour (Mixolydian: IVmaj9)
            { 4, { Q::Dominant13, Q::Major69, Q::Add9 } },        // V13 (Lydian: V6/9)
            { 1, { Q::Minor9, Q::Add9, Q::Dominant9 } },          // ii9 (Lydian: II(add9))
            { 0, { Q::Major9, Q::Dominant13, Q::Dominant9 } },    // Imaj9 (Mixolydian: I13)
            { 6, { Q::Add9, Q::Major69, Q::Major7Sharp11 } },     // Mixolydian bVII(add9)
            { 5, { Q::Minor9, Q::Minor11, Q::MinorAdd9 } },       // vi9
            { 4, { Q::Sus4, Q::Dominant7Sus4, Q::Major6 } },      // Vsus4
            { 1, { Q::Minor11 } },                                // ii11
            { 2, { Q::Minor9, Q::MinorAdd9 } },                   // iii9
            { 4, { Q::Minor9, Q::Minor11 } },                     // Mixolydian v9
            { 0, { Q::Sus2 } },
            { 3, { Q::Sus2 } },
        };

        // Minor family (Natural/Harmonic/Melodic Minor, Dorian, Phrygian, Locrian), most wanted first.
        // A minor:            Gadd9, Fadd9, G6, C6, Dm9, Am11, Fmaj7#11, Gsus4
        // A Harmonic Minor:   E7b9, F6, Dm9, Am(add9), Fmaj7#11, Asus2, E7sus4, Dm6
        // A Dorian:           Gadd9, G6, C6, D9, Am11, Gsus4, Cadd9, Am(add9)
        // A Melodic Minor:    D9, Am(add9), Am6, E9, D6, Bm6, E7sus4, D6/9
        static const std::vector<ColourSlot> minor {
            { 4, { Q::Dominant7Flat9 } },                         // Harmonic minor's V7b9 (only diatonic there)
            { 6, { Q::Add9, Q::Sus4, Q::Sus2 } },                 // VII(add9) - the most used minor-key colour
            { 5, { Q::Add9, Q::Major9, Q::Major6 } },             // VI(add9)
            { 6, { Q::Major6 } },                                 // VII6
            { 2, { Q::Major6, Q::Add9 } },                        // III6
            { 3, { Q::Minor9, Q::Minor11, Q::Dominant9, Q::MinorAdd9, Q::Add9 } },   // iv9 (Dorian / Melodic: IV9)
            { 0, { Q::Minor11, Q::Minor9, Q::MinorAdd9 } },       // i11
            { 5, { Q::Major7Sharp11, Q::Major9 } },               // VImaj7#11 - the Lydian VI of natural minor
            { 6, { Q::Sus4 } },                                   // VIIsus4
            { 2, { Q::Add9, Q::Major9, Q::Major69 } },            // III(add9)
            { 0, { Q::MinorAdd9, Q::Minor6, Q::Sus2 } },          // i(add9) / Melodic Minor's i6
            { 1, { Q::Major9, Q::Add9, Q::Major7Sharp11 } },      // Phrygian / Locrian bII
            { 4, { Q::Minor9, Q::Minor11, Q::Dominant9, Q::Dominant7Sus4, Q::Sus4 } },   // v9 (Melodic Minor: V9)
            { 3, { Q::Minor6, Q::Major6 } },                      // iv6 (Dorian / Melodic: IV6)
            { 1, { Q::Minor9, Q::Minor11, Q::Minor6 } },          // Dorian ii9 / Melodic Minor ii6
            { 4, { Q::Dominant7Sus4 } },                          // V7sus4
            { 3, { Q::Major69 } },                                // Dorian / Melodic IV6/9
            { 0, { Q::Sus2 } },
        };

        return minorFamily ? minor : major;
    }

    std::vector<PaletteChord> buildColour (const Key& key, const std::vector<PaletteSection>& earlier, const HintNames& names)
    {
        const auto pcs = key.pitchClasses();

        struct Pick
        {
            Chord chord;
            int degree;
        };

        std::vector<Pick> picks;

        auto tryAdd = [&] (int degree, ChordQuality q)
        {
            if ((int) picks.size() >= kMaxPadsPerSection)
                return false;

            const Chord c { pcs[(size_t) wrapDegree (degree)], q };
            if (! isDiatonic (c, key) || containsAnywhere (earlier, c))
                return false;

            for (auto& p : picks)
                if (p.chord == c)
                    return false;

            picks.push_back ({ c, wrapDegree (degree) });
            return true;
        };

        for (auto& slot : colourWishList (scaleInfo (key.scale).isMinorFamily))
            for (auto q : slot.qualities)
                if (tryAdd (slot.degree, q))
                    break;

        // Top-up for modes where the wish list is mostly ruled out (Locrian, Phrygian, Harmonic Minor ...).
        static const int fallbackDegrees[] = { 0, 4, 3, 5, 2, 6, 1 };
        static const ChordQuality fallbackQualities[] = {
            ChordQuality::Add9, ChordQuality::MinorAdd9, ChordQuality::Sus2, ChordQuality::Sus4,
            ChordQuality::Major9, ChordQuality::Minor9, ChordQuality::Dominant9, ChordQuality::Dominant7Sus4,
            ChordQuality::Major6, ChordQuality::Minor6, ChordQuality::Major69, ChordQuality::Minor11
        };

        // Round-robin over the degrees, one chord per degree per round (the first quality that works), two rounds: the
        // top-up spreads over the roots instead of piling every variant onto the first productive one (B Locrian used
        // to get five F chords). The tonic still gets the first chance in each round.
        for (int round = 0; round < 2; ++round)
            for (int degree : fallbackDegrees)
                for (auto q : fallbackQualities)
                    if (tryAdd (degree, q))
                        break;

        std::stable_sort (picks.begin(), picks.end(), [] (const Pick& a, const Pick& b) { return a.degree < b.degree; });

        std::vector<PaletteChord> pads;
        for (auto& p : picks)
            pads.push_back (makePad (p.chord, key, extendedHint (key, p.chord, p.degree, names)));
        return pads;
    }

    //==========================================================================
    std::vector<PaletteChord> buildBorrowed (const Key& key, const HintNames& names)
    {
        struct Want
        {
            int degree;      // degree in the source key
            bool seventh;
            bool natural;    // source: the natural minor on the key's tonic (instead of key.parallel())
        };

        // Major-family keys borrowing from the parallel minor, ordered by use in the data: bVI, bVII, iv, bIII,
        // iv7, bVImaj7, v, i7, then the rest.
        static const std::vector<Want> fromMinor {
            { 5, false, false }, { 6, false, false }, { 3, false, false }, { 2, false, false },   // bVI, bVII, iv, bIII
            { 3, true, false },  { 5, true, false },  { 4, false, false }, { 0, true, false },    // iv7, bVImaj7, v, i7
            { 0, false, false }, { 1, true, false },  { 6, true, false },  { 1, false, false },   // i, iiø7, bVII7, ii°
            { 2, true, false },  { 4, true, false },                                             // bIIImaj7, v7
        };

        // Minor-family keys borrowing from the parallel major.
        static const std::vector<Want> fromMajor {
            { 4, false, false }, { 3, false, false }, { 0, false, false }, { 4, true, false },    // V, IV, I, V7
            { 1, false, false }, { 3, true, false },  { 6, false, false }, { 1, true, false },    // ii, IVmaj7, vii°, ii7
            { 0, true, false },  { 5, false, false }, { 2, false, false }, { 6, true, false },    // Imaj7, vi, iii, viiø7
            { 5, true, false },  { 2, true, false },                                             // vi7, iii7
        };

        // The other minor-family scales borrowing the natural-minor chords they lose, ordered by use in the data
        // (VII 219, VI 168, III 110, VI(maj7) 99, i7 88, v 69, iv7 69, iv 68, v7 53, III(maj7) 23).
        static const std::vector<Want> fromNatural {
            { 6, false, true }, { 5, false, true }, { 2, false, true }, { 5, true, true },        // VII, VI, III, VImaj7
            { 0, true, true },  { 4, false, true }, { 3, true, true },  { 3, false, true },       // i7, v, iv7, iv
            { 4, true, true },  { 2, true, true },  { 6, true, true },  { 0, false, true },       // v7, IIImaj7, VII7, i
            { 1, false, true }, { 1, true, true },                                               // ii°, iiø7
        };

        // Per-scale heads; whatever is left is filled from the generic lists above.
        static const std::vector<Want> harmonicMinor {   // A: G, C, Am7, Em, Em7, Cmaj7, D, A
            { 6, false, true }, { 2, false, true }, { 0, true, true },  { 4, false, true },
            { 4, true, true },  { 2, true, true },  { 3, false, false }, { 0, false, false },
        };
        static const std::vector<Want> dorian {          // A: E, F, Dm, A, E7, Fmaj7, Dm7, Dmaj7
            { 4, false, false }, { 5, false, true }, { 3, false, true }, { 0, false, false },
            { 4, true, false },  { 5, true, true },  { 3, true, true },  { 3, true, false },
        };
        static const std::vector<Want> phrygian {        // A: E, G, D, Em, A, E7, Em7, Cmaj7
            { 4, false, false }, { 6, false, true }, { 3, false, false }, { 4, false, true },
            { 0, false, false }, { 4, true, false }, { 4, true, true },   { 2, true, true },
        };
        static const std::vector<Want> locrian {         // A: Am, E, G, C, Em, D, Am7, A
            { 0, false, true }, { 4, false, false }, { 6, false, true }, { 2, false, true },
            { 4, false, true }, { 3, false, false }, { 0, true, true },  { 0, false, false },
        };

        std::vector<const std::vector<Want>*> lists;
        if (! scaleInfo (key.scale).isMinorFamily)
        {
            lists = { &fromMinor };
        }
        else
        {
            switch (key.scale)
            {
                case ScaleType::HarmonicMinor: lists = { &harmonicMinor }; break;
                case ScaleType::MelodicMinor:  lists = { &fromNatural }; break;   // A: G, F, C, Fmaj7, Am7, Em, Dm7, Dm
                case ScaleType::Dorian:        lists = { &dorian }; break;
                case ScaleType::Phrygian:      lists = { &phrygian }; break;
                case ScaleType::Locrian:       lists = { &locrian }; break;
                default:                       break;
            }
            lists.push_back (&fromMajor);
            if (key.scale != ScaleType::NaturalMinor)
                lists.push_back (&fromNatural);
        }

        const Key par = key.parallel();
        const Key natural { key.tonic, ScaleType::NaturalMinor };

        std::vector<PaletteChord> pads;
        for (auto* list : lists)
            for (auto& w : *list)
            {
                if ((int) pads.size() >= kMaxPadsPerSection)
                    return pads;

                const Key& source = w.natural ? natural : par;
                const Chord c = diatonicChord (source, w.degree, w.seventh);
                if (isDiatonic (c, key) || contains (pads, c))
                    continue;

                pads.push_back (makePad (c, key, borrowedHint (key, source, w.degree, w.seventh, names)));
            }
        return pads;
    }

    //==========================================================================
    constexpr int kMaxSecondaryPads = 6;   // Palette.h: "up to 6 secondary dominants"

    std::vector<PaletteChord> buildSecondary (const Key& key, const std::vector<PaletteSection>& earlier)
    {
        const auto pcs = key.pitchClasses();

        struct Pick
        {
            PaletteChord pad;
            int target;
            bool variant;
        };

        std::vector<Pick> picks;
        auto isNew = [&] (const Chord& c)
        {
            if (containsAnywhere (earlier, c))
                return false;
            for (auto& p : picks)
                if (p.pad.chord == c)
                    return false;
            return true;
        };

        // Pass 1: V7/x for every usable target. Pass 2 (minor-family keys only): the darker V7b9/x where it is
        // idiomatic - the target is a minor chord (A minor: A7b9 -> Dm) or the b9 is a scale tone (B7b9 -> Em / E,
        // whose b9 C belongs to A minor). Plain V7s always win the limited room.
        const bool minorKey = scaleInfo (key.scale).isMinorFamily;

        for (int pass = 0; pass < (minorKey ? 2 : 1); ++pass)
            for (int target = 1; target < 7 && (int) picks.size() < kMaxSecondaryPads; ++target)
            {
                // Diminished targets are excluded by definition; augmented ones (III+ of harmonic / melodic minor) are
                // skipped too - "pulls strongly to Ebaug" is not a move anyone wants suggested.
                const Chord targetChord = diatonicChord (key, target, false);
                if (targetChord.quality == ChordQuality::Diminished || targetChord.quality == ChordQuality::Augmented)
                    continue;

                const int root = pc (pcs[(size_t) target] + 7);
                const bool variant = pass == 1;

                if (variant)
                {
                    const bool minorTarget = targetChord.quality == ChordQuality::Minor;
                    const bool flatNineInKey = key.contains (root + 1);
                    if (! minorTarget && ! flatNineInKey)
                        continue;

                    // Only next to its plain V7 (so the row never shows a 7b9 without its V7).
                    bool havePlain = false;
                    for (auto& p : picks)
                        havePlain = havePlain || (p.target == target && ! p.variant);
                    if (! havePlain)
                        continue;
                }

                const Chord c { root, variant ? ChordQuality::Dominant7Flat9 : ChordQuality::Dominant7 };
                if (harmonicFunction (c, key) != HarmonicFunction::Secondary || ! isNew (c))
                    continue;

                const auto targetLabel = targetChord.name (key) + " (" + romanNumeral (targetChord, key, false) + ")";
                const auto hint = variant ? "A darker, dramatic pull into " + targetLabel + " - the b9 adds bite."
                                          : "Pulls strongly to " + targetLabel + " - bright, chromatic momentum.";
                picks.push_back ({ makePad (c, key, hint), target, variant });
            }

        // Target-degree order, each 7b9 right after its V7.
        std::stable_sort (picks.begin(), picks.end(), [] (const Pick& a, const Pick& b)
        {
            return a.target != b.target ? a.target < b.target : (! a.variant && b.variant);
        });

        std::vector<PaletteChord> pads;
        for (auto& p : picks)
            pads.push_back (std::move (p.pad));
        return pads;
    }
} // namespace

//==============================================================================
Chord diatonicChord (const Key& key, int degree, bool sevenths)
{
    const auto pcs = key.pitchClasses();
    const int d = wrapDegree (degree);
    const int root = pcs[(size_t) d];
    const int third = pc (pcs[(size_t) ((d + 2) % 7)] - root);
    const int fifth = pc (pcs[(size_t) ((d + 4) % 7)] - root);
    const int seventh = pc (pcs[(size_t) ((d + 6) % 7)] - root);

    const auto triad = triadQualityFor (third, fifth);
    if (! sevenths)
        return { root, triad };

    using Q = ChordQuality;
    if (third == 4 && fifth == 7 && seventh == 11) return { root, Q::Major7 };
    if (third == 3 && fifth == 7 && seventh == 10) return { root, Q::Minor7 };
    if (third == 4 && fifth == 7 && seventh == 10) return { root, Q::Dominant7 };
    if (third == 3 && fifth == 6 && seventh == 10) return { root, Q::HalfDiminished7 };
    if (third == 3 && fifth == 6 && seventh == 9)  return { root, Q::Diminished7 };
    if (third == 3 && fifth == 7 && seventh == 11) return { root, Q::MinorMajor7 };

    // No matching seventh quality (the augmented-major-7th III+maj7 of harmonic / melodic minor):
    // the diatonic triad is the closest in-key chord.
    return { root, triad };
}

std::vector<PaletteSection> buildPalette (const Key& key)
{
    const auto names = makeNames (key);

    std::vector<PaletteSection> sections;
    sections.reserve ((size_t) numPaletteSections);

    PaletteSection triads { PaletteSectionId::Triads, "Triads", {} };
    PaletteSection sevenths { PaletteSectionId::Sevenths, "Sevenths", {} };

    for (int degree = 0; degree < 7; ++degree)
    {
        const auto t = diatonicChord (key, degree, false);
        triads.chords.push_back (makePad (t, key, triadHint (key, degree, t.quality, names)));

        const auto s = diatonicChord (key, degree, true);
        sevenths.chords.push_back (makePad (s, key, s == t ? triadHint (key, degree, t.quality, names)
                                                           : extendedHint (key, s, degree, names)));
    }

    sections.push_back (std::move (triads));
    sections.push_back (std::move (sevenths));

    sections.push_back ({ PaletteSectionId::Color, "Color", buildColour (key, sections, names) });
    sections.push_back ({ PaletteSectionId::Borrowed, "Borrowed", buildBorrowed (key, names) });
    sections.push_back ({ PaletteSectionId::Secondary, "Secondary", buildSecondary (key, sections) });

    return sections;
}
} // namespace cb
