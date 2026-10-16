#include "core/Generate.h"

#include "core/Suggest.h"

#include <algorithm>
#include <cmath>

// Progression starting points: mood presets and the seeded "Spark" generator.
//
// Presets
//   Each preset is a list of steps (semitones above the tonic + acceptable qualities, most wanted first). A step
//   resolves to the first quality whose chord buildPalette (key) actually shows; a preset with an unresolvable step is
//   skipped for that key, and a preset that resolves to the same chords as an earlier one is dropped. So the palette's
//   pads always light up for a preset, and chords are picked by degree / quality, never by pad index.
//   Order: the scale's own presets first (harmonic / melodic minor and the modes), then the family list,
//   which leads with what out/midi_stats.md says producers use most (minor: i-VI-III-VII, i-VII-VI-VII, VI-VII-i-i,
//   i-iv-VI-v, i-VI-iv-V; major: I-V-vi-IV, vi-IV-I-V, borrowed bVI / bVII / iv). Capped at 20.
//   With round 2's palette every scale gets 12-20 (Lydian 12, Mixolydian 13, the rest 18-20). If a palette ever lacks
//   a family's staple chords, that scale simply gets fewer presets rather than presets with chords it does not show.
//
// Spark
//   1. Candidates: every distinct palette chord, each with a mood weight = quality weight (per mood and chord quality:
//      e.g. Lo-fi m7 1.3 vs triad 0.12, Dreamy maj7 1.4, Uplifting triad 1.0 vs add9 0.5) x role weight (per mood, by
//      the chord's root offset and third, e.g. Dark minor: i 1.4, bII 1.4, VI 1.3, iv 1.1) x section weight
//      (secondary dominants 0.15-0.6, borrowed chords 0.7-1.0) x 1.4-1.5 for the mode's signature chord.
//   2. Opener: weighted pick over mood weight x mood-typical opener weight x (0.15 + openingScore)^2.
//   3. Each next chord: weighted pick among the 6 best of
//        transitionScore(prev, c)^3 x mood weight x mood pattern boost x novelty,
//      never the same chord or the same root as the previous chord. Pattern boosts: Epic i-VI-III-VII cycle (major:
//      I-bVI-bVII and vi-IV-I-V) x1.5-1.8, Lo-fi down-a-fifth ii-V moves between 7th/9th chords x1.6, Dark i <-> bII
//      x1.6, Uplifting pop-axis moves x1.3, Emotional moves into vi / i x1.3, Dreamy I <-> IV x1.2. Novelty: x0.15 per
//      earlier use of the same root (x0.45 for 6+ chords; Dark's tonic only x0.5), and another x0.4 per earlier use of
//      the very same chord.
//   4. Loop closure: the last chord is also weighted by transitionScore(last, first)^2 and avoids the first chord's root.
//   Loops of up to 5 chords first try fresh roots only (each move and the loop-back scoring >= 0.45; Dark may revisit
//   the tonic). Constraints are then relaxed step by step (used roots penalised, then the previous root allowed, then
//   anything but the same chord) only when nothing else is left, so tiny palettes still produce a progression. PRNG: xorshift32 seeded through a murmur3 finaliser of
//   (seed, key, mood, length), so consecutive seeds give unrelated progressions.

namespace cb
{
const char* moodName (Mood m)
{
    static const char* names[] = { "Uplifting", "Emotional", "Dark", "Dreamy", "Epic", "Lo-fi" };
    const int i = (int) m;
    return names[i < 0 || i >= numMoods ? 0 : i];
}

namespace
{
    using Q = ChordQuality;

    bool isMinorKey (const Key& key) { return scaleInfo (key.scale).isMinorFamily; }

    std::vector<Chord> distinctPaletteChords (const std::vector<PaletteSection>& palette)
    {
        std::vector<Chord> out;
        for (auto& s : palette)
            for (auto& pad : s.chords)
            {
                const Chord c { pc (pad.chord.root), pad.chord.quality };
                if (std::find (out.begin(), out.end(), c) == out.end())
                    out.push_back (c);
            }
        return out;
    }

    // +1 major third, -1 minor third, 0 otherwise (sus, diminished, augmented).
    int thirdOf (ChordQuality q)
    {
        switch (q)
        {
            case Q::Major: case Q::Major6: case Q::Add9: case Q::Major7: case Q::Dominant7: case Q::Major9:
            case Q::Dominant9: case Q::Major69: case Q::Dominant13: case Q::Major7Sharp11: case Q::Dominant7Flat9:
                return 1;
            case Q::Minor: case Q::Minor6: case Q::MinorAdd9: case Q::Minor7: case Q::MinorMajor7: case Q::Minor9:
            case Q::Minor11:
                return -1;
            default:
                return 0;
        }
    }

    bool isSusQuality (ChordQuality q)
    {
        return q == Q::Sus2 || q == Q::Sus4 || q == Q::Dominant7Sus4;
    }

    bool isDiminishedQuality (ChordQuality q)
    {
        return q == Q::Diminished || q == Q::Diminished7 || q == Q::HalfDiminished7;
    }

    bool isJazzQuality (ChordQuality q)   // 7ths, 9ths, 11ths, 13ths
    {
        switch (q)
        {
            case Q::Major7: case Q::Minor7: case Q::Dominant7: case Q::HalfDiminished7: case Q::Major9: case Q::Minor9:
            case Q::Dominant9: case Q::Minor11: case Q::Dominant13: case Q::Major7Sharp11: case Q::Dominant7Flat9:
            case Q::MinorMajor7:
                return true;
            default:
                return false;
        }
    }

    //==========================================================================
    // Presets
    //==========================================================================
    struct Step
    {
        int offset;                          // semitones above the tonic
        std::vector<ChordQuality> qualities; // first one the palette shows wins
        int beats;
    };

    struct PresetDef
    {
        const char* name;
        const char* description;
        Mood mood;
        std::vector<Step> steps;
    };

    Step s (int offset, std::vector<ChordQuality> qualities, int beats = 4) { return { offset, std::move (qualities), beats }; }

    // Minor-family offsets: i 0, bII 1, ii 2, III 3, iv / IV 5, v / V 7, VI 8, VII 10.
    const std::vector<PresetDef>& minorPresets()
    {
        static const std::vector<PresetDef> list {
            { "Minor pop anthem", "The most-played minor loop in modern pop, EDM and trap.", Mood::Emotional,
              { s (0, { Q::Minor }), s (8, { Q::Major }), s (3, { Q::Major }), s (10, { Q::Major }) } },
            { "Pendulum", "Rocks down a step and back up - hypnotic and moody.", Mood::Emotional,
              { s (0, { Q::Minor }), s (10, { Q::Major }), s (8, { Q::Major }), s (10, { Q::Major }) } },
            { "Rise to home", "Climbs by step and lands home - the classic minor lift.", Mood::Uplifting,
              { s (8, { Q::Major }), s (10, { Q::Major }), s (0, { Q::Minor }), s (0, { Q::Minor }) } },
            { "Minor ballad", "Soft and melancholic, with the gentle minor v instead of a hard V.", Mood::Emotional,
              { s (0, { Q::Minor }), s (5, { Q::Minor }), s (8, { Q::Major }), s (7, { Q::Minor }) } },
            { "Harmonic drama", "The major V adds a dramatic pull back home.", Mood::Dark,
              { s (0, { Q::Minor }), s (8, { Q::Major }), s (5, { Q::Minor }), s (7, { Q::Major }) } },
            { "Andalusian cadence", "Steps down to a dramatic V - flamenco, film scores and drill.", Mood::Dark,
              { s (0, { Q::Minor }), s (10, { Q::Major }), s (8, { Q::Major }), s (7, { Q::Major }) } },
            { "Lo-fi study", "Mellow minor sevenths for late-night beats.", Mood::LoFi,
              { s (0, { Q::Minor7 }), s (5, { Q::Minor7 }), s (8, { Q::Major7 }), s (7, { Q::Minor7 }) } },
            { "Epic add9 anthem", "The big minor loop with shimmering add9 colour.", Mood::Epic,
              { s (0, { Q::MinorAdd9, Q::Minor9, Q::Minor }), s (8, { Q::Add9, Q::Major9, Q::Major69, Q::Major }),
                s (3, { Q::Add9, Q::Major69, Q::Major9, Q::Major }), s (10, { Q::Add9, Q::Major6, Q::Major }) } },
            { "Dark trap", "Sinks to VI and climbs through VII back home - brooding and heavy.", Mood::Dark,
              { s (0, { Q::Minor }), s (8, { Q::Major }), s (10, { Q::Major }), s (0, { Q::Minor }) } },
            { "Phrygian menace", "The half-step bII - instant menace for trap and drill.", Mood::Dark,
              { s (0, { Q::Minor }, 8), s (1, { Q::Major }, 8) } },
            { "Trailer swell", "Suspended VII that resolves - a cinematic build.", Mood::Epic,
              { s (0, { Q::Minor }), s (8, { Q::Major }), s (10, { Q::Sus4 }), s (10, { Q::Major }) } },
            { "Royal road (minor)", "The J-pop 'royal road', told from the minor side - heartfelt and driving.", Mood::Emotional,
              { s (8, { Q::Major }), s (10, { Q::Major }), s (7, { Q::Minor }), s (0, { Q::Minor }) } },
            { "Late-night R&B", "VImaj7 into v7, the colour move producers love, in a smooth loop.", Mood::Dreamy,
              { s (8, { Q::Major7, Q::Major9 }), s (7, { Q::Minor7, Q::Minor9 }), s (0, { Q::Minor7, Q::Minor9 }),
                s (5, { Q::Minor7, Q::Minor9 }) } },
            { "Minor ii-V-i", "The jazz cadence in minor - smoky, then resolved.", Mood::LoFi,
              { s (2, { Q::HalfDiminished7, Q::Minor7 }), s (7, { Q::Dominant7, Q::Dominant7Flat9 }),
                s (0, { Q::Minor7, Q::Minor9, Q::MinorMajor7, Q::Minor6 }, 8) } },
            { "Neo-soul glide", "Lush minor ninths with a dominant pull back home.", Mood::LoFi,
              { s (0, { Q::Minor9, Q::Minor11, Q::Minor7 }), s (5, { Q::Minor9, Q::Minor11, Q::Minor7 }),
                s (8, { Q::Major9, Q::Major7 }), s (7, { Q::Dominant7, Q::Dominant9, Q::Dominant7Flat9 }) } },
            { "Hopeful minor", "Brightens through the relative major before settling.", Mood::Uplifting,
              { s (0, { Q::Minor }), s (3, { Q::Major }), s (10, { Q::Major }), s (8, { Q::Major }) } },
            { "Dorian vamp", "The raised sixth gives minor a soulful, funky lift.", Mood::LoFi,
              { s (0, { Q::Minor7, Q::Minor6 }), s (5, { Q::Major, Q::Dominant7 }) } },
            { "Circle of fifths", "Every chord falls a fifth, jazz-standard style.", Mood::Emotional,
              { s (5, { Q::Minor7 }), s (10, { Q::Dominant7, Q::Major }), s (3, { Q::Major7 }), s (8, { Q::Major7 }),
                s (2, { Q::HalfDiminished7 }), s (7, { Q::Dominant7 }), s (0, { Q::Minor7, Q::Minor }, 8) } },
            { "Minor canon", "Pachelbel's canon in a minor key.", Mood::Emotional,
              { s (0, { Q::Minor }), s (7, { Q::Minor }), s (8, { Q::Major }), s (3, { Q::Major }), s (5, { Q::Minor }),
                s (0, { Q::Minor }), s (5, { Q::Minor }), s (7, { Q::Major }) } },
        };
        return list;
    }

    // Major-family offsets: I 0, ii 2, bIII 3, iii / III 4, IV / iv 5, V / v 7, bVI 8, vi 9, bVII 10.
    const std::vector<PresetDef>& majorPresets()
    {
        static const std::vector<PresetDef> list {
            { "Pop anthem", "The four chords behind countless pop hits.", Mood::Uplifting,
              { s (0, { Q::Major }), s (7, { Q::Major }), s (9, { Q::Minor }), s (5, { Q::Major }) } },
            { "Sensitive pop", "Starts on the relative minor - bittersweet and anthemic.", Mood::Emotional,
              { s (9, { Q::Minor }), s (5, { Q::Major }), s (0, { Q::Major }), s (7, { Q::Major }) } },
            { "Doo-wop", "The 50s classic: sweet, nostalgic, timeless.", Mood::Uplifting,
              { s (0, { Q::Major }), s (9, { Q::Minor }), s (5, { Q::Major }), s (7, { Q::Major }) } },
            { "Open road", "Bright, open-hearted and easy to sing over.", Mood::Uplifting,
              { s (0, { Q::Major }), s (5, { Q::Major }), s (9, { Q::Minor }), s (7, { Q::Major }) } },
            { "Epic borrowed", "Borrowed bVI and bVII for a heroic, cinematic lift.", Mood::Epic,
              { s (0, { Q::Major }), s (8, { Q::Major }), s (10, { Q::Major }), s (0, { Q::Major }) } },
            { "Mixolydian rock", "The flat seven gives classic-rock swagger.", Mood::Uplifting,
              { s (0, { Q::Major }), s (10, { Q::Major }), s (5, { Q::Major }), s (0, { Q::Major }) } },
            { "Jazz ii-V-I", "The most important cadence in jazz.", Mood::LoFi,
              { s (2, { Q::Minor7 }), s (7, { Q::Dominant7 }), s (0, { Q::Major7 }, 8) } },
            { "Canon", "Pachelbel's canon - the wedding classic.", Mood::Emotional,
              { s (0, { Q::Major }), s (7, { Q::Major }), s (9, { Q::Minor }), s (4, { Q::Minor }), s (5, { Q::Major }),
                s (0, { Q::Major }), s (5, { Q::Major }), s (7, { Q::Major }) } },
            { "Royal road", "The J-pop favourite: rising tension into a heartfelt vi.", Mood::Emotional,
              { s (5, { Q::Major7, Q::Major }), s (7, { Q::Dominant7, Q::Major }), s (4, { Q::Minor7, Q::Minor }), s (9, { Q::Minor }) } },
            { "Heroic rise", "bVI - bVII - I: the triumphant video-game cadence.", Mood::Epic,
              { s (8, { Q::Major }), s (10, { Q::Major }), s (0, { Q::Major }, 8) } },
            { "Dreamy drift", "Two major sevenths floating back and forth.", Mood::Dreamy,
              { s (0, { Q::Major7, Q::Major9, Q::Add9 }), s (5, { Q::Major7, Q::Major9, Q::Add9 }) } },
            { "Dream pop", "Add9 shimmer melting into soft minor sevenths.", Mood::Dreamy,
              { s (0, { Q::Add9, Q::Major9 }), s (4, { Q::Minor7 }), s (9, { Q::Minor7, Q::Minor9 }), s (5, { Q::Major9, Q::Major7, Q::Add9 }) } },
            { "Lo-fi turnaround", "Smooth sevenths circling back home.", Mood::LoFi,
              { s (0, { Q::Major7 }), s (9, { Q::Minor7 }), s (2, { Q::Minor7 }), s (7, { Q::Dominant7 }) } },
            { "Neo-soul turnaround", "A secondary dominant pulls into a soulful vi7.", Mood::LoFi,
              { s (5, { Q::Major7, Q::Major9 }), s (4, { Q::Dominant7, Q::Dominant9 }), s (9, { Q::Minor7, Q::Minor9 }, 8) } },
            { "Festival lift", "The EDM-anthem rotation: lifts from IV and peaks on vi.", Mood::Uplifting,
              { s (5, { Q::Major }), s (0, { Q::Major }), s (7, { Q::Major }), s (9, { Q::Minor }) } },
            { "Minor iv heartbreak", "IV turns minor for a bittersweet sigh before home.", Mood::Emotional,
              { s (0, { Q::Major }), s (5, { Q::Major }), s (5, { Q::Minor }), s (0, { Q::Major }) } },
            { "Bittersweet turn", "A bright III and a borrowed iv - alt-rock melancholy.", Mood::Emotional,
              { s (0, { Q::Major }), s (4, { Q::Major, Q::Dominant7 }), s (5, { Q::Major }), s (5, { Q::Minor }) } },
            { "Shadowed major", "Borrowed iv, bVI and bVII cast a dark, cinematic shadow.", Mood::Dark,
              { s (0, { Q::Major }), s (5, { Q::Minor }), s (8, { Q::Major }), s (10, { Q::Major }) } },
            { "Major Andalusian", "Borrowed steps down to V - dark and dramatic.", Mood::Dark,
              { s (0, { Q::Major }), s (10, { Q::Major }), s (8, { Q::Major }), s (7, { Q::Major }) } },
        };
        return list;
    }

    const std::vector<PresetDef>& modePresets (ScaleType scale)
    {
        static const std::vector<PresetDef> none;

        // Dorian: i ii III IV v vi° VII.
        static const std::vector<PresetDef> dorian {
            { "Dorian vamp", "The raised sixth gives minor a soulful, funky lift.", Mood::LoFi,
              { s (0, { Q::Minor7 }), s (5, { Q::Dominant7, Q::Major }) } },
            { "Dorian groove", "Disco-funk Dorian: bright IV, smooth minor v.", Mood::Uplifting,
              { s (0, { Q::Minor7, Q::Minor }), s (3, { Q::Major }), s (7, { Q::Minor7, Q::Minor }), s (5, { Q::Major }) } },
            { "Modal soul", "Two minor sevenths a step apart - pure Dorian soul.", Mood::Dreamy,
              { s (0, { Q::Minor7, Q::Minor9 }), s (2, { Q::Minor7 }) } },
            { "Dorian rock", "i - VII - IV: the modal rock riff.", Mood::Epic,
              { s (0, { Q::Minor }), s (10, { Q::Major }), s (5, { Q::Major }, 8) } },
            { "Dorian circle", "Falls in fourths through the mode's bright chords.", Mood::Emotional,
              { s (0, { Q::Minor }), s (5, { Q::Major }), s (10, { Q::Major }), s (3, { Q::Major }) } },
            { "Dorian ballad", "Folk-like and wistful, with the major IV as the lift.", Mood::Emotional,
              { s (0, { Q::Minor }), s (7, { Q::Minor }), s (10, { Q::Major }), s (5, { Q::Major }) } },
            { "Modal cadence", "bVII - IV - i: a big, open modal landing.", Mood::Epic,
              { s (10, { Q::Major }), s (5, { Q::Major }), s (0, { Q::Minor }, 8) } },
            { "Dorian climb", "Steps up the mode's chords - funky and hopeful.", Mood::Uplifting,
              { s (0, { Q::Minor }), s (2, { Q::Minor }), s (3, { Q::Major }), s (5, { Q::Major }) } },
            { "Funk vamp", "Minor ninth to dominant ninth - a funk staple.", Mood::LoFi,
              { s (0, { Q::Minor9, Q::Minor11 }), s (5, { Q::Dominant9, Q::Dominant13 }) } },
        };

        // Phrygian: i bII bIII iv v° bVI bvii.
        static const std::vector<PresetDef> phrygian {
            { "Phrygian menace", "The half-step bII - instant menace for trap and drill.", Mood::Dark,
              { s (0, { Q::Minor }, 8), s (1, { Q::Major }, 8) } },
            { "Phrygian riff", "i - bII - bIII - bII: the metal and flamenco staple.", Mood::Dark,
              { s (0, { Q::Minor }), s (1, { Q::Major }), s (3, { Q::Major }), s (1, { Q::Major }) } },
            { "Flamenco descent", "Steps down to the tonic through the bII - Spanish and fiery.", Mood::Emotional,
              { s (5, { Q::Minor }), s (3, { Q::Major }), s (1, { Q::Major }), s (0, { Q::Minor }) } },
            { "Phrygian pulse", "Hovers around home with dark half-step neighbours.", Mood::Dark,
              { s (0, { Q::Minor }), s (1, { Q::Major }), s (0, { Q::Minor }), s (10, { Q::Minor }) } },
            { "Phrygian lament", "Falls through bvii and bVI to the stinging bII.", Mood::Emotional,
              { s (0, { Q::Minor }), s (10, { Q::Minor }), s (8, { Q::Major }), s (1, { Q::Major }) } },
            { "Desert night", "Lush bIImaj7 against the minor tonic.", Mood::Dreamy,
              { s (0, { Q::Minor7, Q::Minor }), s (1, { Q::Major7, Q::Major9, Q::Major }) } },
        };

        // Lydian: I II iii #iv° V vi vii.
        static const std::vector<PresetDef> lydian {
            { "Lydian float", "The bright major II - weightless, film-score wonder.", Mood::Dreamy,
              { s (0, { Q::Major7, Q::Major }), s (2, { Q::Major, Q::Add9 }) } },
            { "Lydian shimmer", "maj7#11 colour: the sound of the mode in one chord.", Mood::Dreamy,
              { s (0, { Q::Major7Sharp11, Q::Major9 }), s (2, { Q::Add9, Q::Major6, Q::Major }) } },
            { "Lydian pop", "A dreamy II replaces the usual IV.", Mood::Uplifting,
              { s (0, { Q::Major }), s (2, { Q::Major }), s (9, { Q::Minor }), s (7, { Q::Major }) } },
            { "Lydian dream", "Rocks between I, II and iii - floating and sweet.", Mood::Dreamy,
              { s (0, { Q::Major }), s (2, { Q::Major }), s (4, { Q::Minor }), s (2, { Q::Major }) } },
            { "Wonder theme", "I - II - V: the adventurous movie-theme lift.", Mood::Epic,
              { s (0, { Q::Major }), s (2, { Q::Major }), s (7, { Q::Major }, 8) } },
            { "Lydian anthem", "The pop loop with the bright II in place of IV.", Mood::Uplifting,
              { s (0, { Q::Major }), s (7, { Q::Major }), s (9, { Q::Minor }), s (2, { Q::Major }) } },
            { "Lydian lo-fi", "Major seventh, bright II7 and soft minor sevenths.", Mood::LoFi,
              { s (0, { Q::Major7 }), s (2, { Q::Dominant7, Q::Major }), s (4, { Q::Minor7 }), s (9, { Q::Minor7 }) } },
            { "Lydian descent", "Steps down from the bright II - wistful and floating.", Mood::Emotional,
              { s (2, { Q::Major }), s (0, { Q::Major }), s (11, { Q::Minor }), s (9, { Q::Minor }) } },
        };

        // Mixolydian: I ii iii° IV v vi bVII.
        static const std::vector<PresetDef> mixolydian {
            { "Mixolydian vamp", "I to bVII and back - the sound of the mode.", Mood::Uplifting,
              { s (0, { Q::Major }, 8), s (10, { Q::Major }, 8) } },
            { "Mixolydian rock", "The flat seven gives classic-rock swagger.", Mood::Uplifting,
              { s (0, { Q::Major }), s (10, { Q::Major }), s (5, { Q::Major }), s (0, { Q::Major }) } },
            { "Southern rock", "I - bVII - IV: the guitar anthem.", Mood::Uplifting,
              { s (0, { Q::Major }), s (10, { Q::Major }), s (5, { Q::Major }, 8) } },
            { "Mixolydian groove", "The minor v keeps it laid-back and open.", Mood::Dreamy,
              { s (0, { Q::Major }), s (7, { Q::Minor }), s (10, { Q::Major }), s (5, { Q::Major }) } },
            { "Modal plagal", "bVII - IV - I: a wide, open landing.", Mood::Epic,
              { s (10, { Q::Major }), s (5, { Q::Major }), s (0, { Q::Major }, 8) } },
            { "Bluesy vamp", "Dominant sevenths on I and IV - blues and funk.", Mood::LoFi,
              { s (0, { Q::Dominant7, Q::Dominant9 }), s (5, { Q::Dominant7, Q::Major }) } },
        };

        // Harmonic minor: i ii° III+ iv V VI vii° (plus the natural-minor chords the palette borrows).
        static const std::vector<PresetDef> harmonicMinor {
            { "Harmonic drama", "The major V adds a dramatic pull back home.", Mood::Dark,
              { s (0, { Q::Minor }), s (8, { Q::Major }), s (5, { Q::Minor }), s (7, { Q::Major }) } },
            { "Classical minor", "i - iv - i - V: the cadence of every minor-key classic.", Mood::Dark,
              { s (0, { Q::Minor }), s (5, { Q::Minor }), s (0, { Q::Minor }), s (7, { Q::Major, Q::Dominant7 }) } },
            { "Drill bounce", "Minor tonic, VI and a biting V7b9 - the drill and trap staple.", Mood::Dark,
              { s (0, { Q::Minor }), s (8, { Q::Major }), s (7, { Q::Dominant7Flat9, Q::Dominant7, Q::Major }, 8) } },
            { "Gothic tension", "The diminished seventh sharpens the pull home.", Mood::Dark,
              { s (0, { Q::Minor }), s (11, { Q::Diminished7, Q::Diminished }), s (0, { Q::Minor }), s (7, { Q::Major }) } },
            { "Minor cadence", "ii° - V - i: the classical minor resolution.", Mood::Emotional,
              { s (2, { Q::Diminished }), s (7, { Q::Major }), s (0, { Q::Minor }, 8) } },
            { "Line cliche", "A falling inner line turns the tonic into a slow, cinematic sigh.", Mood::Emotional,
              { s (0, { Q::Minor }), s (0, { Q::MinorMajor7 }), s (0, { Q::Minor7 }), s (8, { Q::Major7, Q::Major }) } },
        };

        // Melodic minor: i ii III+ IV V vi° vii° (jazz minor).
        static const std::vector<PresetDef> melodicMinor {
            { "Jazz minor vamp", "Minor sixth against a bright IV7 - smoky jazz minor.", Mood::LoFi,
              { s (0, { Q::Minor6, Q::Minor }), s (5, { Q::Dominant7, Q::Dominant9 }) } },
            { "Melodic cadence", "ii7 - V7 - i(maj7): a jazz resolution with a mysterious landing.", Mood::LoFi,
              { s (2, { Q::Minor7 }), s (7, { Q::Dominant7 }), s (0, { Q::MinorMajor7, Q::Minor6 }, 8) } },
            { "Bright minor", "Major IV and V lift the minor tonic.", Mood::Uplifting,
              { s (0, { Q::Minor }), s (5, { Q::Major }), s (7, { Q::Major }), s (0, { Q::Minor }) } },
            { "Line cliche", "The tonic's falling inner line - Am, Am(maj7), Am7, Am6 style.", Mood::Emotional,
              { s (0, { Q::Minor }), s (0, { Q::MinorMajor7 }), s (0, { Q::Minor7 }), s (0, { Q::Minor6 }) } },
            { "Film noir", "Minor-major seventh and dominant colours - mysterious and smoky.", Mood::Dark,
              { s (0, { Q::MinorMajor7 }), s (5, { Q::Dominant7, Q::Dominant9 }), s (0, { Q::Minor6 }), s (7, { Q::Dominant7 }) } },
            { "Melodic minor rise", "Climbs i - ii - IV - V with the scale's raised steps.", Mood::Epic,
              { s (0, { Q::Minor }), s (2, { Q::Minor }), s (5, { Q::Major }), s (7, { Q::Major }) } },
        };

        // Locrian: i° bII biii iv bV bVI bvii (rarely used; kept short and unstable).
        static const std::vector<PresetDef> locrian {
            { "Locrian tension", "The diminished home and its bII - pure unease.", Mood::Dark,
              { s (0, { Q::Diminished }, 8), s (1, { Q::Major }, 8) } },
            { "Locrian climb", "Edges upward without ever settling.", Mood::Dark,
              { s (0, { Q::Diminished }), s (1, { Q::Major }), s (3, { Q::Minor }), s (1, { Q::Major }) } },
            { "Locrian shadow", "bVI - bvii - i°: a dark, unresolved landing.", Mood::Dark,
              { s (8, { Q::Major }), s (10, { Q::Minor }), s (0, { Q::Diminished }, 8) } },
            { "Locrian vamp", "Half-diminished home against a lush bIImaj7.", Mood::LoFi,
              { s (0, { Q::HalfDiminished7 }), s (1, { Q::Major7 }) } },
            { "Locrian descent", "Falls from home through bvii and bVI to the bII.", Mood::Emotional,
              { s (0, { Q::Diminished }), s (10, { Q::Minor }), s (8, { Q::Major }), s (1, { Q::Major }) } },
            { "Locrian drift", "bII and biii rock gently over the unstable home.", Mood::Dreamy,
              { s (1, { Q::Major7, Q::Major }), s (3, { Q::Minor7, Q::Minor }) } },
        };

        switch (scale)
        {
            case ScaleType::Dorian:     return dorian;
            case ScaleType::Phrygian:   return phrygian;
            case ScaleType::Lydian:     return lydian;
            case ScaleType::Mixolydian: return mixolydian;
            case ScaleType::Locrian:       return locrian;
            case ScaleType::HarmonicMinor: return harmonicMinor;
            case ScaleType::MelodicMinor:  return melodicMinor;
            case ScaleType::Major:
            case ScaleType::NaturalMinor:
            case ScaleType::NumScales:
                break;
        }
        return none;
    }

    bool resolvePreset (const PresetDef& def, const Key& key, const std::vector<Chord>& available, Progression& out)
    {
        out.slots.clear();
        for (auto& step : def.steps)
        {
            const int root = pc (key.tonic + step.offset);
            bool found = false;
            for (auto q : step.qualities)
            {
                const Chord c { root, q };
                if (std::find (available.begin(), available.end(), c) != available.end())
                {
                    ProgressionSlot slot;
                    slot.chord = c;
                    slot.lengthBeats = std::clamp (step.beats, kMinSlotBeats, kMaxSlotBeats);
                    out.slots.push_back (slot);
                    found = true;
                    break;
                }
            }
            if (! found || (int) out.slots.size() > kMaxSlots)
                return false;
        }
        return ! out.slots.empty();
    }

    std::string romanSummaryOf (const Progression& p, const Key& key)
    {
        std::string out;
        for (auto& slot : p.slots)
        {
            if (! out.empty())
                out += " - ";
            out += romanNumeral (slot.chord, key);
        }
        return out;
    }

    //==========================================================================
    // Spark
    //==========================================================================
    struct Rng
    {
        std::uint32_t state;

        explicit Rng (std::uint32_t seed)
        {
            // murmur3 finaliser so that neighbouring seeds start far apart
            seed ^= seed >> 16; seed *= 0x85EBCA6Bu;
            seed ^= seed >> 13; seed *= 0xC2B2AE35u;
            seed ^= seed >> 16;
            state = seed != 0 ? seed : 0x6D2B79F5u;
        }

        std::uint32_t next()
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return state;
        }

        float uniform() { return (float) (next() >> 8) * (1.0f / 16777216.0f); }   // [0, 1)
    };

    // Quality weight per mood, indexed by ChordQuality.
    //                                  Maj   min   dim   aug   sus2  sus4  6     m6    add9  madd9 maj7  m7    7     m7b5  dim7  mmaj7 7sus4 maj9  m9    9     m11   6/9   13    #11   7b9
    constexpr float kQualityWeights[numMoods][numChordQualities] = {
        /* Uplifting */ { 1.0f, 1.0f, .08f, .02f, .35f, .30f, .30f, .10f, .50f, .40f, .15f, .15f, .12f, .05f, .03f, .02f, .15f, .10f, .08f, .05f, .05f, .20f, .03f, .03f, .02f },
        /* Emotional */ { 1.0f, 1.0f, .10f, .03f, .50f, .45f, .30f, .20f, .55f, .55f, .30f, .30f, .12f, .10f, .05f, .05f, .25f, .20f, .20f, .08f, .12f, .20f, .05f, .05f, .05f },
        /* Dark      */ { 1.0f, 1.0f, .25f, .10f, .20f, .30f, .05f, .35f, .10f, .45f, .10f, .45f, .25f, .30f, .20f, .30f, .20f, .05f, .45f, .10f, .40f, .03f, .05f, .03f, .35f },
        /* Dreamy    */ { .30f, .35f, .03f, .05f, .60f, .35f, 1.0f, .50f, 1.3f, 1.0f, 1.4f, 1.0f, .15f, .10f, .02f, .25f, .35f, 1.4f, 1.0f, .20f, .90f, 1.2f, .20f, 1.1f, .05f },
        /* Epic      */ { 1.0f, 1.0f, .05f, .05f, .55f, .60f, .10f, .05f, .60f, .55f, .12f, .12f, .15f, .05f, .05f, .03f, .30f, .12f, .12f, .08f, .08f, .10f, .05f, .05f, .05f },
        /* Lo-fi     */ { .12f, .12f, .05f, .02f, .25f, .20f, .50f, .40f, .60f, .60f, 1.3f, 1.3f, .90f, .90f, .30f, .35f, .70f, 1.3f, 1.4f, 1.1f, 1.4f, .90f, 1.0f, .60f, .60f },
    };

    struct RolePref
    {
        int offset;
        int third;   // +1 major, -1 minor, 0 any
        float weight;
    };

    template <size_t N>
    float lookupRole (const RolePref (&prefs)[N], int offset, int third, float fallback)
    {
        for (auto& r : prefs)
            if (r.offset == offset && (r.third == 0 || r.third == third))
                return r.weight;
        return fallback;
    }

    // Role weight by root offset and third, per mood and key family.
    float roleWeight (Mood mood, bool minorKey, int offset, int third)
    {
        static const RolePref upliftingMajor[] = { { 0, 1, 1.3f }, { 5, 1, 1.2f }, { 7, 1, 1.2f }, { 9, -1, 1.0f }, { 2, -1, 0.6f }, { 4, -1, 0.4f },
                                                   { 10, 1, 0.5f }, { 8, 1, 0.4f }, { 5, -1, 0.25f }, { 0, -1, 0.1f }, { 2, 1, 0.6f }, { 7, -1, 0.5f } };
        static const RolePref upliftingMinor[] = { { 8, 1, 1.3f }, { 10, 1, 1.3f }, { 3, 1, 1.2f }, { 0, -1, 1.0f }, { 5, -1, 0.6f }, { 7, -1, 0.5f },
                                                   { 5, 1, 0.8f }, { 7, 1, 0.5f }, { 0, 1, 0.4f }, { 2, -1, 0.5f }, { 1, 1, 0.3f } };
        static const RolePref emotionalMajor[] = { { 9, -1, 1.4f }, { 5, 1, 1.2f }, { 0, 1, 1.0f }, { 7, 1, 0.9f }, { 4, -1, 0.7f }, { 2, -1, 0.6f },
                                                   { 5, -1, 0.9f }, { 8, 1, 0.6f }, { 10, 1, 0.5f }, { 0, -1, 0.3f } };
        static const RolePref emotionalMinor[] = { { 0, -1, 1.3f }, { 8, 1, 1.3f }, { 10, 1, 1.0f }, { 3, 1, 0.9f }, { 5, -1, 1.0f }, { 7, -1, 0.7f },
                                                   { 7, 1, 0.5f }, { 5, 1, 0.5f }, { 2, -1, 0.5f } };
        static const RolePref darkMajor[] = { { 9, -1, 1.2f }, { 5, -1, 1.2f }, { 8, 1, 1.2f }, { 10, 1, 1.0f }, { 0, -1, 1.0f }, { 7, -1, 0.9f },
                                              { 4, -1, 0.8f }, { 3, 1, 0.7f }, { 0, 1, 0.5f }, { 5, 1, 0.4f }, { 7, 1, 0.5f }, { 2, -1, 0.6f } };
        static const RolePref darkMinor[] = { { 0, -1, 1.4f }, { 8, 1, 1.3f }, { 10, 1, 1.0f }, { 5, -1, 1.1f }, { 7, -1, 1.0f }, { 1, 1, 1.4f },
                                              { 7, 1, 0.8f }, { 3, 1, 0.5f }, { 5, 1, 0.3f }, { 0, 1, 0.1f }, { 2, -1, 0.4f }, { 10, -1, 0.8f } };
        static const RolePref dreamyMajor[] = { { 0, 1, 1.2f }, { 5, 1, 1.3f }, { 9, -1, 1.0f }, { 4, -1, 0.9f }, { 2, -1, 0.8f }, { 7, 1, 0.5f },
                                                { 2, 1, 1.0f }, { 8, 1, 0.5f }, { 5, -1, 0.5f }, { 10, 1, 0.5f } };
        static const RolePref dreamyMinor[] = { { 8, 1, 1.3f }, { 3, 1, 1.2f }, { 0, -1, 1.0f }, { 5, -1, 0.9f }, { 10, 1, 0.8f }, { 7, -1, 0.8f },
                                                { 5, 1, 0.8f }, { 2, -1, 0.8f }, { 7, 1, 0.3f }, { 1, 1, 0.8f } };
        static const RolePref epicMajor[] = { { 0, 1, 1.2f }, { 8, 1, 1.3f }, { 10, 1, 1.3f }, { 9, -1, 1.1f }, { 5, 1, 1.0f }, { 7, 1, 1.0f },
                                              { 4, -1, 0.4f }, { 2, -1, 0.4f }, { 5, -1, 0.6f }, { 3, 1, 0.6f } };
        static const RolePref epicMinor[] = { { 0, -1, 1.3f }, { 8, 1, 1.4f }, { 3, 1, 1.3f }, { 10, 1, 1.4f }, { 5, -1, 0.7f }, { 7, -1, 0.4f },
                                              { 7, 1, 0.6f }, { 5, 1, 0.5f } };
        static const RolePref lofiMajor[] = { { 2, -1, 1.3f }, { 7, 1, 1.1f }, { 0, 1, 1.1f }, { 9, -1, 1.1f }, { 5, 1, 1.1f }, { 4, -1, 0.9f },
                                              { 5, -1, 0.5f }, { 10, 1, 0.5f }, { 2, 1, 0.6f }, { 4, 1, 0.6f } };
        static const RolePref lofiMinor[] = { { 0, -1, 1.2f }, { 5, -1, 1.2f }, { 7, -1, 1.0f }, { 8, 1, 1.2f }, { 3, 1, 1.0f }, { 10, 1, 0.8f },
                                              { 2, 0, 0.9f }, { 7, 1, 0.7f }, { 5, 1, 0.8f } };

        switch (mood)
        {
            case Mood::Uplifting: return minorKey ? lookupRole (upliftingMinor, offset, third, 0.4f) : lookupRole (upliftingMajor, offset, third, 0.4f);
            case Mood::Emotional: return minorKey ? lookupRole (emotionalMinor, offset, third, 0.4f) : lookupRole (emotionalMajor, offset, third, 0.4f);
            case Mood::Dark:      return minorKey ? lookupRole (darkMinor, offset, third, 0.4f)      : lookupRole (darkMajor, offset, third, 0.4f);
            case Mood::Dreamy:    return minorKey ? lookupRole (dreamyMinor, offset, third, 0.4f)    : lookupRole (dreamyMajor, offset, third, 0.4f);
            case Mood::Epic:      return minorKey ? lookupRole (epicMinor, offset, third, 0.4f)      : lookupRole (epicMajor, offset, third, 0.4f);
            case Mood::LoFi:      return minorKey ? lookupRole (lofiMinor, offset, third, 0.5f)      : lookupRole (lofiMajor, offset, third, 0.5f);
            case Mood::NumMoods:  break;
        }
        return 0.4f;
    }

    // How typical a chord is as the first chord for the mood (0.03 = untypical).
    float openerWeight (Mood mood, bool minorKey, int offset, int third)
    {
        auto is = [offset, third] (int o, int t) { return offset == o && third == t; };

        if (minorKey)
        {
            switch (mood)
            {
                case Mood::Uplifting: return is (0, -1) ? 0.6f : is (8, 1) ? 0.5f : is (3, 1) ? 0.35f : 0.03f;
                case Mood::Emotional: return is (0, -1) ? 1.0f : is (8, 1) ? 0.6f : 0.03f;
                case Mood::Dark:      return is (0, -1) ? 1.0f : is (8, 1) ? 0.2f : 0.03f;
                case Mood::Dreamy:    return is (8, 1) ? 0.7f : is (0, -1) ? 0.7f : is (3, 1) ? 0.4f : 0.03f;
                case Mood::Epic:      return is (0, -1) ? 1.0f : is (8, 1) ? 1.0f : 0.03f;
                case Mood::LoFi:      return is (0, -1) ? 1.0f : is (5, -1) ? 0.4f : is (8, 1) ? 0.5f : 0.03f;
                case Mood::NumMoods:  break;
            }
        }
        else
        {
            switch (mood)
            {
                case Mood::Uplifting: return is (0, 1) ? 1.0f : is (5, 1) ? 0.35f : is (9, -1) ? 0.2f : 0.03f;
                case Mood::Emotional: return is (9, -1) ? 0.8f : is (0, 1) ? 0.6f : is (5, 1) ? 0.35f : 0.03f;
                case Mood::Dark:      return is (9, -1) ? 1.0f : is (0, -1) ? 0.6f : is (5, -1) ? 0.3f : 0.03f;
                case Mood::Dreamy:    return is (0, 1) ? 1.0f : is (5, 1) ? 0.6f : is (9, -1) ? 0.3f : 0.03f;
                case Mood::Epic:      return is (0, 1) ? 0.7f : is (9, -1) ? 0.6f : is (8, 1) ? 0.7f : 0.03f;
                case Mood::LoFi:      return is (2, -1) ? 0.8f : is (0, 1) ? 0.8f : is (5, 1) ? 0.5f : is (9, -1) ? 0.4f : 0.03f;
                case Mood::NumMoods:  break;
            }
        }
        return 0.03f;
    }

    // The mode's signature chord (Dorian IV, Phrygian bII, Lydian II, Mixolydian bVII) gets a boost.
    float signatureBoost (const Key& key, int offset, int third)
    {
        switch (key.scale)
        {
            case ScaleType::Dorian:     return offset == 5 && third == 1 ? 1.4f : 1.0f;
            case ScaleType::Phrygian:   return offset == 1 && third == 1 ? 1.5f : 1.0f;
            case ScaleType::Lydian:     return offset == 2 && third == 1 ? 1.5f : 1.0f;
            case ScaleType::Mixolydian: return offset == 10 && third == 1 ? 1.4f : 1.0f;
            case ScaleType::Major:
            case ScaleType::NaturalMinor:
            case ScaleType::HarmonicMinor:
            case ScaleType::MelodicMinor:
            case ScaleType::Locrian:
            case ScaleType::NumScales:
                break;
        }
        return 1.0f;
    }

    struct Candidate
    {
        Chord chord;
        int offset;   // root, semitones above the tonic
        int third;    // +1 / -1 / 0
        float mood;   // mood weight (quality x role x section x signature)
    };

    float sectionWeight (Mood mood, const Chord& c, const Key& key)
    {
        const auto f = harmonicFunction (c, key);
        if (f == HarmonicFunction::Secondary)
            return mood == Mood::LoFi ? 0.6f : (mood == Mood::Emotional || mood == Mood::Dreamy || mood == Mood::Epic ? 0.2f : 0.15f);

        if (f == HarmonicFunction::Borrowed)
        {
            // Natural-minor chords inside harmonic / melodic minor, Dorian ... are native minor-key sounds.
            if (isMinorKey (key) && key.scale != ScaleType::NaturalMinor && isDiatonic (c, { key.tonic, ScaleType::NaturalMinor }))
                return 0.85f;
            return (mood == Mood::Dark || mood == Mood::Epic) && ! isMinorKey (key) ? 1.0f : 0.7f;
        }

        if (f == HarmonicFunction::Chromatic)
            return 0.1f;
        return 1.0f;
    }

    // Mood-specific boosts for well-loved moves (offsets of the two roots).
    float patternBoost (Mood mood, bool minorKey, const Candidate& a, const Candidate& b)
    {
        const int fa = a.offset, fb = b.offset;
        auto move = [&] (int x, int tx, int y, int ty) { return fa == x && a.third == tx && fb == y && b.third == ty; };

        switch (mood)
        {
            case Mood::Epic:
                if (minorKey)
                {
                    if (move (0, -1, 8, 1) || move (8, 1, 3, 1) || move (3, 1, 10, 1) || move (10, 1, 0, -1)) return 1.8f;
                    if (move (8, 1, 10, 1)) return 1.5f;
                }
                else
                {
                    if (move (0, 1, 8, 1) || move (8, 1, 10, 1) || move (10, 1, 0, 1)) return 1.8f;
                    if (move (9, -1, 5, 1) || move (5, 1, 0, 1) || move (0, 1, 7, 1) || move (7, 1, 9, -1)) return 1.5f;
                }
                return 1.0f;

            case Mood::LoFi:
                if (pc (fb - fa) == 5 && isJazzQuality (a.chord.quality) && isJazzQuality (b.chord.quality)) return 1.6f;
                return 1.0f;

            case Mood::Dark:
                if (minorKey && (move (0, -1, 1, 1) || move (1, 1, 0, -1))) return 1.6f;
                if (minorKey && (move (0, -1, 8, 1) || move (10, 1, 0, -1) || move (5, -1, 0, -1) || move (7, -1, 8, 1))) return 1.2f;
                if (! minorKey && (move (9, -1, 5, -1) || move (5, -1, 8, 1) || move (8, 1, 10, 1))) return 1.3f;
                return 1.0f;

            case Mood::Uplifting:
                if (! minorKey && (move (0, 1, 7, 1) || move (7, 1, 9, -1) || move (9, -1, 5, 1) || move (5, 1, 0, 1)
                                   || move (0, 1, 5, 1) || move (5, 1, 7, 1)))
                    return 1.3f;
                if (minorKey && (move (8, 1, 10, 1) || move (10, 1, 0, -1) || move (0, -1, 8, 1) || move (3, 1, 10, 1)))
                    return 1.3f;
                return 1.0f;

            case Mood::Emotional:
                if (! minorKey && fb == 9 && b.third == -1) return 1.3f;
                if (minorKey && fb == 0 && b.third == -1) return 1.3f;
                if (! minorKey && fb == 5 && b.third == -1) return 1.3f;   // borrowed iv
                return 1.0f;

            case Mood::Dreamy:
                if (! minorKey && ((fa == 0 && fb == 5) || (fa == 5 && fb == 0))) return 1.2f;
                if (minorKey && ((fa == 8 && fb == 3) || (fa == 0 && fb == 8) || (fa == 3 && fb == 8))) return 1.2f;
                return 1.0f;

            case Mood::NumMoods:
                break;
        }
        return 1.0f;
    }

    // Weighted pick among the `topK` heaviest entries; -1 when every weight is zero.
    int weightedPick (std::vector<std::pair<float, int>>& weighted, Rng& rng, size_t topK)
    {
        weighted.erase (std::remove_if (weighted.begin(), weighted.end(), [] (const std::pair<float, int>& w) { return ! (w.first > 0.0f); }),
                        weighted.end());
        if (weighted.empty())
            return -1;

        std::stable_sort (weighted.begin(), weighted.end(),
                          [] (const std::pair<float, int>& x, const std::pair<float, int>& y) { return x.first > y.first; });
        if (weighted.size() > topK)
            weighted.resize (topK);

        float total = 0.0f;
        for (auto& w : weighted)
            total += w.first;

        float r = rng.uniform() * total;
        for (auto& w : weighted)
        {
            r -= w.first;
            if (r < 0.0f)
                return w.second;
        }
        return weighted.back().second;
    }
} // namespace

//==============================================================================
std::vector<Preset> presetsFor (const Key& key)
{
    const Key k { pc (key.tonic), key.scale };
    const auto palette = buildPalette (k);
    const auto available = distinctPaletteChords (palette);

    std::vector<const PresetDef*> defs;
    for (auto& d : modePresets (k.scale))
        defs.push_back (&d);
    for (auto& d : isMinorKey (k) ? minorPresets() : majorPresets())
        defs.push_back (&d);

    constexpr size_t kMaxPresets = 20;
    std::vector<Preset> out;

    for (auto* def : defs)
    {
        if (out.size() >= kMaxPresets)
            break;

        Progression p;
        if (! resolvePreset (*def, k, available, p))
            continue;

        bool duplicate = false;
        for (auto& existing : out)
            if (existing.progression == p || existing.name == def->name)
                duplicate = true;
        if (duplicate)
            continue;

        Preset preset;
        preset.name = def->name;
        preset.description = def->description;
        preset.romanSummary = romanSummaryOf (p, k);
        preset.mood = def->mood;
        preset.progression = std::move (p);
        out.push_back (std::move (preset));
    }
    return out;
}

Progression sparkProgression (const Key& keyIn, const std::vector<PaletteSection>& palette, std::uint32_t seed,
                              const SparkSettings& settings)
{
    const Key key { pc (keyIn.tonic), keyIn.scale };
    const int numChords = std::clamp (settings.numChords, 2, std::min (8, kMaxSlots));
    const int beats = std::clamp (settings.beatsPerChord, kMinSlotBeats, kMaxSlotBeats);
    const int moodIndex = std::clamp ((int) settings.mood, 0, numMoods - 1);
    const Mood mood = (Mood) moodIndex;
    const bool minorKey = isMinorKey (key);

    Progression result;
    const auto chords = distinctPaletteChords (palette);
    if (chords.empty())
        return result;

    std::vector<Candidate> cands;
    cands.reserve (chords.size());
    for (auto& c : chords)
    {
        Candidate cand;
        cand.chord = c;
        cand.offset = pc (c.root - key.tonic);
        cand.third = thirdOf (c.quality);
        if (cand.third == 0 && isSusQuality (c.quality) && key.contains (c.root))   // Asus2 in A minor plays the i role
            cand.third = thirdOf (diatonicChord (key, key.degreeOf (c.root), false).quality);
        const int qi = std::clamp ((int) c.quality, 0, numChordQualities - 1);
        cand.mood = kQualityWeights[moodIndex][qi]
                  * roleWeight (mood, minorKey, cand.offset, cand.third)
                  * sectionWeight (mood, c, key)
                  * signatureBoost (key, cand.offset, cand.third);
        if (isDiminishedQuality (c.quality) && cand.offset != 0)
            cand.mood *= 0.5f;
        cands.push_back (cand);
    }

    std::uint32_t mixed = seed * 0x9E3779B1u;
    mixed ^= (std::uint32_t) (key.tonic * 131 + (int) key.scale * 17 + moodIndex * 7919 + numChords * 104729);
    Rng rng (mixed);

    std::vector<int> picked;
    std::vector<std::pair<float, int>> weighted;

    // 1. Opener
    for (int i = 0; i < (int) cands.size(); ++i)
    {
        const auto& c = cands[(size_t) i];
        const float open = 0.15f + openingScore (c.chord, key);
        weighted.push_back ({ c.mood * openerWeight (mood, minorKey, c.offset, c.third) * open * open, i });
    }
    int first = weightedPick (weighted, rng, 6);
    if (first < 0)
        first = 0;
    picked.push_back (first);

    // 2. Chain
    const bool longLoop = numChords >= 6;
    for (int pos = 1; pos < numChords; ++pos)
    {
        const auto& prev = cands[(size_t) picked.back()];
        const auto& head = cands[(size_t) first];
        const bool last = pos == numChords - 1;
        int choice = -1;

        // Pass 0 (loops of up to 5 chords): only fresh roots that follow well (transition >= 0.45; Dark may return to
        // the tonic). Pass 1: all rules, used chords / roots penalised. Pass 2: allow the previous root (not the same
        // chord). Pass 3: anything but the same chord.
        for (int pass = longLoop ? 1 : 0; pass < 4 && choice < 0; ++pass)
        {
            weighted.clear();
            for (int i = 0; i < (int) cands.size(); ++i)
            {
                const auto& c = cands[(size_t) i];
                if (c.chord == prev.chord)
                    continue;
                if (pass <= 1 && c.chord.root == prev.chord.root)
                    continue;

                int chordUses = 0, rootUses = 0;
                for (int p : picked)
                {
                    chordUses += cands[(size_t) p].chord == c.chord ? 1 : 0;
                    rootUses += cands[(size_t) p].chord.root == c.chord.root ? 1 : 0;
                }
                const bool usedChord = chordUses > 0, usedRoot = rootUses > 0;

                // Dark loops like to fall back onto the tonic mid-loop (i - bII - i - VI), so the tonic is let off lightly.
                const bool darkTonic = mood == Mood::Dark && c.offset == 0;
                const float t = transitionScore (prev.chord, c.chord, key);
                if (pass == 0 && ((usedRoot && ! darkTonic) || usedChord || t < 0.45f))
                    continue;

                float w = t * t * t * (t < 0.3f ? 0.05f : 1.0f);
                w *= pass < 3 ? c.mood : std::max (c.mood, 0.05f);
                w *= patternBoost (mood, minorKey, prev, c);

                // Each earlier use of the root multiplies the penalty, so 8-chord loops spread over more roots.
                const float rootPenalty = darkTonic ? 0.5f : (longLoop ? 0.45f : 0.15f);
                for (int u = 0; u < rootUses; ++u)
                    w *= rootPenalty;
                for (int u = 0; u < chordUses; ++u)
                    w *= 0.4f;

                if (last)
                {
                    const float closure = transitionScore (c.chord, head.chord, key);
                    if (pass == 0 && closure < 0.45f)
                        continue;
                    w *= closure * closure * (closure < 0.3f ? 0.1f : 1.0f);
                    if (c.chord.root == head.chord.root)
                        w *= pass <= 1 ? 0.0f : 0.05f;
                }

                if (pass == 3)
                    w = std::max (w, 1.0e-6f);

                weighted.push_back ({ w, i });
            }
            choice = weightedPick (weighted, rng, 6);
        }

        if (choice < 0)
            break;   // a one-chord palette
        picked.push_back (choice);
    }

    for (int i : picked)
    {
        ProgressionSlot slot;
        slot.chord = cands[(size_t) i].chord;
        slot.lengthBeats = beats;
        result.slots.push_back (slot);
    }
    return result;
}

} // namespace cb
