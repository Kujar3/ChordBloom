#include "core/Palette.h"

#include <algorithm>

// Chord palette generation.
//
//  Triads / Sevenths : the 7 diatonic chords stacked in thirds, degree order.
//  Color             : up to 8 diatonic colour chords (add9, maj9/m9/9, sus2, sus4, 7sus4, 6). Chosen from a
//                      per-family wish list (the chords modern producers reach for first), filtered to chords whose
//                      tones are all in the key, topped up from a generic list when a mode rules many out, and finally
//                      shown in scale-degree order (tonic colours first) so the row reads like the Triads row.
//  Borrowed          : up to 8 chords of key.parallel() that are not diatonic in the key, ordered by how often they
//                      turn up in pop / film music (major keys: iv, bVII, bVI, bIII, iv7, bVImaj7, v, i, ...;
//                      minor keys: V, IV, I, V7, ii, IVmaj7, vii°, ii7, ...).
//  Secondary         : V7/x for every diatonic, non-diminished, non-tonic target x, in target-degree order, skipping
//                      chords already shown earlier in the palette (and augmented III+ targets of harmonic/melodic minor).

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

        if (chord.quality == ChordQuality::Sus4)
            return std::string (flavourPhrase (chord.quality)) + " - resolve it to " + triad.name (key) + ".";

        return std::string (flavourPhrase (chord.quality)) + " - " + rolePhrase (key, degree, triad.quality, n) + ".";
    }

    std::string borrowedHint (const Key& key, int parallelDegree, bool seventh, const HintNames& n)
    {
        const bool fromMinor = scaleInfo (key.parallel().scale).isMinorFamily;
        const std::string source = "Borrowed from " + key.tonicName() + (fromMinor ? " minor: " : " major: ");

        std::string feel;
        if (fromMinor)
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

        return source + feel + ".";
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

        // Major family (Major, Lydian, Mixolydian). In C major this yields
        // Cadd9, Cmaj9, Csus2, Csus4, Dm9, Fmaj9, G7sus4, Am9.
        static const std::vector<ColourSlot> major {
            { 0, { Q::Add9 } },
            { 0, { Q::Major9, Q::Dominant9 } },
            { 0, { Q::Sus2 } },
            { 0, { Q::Sus4 } },
            { 1, { Q::Minor9, Q::Add9, Q::Dominant9, Q::MinorAdd9 } },
            { 3, { Q::Major9, Q::Add9, Q::Minor9, Q::MinorAdd9 } },
            { 4, { Q::Dominant7Sus4, Q::Sus4 } },
            { 5, { Q::Minor9, Q::MinorAdd9 } },
            { 6, { Q::Major9, Q::Add9 } },          // Mixolydian bVII
            { 2, { Q::Minor9, Q::MinorAdd9 } },     // Lydian iii
            { 3, { Q::Major6 } },
            { 4, { Q::Dominant9 } },
        };

        // Minor family (Natural/Harmonic/Melodic Minor, Dorian, Phrygian, Locrian). In A minor this yields
        // Am(add9), Am9, Asus2, Asus4, Cadd9, Dm9, Fmaj9, Gadd9.
        static const std::vector<ColourSlot> minor {
            { 0, { Q::MinorAdd9 } },
            { 0, { Q::Minor9, Q::Minor6 } },
            { 0, { Q::Sus2 } },
            { 0, { Q::Sus4 } },
            { 2, { Q::Add9, Q::MinorAdd9, Q::Major9, Q::Minor9 } },
            { 3, { Q::Minor9, Q::Dominant9, Q::MinorAdd9, Q::Add9 } },
            { 5, { Q::Major9, Q::Add9, Q::Major6 } },
            { 6, { Q::Add9, Q::Sus4, Q::Sus2 } },
            { 1, { Q::Major9, Q::Add9 } },          // Phrygian / Locrian bII
            { 4, { Q::Dominant7Sus4, Q::Sus4 } },
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
            ChordQuality::Major6, ChordQuality::Minor6
        };

        for (int degree : fallbackDegrees)
            for (auto q : fallbackQualities)
                tryAdd (degree, q);

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
            int degree;    // degree in the parallel key
            bool seventh;
        };

        // Ordered by how common the chord is in pop / film music.
        static const Want fromMinor[] = {   // major-family keys borrowing from the parallel minor
            { 3, false }, { 6, false }, { 5, false }, { 2, false },   // iv, bVII, bVI, bIII
            { 3, true },  { 5, true },  { 4, false }, { 0, false },   // iv7, bVImaj7, v, i
            { 1, true },  { 6, true },  { 1, false }, { 2, true },    // iiø7, bVII7, ii°, bIIImaj7
            { 4, true },  { 0, true },                                // v7, i7
        };

        static const Want fromMajor[] = {   // minor-family keys borrowing from the parallel major
            { 4, false }, { 3, false }, { 0, false }, { 4, true },    // V, IV, I, V7
            { 1, false }, { 3, true },  { 6, false }, { 1, true },    // ii, IVmaj7, vii°, ii7
            { 0, true },  { 5, false }, { 2, false }, { 6, true },    // Imaj7, vi, iii, viiø7
            { 5, true },  { 2, true },                                // vi7, iii7
        };

        const Key par = key.parallel();
        const bool parallelIsMinor = scaleInfo (par.scale).isMinorFamily;

        std::vector<PaletteChord> pads;
        for (auto& w : parallelIsMinor ? fromMinor : fromMajor)
        {
            if ((int) pads.size() >= kMaxPadsPerSection)
                break;

            const Chord c = diatonicChord (par, w.degree, w.seventh);
            if (isDiatonic (c, key) || contains (pads, c))
                continue;

            pads.push_back (makePad (c, key, borrowedHint (key, w.degree, w.seventh, names)));
        }
        return pads;
    }

    //==========================================================================
    std::vector<PaletteChord> buildSecondary (const Key& key, const std::vector<PaletteSection>& earlier)
    {
        const auto pcs = key.pitchClasses();
        std::vector<PaletteChord> pads;

        for (int target = 1; target < 7 && (int) pads.size() < kMaxPadsPerSection; ++target)
        {
            // Diminished targets are excluded by definition; augmented ones (III+ of harmonic / melodic minor) are
            // skipped too - "pulls strongly to Ebaug" is not a move anyone wants suggested.
            const Chord targetChord = diatonicChord (key, target, false);
            if (targetChord.quality == ChordQuality::Diminished || targetChord.quality == ChordQuality::Augmented)
                continue;

            const Chord c { pc (pcs[(size_t) target] + 7), ChordQuality::Dominant7 };
            if (harmonicFunction (c, key) != HarmonicFunction::Secondary || containsAnywhere (earlier, c) || contains (pads, c))
                continue;

            const auto targetName = targetChord.name (key);
            const auto hint = "Pulls strongly to " + targetName + " (" + romanNumeral (targetChord, key, false)
                            + ") - bright, chromatic momentum.";
            pads.push_back (makePad (c, key, hint));
        }
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
