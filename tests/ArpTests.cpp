#include "TestFramework.h"

#include "core/Arp.h"
#include "core/Melody.h"
#include "core/Voicing.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <string>

// Arp layer (owner: WP2; final-spec 4.3, 9 WP2). WP0 seeded this file with checks of the frozen shape / rhythm data;
// the rest tests renderArp against properties of the spec and a small reference model of the register rule.

using namespace cb;

TEST_CASE ("arp: WP0 shape and rhythm data")
{
    CHECK_EQ (numArpShapes, 17);
    CHECK_EQ (numArpRhythms, 7);
    CHECK_EQ (std::string (arpShapeName (ArpShape::Up3)), std::string ("Up 3"));
    CHECK_EQ (std::string (arpShapeName (ArpShape::Alternate15)), std::string ("Alternate 1-5"));
    CHECK_EQ (std::string (arpShapeFamily (ArpShape::Broken)), std::string ("Ladders"));
    CHECK_EQ (std::string (arpShapeFamily (ArpShape::RootPulse)), std::string ("Pedal & bounce"));
    CHECK_EQ (std::string (arpShapeFamily (ArpShape::Alternate13)), std::string ("Two-note"));
    CHECK_EQ (arpShapeTones (ArpShape::TwoOctaves).size(), (size_t) 6);
    CHECK (arpShapeTones (ArpShape::Down4).front() == (ArpTone { 0, 1 }));
    CHECK (arpShapeFreeRuns (ArpShape::Down3));
    CHECK (! arpShapeFreeRuns (ArpShape::Up4));

    CHECK (! arpStepSounds (ArpRhythm::ThreeOnOneOff, 3, 16));
    CHECK (arpStepSounds (ArpRhythm::ThreeOnOneOff, 4, 16));
    CHECK (! arpStepSounds (ArpRhythm::Breath, 15, 16));
    CHECK (! arpStepSounds (ArpRhythm::LateStart, 0, 8));
    CHECK_EQ (std::string (arpRhythmName (ArpRhythm::ThreeOnOneOff)), std::string ("3 on, 1 off"));
}

namespace
{
constexpr double kTol = 1.0e-9;

bool near (double a, double b, double tol = kTol) { return std::abs (a - b) <= tol; }

// Lengths come from the float gate (0.9f is not 0.9): compare them to a micro-beat.
bool nearLen (double a, double b) { return std::abs (a - b) <= 1.0e-6; }

Chord chordOf (int root, ChordQuality quality = ChordQuality::Major) { return Chord { root, quality }; }

Progression makeProgression (const std::vector<Chord>& chords, const std::vector<int>& lengths = {})
{
    Progression p;
    for (size_t i = 0; i < chords.size(); ++i)
    {
        ProgressionSlot s;
        s.chord = chords[i];
        s.lengthBeats = i < lengths.size() ? lengths[i] : 4;
        p.slots.push_back (s);
    }
    return p;
}

Progression cAmFG() { return makeProgression ({ chordOf (0), chordOf (9, ChordQuality::Minor), chordOf (5), chordOf (7) }); }

// 14 beats in slots of 1, 2, 3, 5 and 3 beats.
Progression oddSlots()
{
    return makeProgression ({ chordOf (0, ChordQuality::Major9), chordOf (9, ChordQuality::Minor), chordOf (5, ChordQuality::Add9),
                              chordOf (7, ChordQuality::Dominant7), chordOf (2, ChordQuality::Sus2) },
                            { 1, 2, 3, 5, 3 });
}

// Sus / 7th / 6/9 / m11 / 13 chords in slots of 3, 1, 2, 5 and 3 beats.
Progression colourChords()
{
    return makeProgression ({ chordOf (7, ChordQuality::Dominant7Sus4), chordOf (0, ChordQuality::Major7),
                              chordOf (5, ChordQuality::Major69), chordOf (2, ChordQuality::Minor11),
                              chordOf (7, ChordQuality::Dominant13) },
                            { 3, 1, 2, 5, 3 });
}

RenderSettings feelOf (RateDivision chordRate = RateDivision::Eighth, float swing = 0.0f, float humanize = 0.0f)
{
    RenderSettings s;
    s.pattern = RhythmPattern::Block;
    s.rate = chordRate;
    s.bass = BassMode::Off;
    s.velocity = 0.75f;
    s.gate = 0.9f;
    s.swing = swing;
    s.humanize = humanize;
    return s;
}

ArpSettings arpOf (ArpShape shape, RateDivision rate = RateDivision::Sixteenth, ArpRhythm rhythm = ArpRhythm::EveryStep,
                   int centre = 65, float gate = 0.9f)
{
    ArpSettings a;
    a.shape = shape;
    a.rate = rate;
    a.rhythm = rhythm;
    a.centreNote = centre;
    a.gate = gate;
    return a;
}

std::vector<std::vector<int>> voiced (const Progression& p) { return voiceProgression (p, VoicingSettings {}); }

std::vector<NoteEvent> render (const Progression& p, const ArpSettings& a, const RenderSettings& f = feelOf(),
                               const std::vector<NoteEvent>& bass = {}, int channel = 4)
{
    return renderArp (p, voiced (p), a, f, bass, channel);
}

int stepsPerBeatOf (RateDivision r) { return (int) std::lround (1.0 / rateInBeats (r)); }

// Independent copy of the tone stack rules: relative to the root, first min (4, n) intervals, rotated for an inversion.
std::vector<int> modelStack (const ProgressionSlot& slot, const std::vector<int>& voicing)
{
    const auto& iv = qualityIntervals (slot.chord.quality);
    std::vector<int> core (iv.begin(), iv.begin() + (long) std::min<size_t> (4, iv.size()));
    if (slot.inversion < 0)
        return core;

    const int bassPc = slotBassPitchClass (slot, voicing);
    for (size_t r = 0; r < core.size(); ++r)
        if (pc (slot.chord.root + core[r]) == bassPc)
        {
            std::vector<int> out { core[r] % 12 };
            for (size_t j = 1; j < core.size(); ++j)
            {
                int v = core[(r + j) % core.size()];
                while (v <= out.back())
                    v += 12;
                out.push_back (v);
            }
            return out;
        }
    return core;
}

int modelTone (const std::vector<int>& stack, int tone)
{
    return tone < (int) stack.size() ? stack[(size_t) tone] : stack[(size_t) tone - stack.size()] + 12;
}

// The cycle's tones relative to tone 0.
std::vector<int> modelCycle (ArpShape shape, const std::vector<int>& stack)
{
    std::vector<int> rel;
    for (const auto& t : arpShapeTones (shape))
        rel.push_back (modelTone (stack, t.tone) - stack.front() + 12 * t.octave);
    return rel;
}

std::vector<int> pitches (const std::vector<NoteEvent>& ev)
{
    std::vector<int> n;
    for (const auto& e : ev)
        n.push_back (e.note);
    return n;
}

std::vector<int> relativeToFirst (const std::vector<NoteEvent>& ev)
{
    std::vector<int> n;
    for (const auto& e : ev)
        n.push_back (e.note - ev.front().note);
    return n;
}

// Steps (in arp steps from beat 0) that sound, per the mask; counts per slot.
int expectedNotes (const Progression& p, const ArpSettings& a)
{
    const int spb = stepsPerBeatOf (a.rate);
    int count = 0;
    for (int k = 0; k < p.totalBeats() * spb; ++k)
        if (arpStepSounds (a.rhythm, k % (4 * spb), 4 * spb))
            ++count;
    return count;
}

// Invariants of every arp render. `exactGrid`: swing 0 and humanize 0, so every start is on the arp grid.
void checkInvariants (const Progression& prog, const std::vector<NoteEvent>& ev, const ArpSettings& a, bool exactGrid,
                      float velocity, int channel = 4)
{
    CHECK_EQ ((int) ev.size(), expectedNotes (prog, a));

    const int spb = stepsPerBeatOf (a.rate);
    std::map<int, double> lastEnd;
    for (size_t i = 0; i < ev.size(); ++i)
    {
        const auto& e = ev[i];
        const int slot = prog.slotAtBeat (e.startBeat);
        REQUIRE (slot >= 0);

        CHECK (e.note >= kArpLowestNote && e.note <= kArpHighestNote);
        CHECK (e.startBeat >= prog.slotStartBeat (slot) - kTol);
        CHECK (e.startBeat + e.lengthBeats <= prog.slotStartBeat (slot + 1) + kTol);
        CHECK (e.lengthBeats >= 1.0e-4);
        CHECK_EQ (e.velocity, velocity);
        CHECK_EQ (e.channel, channel);
        if (exactGrid)
            CHECK (near (e.startBeat * spb, std::round (e.startBeat * spb), 1.0e-7));

        if (i > 0)
            CHECK (ev[i - 1].startBeat < e.startBeat || (ev[i - 1].startBeat == e.startBeat && ev[i - 1].note < e.note));

        const auto it = lastEnd.find (e.note);
        if (it != lastEnd.end())
            CHECK (e.startBeat >= it->second - kTol);   // same pitch: the earlier note ended before this start
        lastEnd[e.note] = e.startBeat + e.lengthBeats;

        // Every note is a tone of the chord's first four intervals (any octave).
        const auto& slotInfo = prog.slots[(size_t) slot];
        const auto& iv = qualityIntervals (slotInfo.chord.quality);
        bool inChord = false;
        for (size_t t = 0; t < std::min<size_t> (4, iv.size()); ++t)
            inChord = inChord || pc (e.note) == pc (slotInfo.chord.root + iv[t]);
        CHECK (inChord);
    }
}

constexpr RateDivision kAllRates[] = { RateDivision::Quarter, RateDivision::Eighth, RateDivision::Sixteenth,
                                       RateDivision::EighthTriplet };
} // namespace

TEST_CASE ("arp: sweep - 17 shapes x 4 rates x 7 rhythms x octaves -2..2 over three progressions")
{
    const std::vector<Progression> progressions { cAmFG(), oddSlots(), colourChords() };

    int renders = 0;
    for (const auto& prog : progressions)
    {
        const auto v = voiced (prog);
        for (int shape = 0; shape < numArpShapes; ++shape)
            for (auto rate : kAllRates)
                for (int rhythm = 0; rhythm < numArpRhythms; ++rhythm)
                    for (int octave = -2; octave <= 2; ++octave)
                    {
                        const auto a = arpOf ((ArpShape) shape, rate, (ArpRhythm) rhythm, 65 + 12 * octave);
                        const auto f = feelOf();
                        const auto ev = renderArp (prog, v, a, f, {});
                        ++renders;
                        checkInvariants (prog, ev, a, true, f.velocity);

                        if (octave == 0 && (shape + rhythm) % 5 == 0)
                            CHECK (ev == renderArp (prog, v, a, f, {}));   // deterministic
                    }
    }
    CHECK_EQ (renders, 3 * 17 * 4 * 7 * 5);
}

TEST_CASE ("arp: sweep - every chord quality and inversion, every shape and rate")
{
    int renders = 0;
    for (int q = 0; q < numChordQualities; ++q)
    {
        const int tones = (int) qualityIntervals ((ChordQuality) q).size();
        for (int inversion = -1; inversion < std::min (tones, 6); ++inversion)
        {
            auto prog = makeProgression ({ chordOf (7, (ChordQuality) q), chordOf (2, (ChordQuality) q) }, { 4, 3 });
            for (auto& slot : prog.slots)
                slot.inversion = inversion;
            const auto v = voiced (prog);

            for (int shape = 0; shape < numArpShapes; ++shape)
                for (auto rate : kAllRates)
                {
                    const auto a = arpOf ((ArpShape) shape, rate, (ArpRhythm) ((shape + q) % numArpRhythms));
                    const auto f = feelOf();
                    checkInvariants (prog, renderArp (prog, v, a, f, {}), a, true, f.velocity);
                    ++renders;
                }
        }
    }
    CHECK (renders > 8000);
}

TEST_CASE ("arp: sweep - swing, humanize and gate never drop a sounding step, never overlap a pitch, never leave the slot")
{
    const std::vector<Progression> progressions { cAmFG(), oddSlots() };

    for (const auto& prog : progressions)
    {
        const auto v = voiced (prog);
        for (int shape = 0; shape < numArpShapes; ++shape)
            for (auto arpRate : kAllRates)
                for (auto chordRate : kAllRates)
                    for (float gate : { 0.05f, 0.35f, 1.0f })
                    {
                        const auto a = arpOf ((ArpShape) shape, arpRate, (ArpRhythm) (shape % numArpRhythms), 65, gate);
                        const auto f = feelOf (chordRate, 1.0f, 1.0f);
                        checkInvariants (prog, renderArp (prog, v, a, f, {}), a, false, f.velocity);
                    }
    }
}

TEST_CASE ("arp: Up 3 over C is C E G C E G ... and runs across bar lines")
{
    const auto prog = makeProgression ({ chordOf (0) }, { 8 });
    const int base = 60;   // C E G around the centre 65
    const int tones[] = { 0, 4, 7 };

    auto ev = render (prog, arpOf (ArpShape::Up3));
    REQUIRE (ev.size() == 32);
    for (size_t i = 0; i < ev.size(); ++i)
        CHECK_EQ (ev[i].note, base + tones[i % 3]);
    CHECK_EQ (ev[16].note, base + 4);   // bar 2 starts on E, not C

    ev = render (prog, arpOf (ArpShape::Up3, RateDivision::Eighth));
    REQUIRE (ev.size() == 16);
    CHECK_EQ (ev[8].note, base + 7);    // at 1/8 bar 2 starts on G (8 % 3 == 2)

    ev = render (prog, arpOf (ArpShape::Down3));
    REQUIRE (ev.size() == 32);
    const int down[] = { 0, 7, 4 };
    for (size_t i = 0; i < ev.size(); ++i)
        CHECK_EQ (ev[i].note, base + down[i % 3]);
}

TEST_CASE ("arp: free-running shapes restart at the loop start only, the others at every bar line")
{
    for (int slotBeats : { 4, 5, 8, 9 })
        for (auto rate : kAllRates)
            for (int shape = 0; shape < numArpShapes; ++shape)
            {
                const auto prog = makeProgression ({ chordOf (0) }, { slotBeats });
                const auto a = arpOf ((ArpShape) shape, rate);
                const auto ev = render (prog, a);
                const int spb = stepsPerBeatOf (rate);
                const int stepsPerBar = 4 * spb;
                REQUIRE ((int) ev.size() == slotBeats * spb);

                const auto stack = modelStack (prog.slots[0], {});
                const auto rel = modelCycle ((ArpShape) shape, stack);
                const auto& cycle = arpShapeTones ((ArpShape) shape);
                const int base = ev[0].note - rel[0];
                const bool free = arpShapeFreeRuns ((ArpShape) shape);
                CHECK (free == (shape == (int) ArpShape::Up3 || shape == (int) ArpShape::Down3));

                for (int k = 0; k < (int) ev.size(); ++k)
                {
                    const int position = free ? k : k % stepsPerBar;
                    CHECK_EQ (ev[(size_t) k].note, base + rel[(size_t) (position % (int) cycle.size())]);
                }
            }
}

TEST_CASE ("arp: a chord change mid-cycle re-maps the cycle position onto the new chord")
{
    // Two octaves (6 tones, restarts per bar) over C (2 beats = 8 steps) then Am: step 8 is cycle position 2 of Am.
    const auto prog = makeProgression ({ chordOf (0), chordOf (9, ChordQuality::Minor) }, { 2, 4 });
    const auto ev = render (prog, arpOf (ArpShape::TwoOctaves));
    REQUIRE (ev.size() == 24);
    CHECK_EQ (pc (ev[7].note), pc (0 + 4));    // position 1 of C
    CHECK_EQ (pc (ev[8].note), pc (9 + 7));    // position 2 of Am: its fifth
    CHECK_EQ (pc (ev[9].note), pc (9));        // position 3 = tone 0 an octave up
    CHECK_EQ (ev[9].note - ev[8].note, 5);     // E up to A
    // Bar line at step 16: the cycle restarts on Am's root.
    CHECK_EQ (pc (ev[16].note), pc (9));
    CHECK_EQ (pc (ev[17].note), pc (9 + 3));

    // Free-running Up 3 keeps counting across the chord change: step 8 is position 8 % 3 = 2 of Am.
    const auto up = render (prog, arpOf (ArpShape::Up3));
    CHECK_EQ (pc (up[8].note), pc (9 + 7));
    CHECK_EQ (pc (up[9].note), pc (9));
}

TEST_CASE ("arp: rhythm masks silence the right steps and never skip a tone")
{
    const auto prog = makeProgression ({ chordOf (0) }, { 8 });   // 2 bars of 16 steps

    struct Case { ArpRhythm rhythm; std::vector<int> silentInBar; };
    const Case cases[] = {
        { ArpRhythm::EveryStep, {} },
        { ArpRhythm::ThreeOnOneOff, { 3, 7, 11, 15 } },
        { ArpRhythm::KickGap, { 0, 4, 8, 12 } },
        { ArpRhythm::Skip2nd, { 1, 5, 9, 13 } },
        { ArpRhythm::Skip3rd, { 2, 6, 10, 14 } },
        { ArpRhythm::Breath, { 15 } },
        { ArpRhythm::LateStart, { 0 } },
    };

    for (const auto& c : cases)
    {
        for (auto shape : { ArpShape::Up3, ArpShape::Up4, ArpShape::TwoOctaves })
        {
            const auto ev = render (prog, arpOf (shape, RateDivision::Sixteenth, c.rhythm));

            std::set<int> expectedSteps;
            for (int k = 0; k < 32; ++k)
                if (std::find (c.silentInBar.begin(), c.silentInBar.end(), k % 16) == c.silentInBar.end())
                    expectedSteps.insert (k);

            std::set<int> steps;
            for (const auto& e : ev)
                steps.insert ((int) std::lround (e.startBeat * 4.0));
            CHECK (steps == expectedSteps);

            // The cycle advances on sounding steps only: the i-th sounding step of a bar plays position i.
            const auto stack = modelStack (prog.slots[0], {});
            const auto rel = modelCycle (shape, stack);
            const int base = ev[0].note - rel[0];   // the first sounding step of the loop is always position 0
            for (size_t i = 0; i < ev.size(); ++i)
            {
                const int k = (int) std::lround (ev[i].startBeat * 4.0);
                const int from = arpShapeFreeRuns (shape) ? 0 : (k / 16) * 16;
                int soundingBefore = 0;   // sounding steps before this one in the same bar (restart) or the loop (free)
                for (int j = from; j < k; ++j)
                    if (expectedSteps.count (j) != 0)
                        ++soundingBefore;
                CHECK_EQ (ev[i].note, base + rel[(size_t) (soundingBefore % (int) rel.size())]);
            }
        }
    }

    // 3 on 1 off with Up 3: the tones run C E G C E G ... through the gaps (12 notes per bar = 4 full cycles).
    const auto ev = render (prog, arpOf (ArpShape::Up3, RateDivision::Sixteenth, ArpRhythm::ThreeOnOneOff));
    REQUIRE (ev.size() == 24);
    const int tones[] = { 0, 4, 7 };
    for (size_t i = 0; i < ev.size(); ++i)
        CHECK_EQ (ev[i].note, 60 + tones[i % 3]);

    // Late start with Up 4: the first sounding step of every bar plays tone 0.
    const auto late = render (prog, arpOf (ArpShape::Up4, RateDivision::Sixteenth, ArpRhythm::LateStart));
    REQUIRE (late.size() == 30);
    CHECK (near (late[0].startBeat, 0.25));
    CHECK_EQ (late[0].note, 60);
    CHECK (near (late[15].startBeat, 4.25));
    CHECK_EQ (late[15].note, 60);
}

TEST_CASE ("arp: the tone stack - slash chords, inversions, 5/6-tone chords, triad tone 3")
{
    struct Case
    {
        ChordQuality quality;
        int inversion;
        std::vector<int> expectedRel;   // Climb7 over one beat per tone (rate 1/4), relative to tone 0
    };
    const Case cases[] = {
        { ChordQuality::Major, -1, { 0, 4, 7, 12 } },                  // triad: tone 3 = tone 0 + 12
        { ChordQuality::Major, 1, { 0, 3, 8, 12 } },                   // C/E: E G C' E'
        { ChordQuality::Major, 2, { 0, 5, 9, 12 } },                   // C/G: G C' E' G'
        { ChordQuality::Major7, 3, { 0, 1, 5, 8 } },                   // Cmaj7 over B: B C' E' G'
        { ChordQuality::Dominant7, 2, { 0, 3, 5, 9 } },                // C7 over G: G Bb C' E'
        { ChordQuality::Dominant13, -1, { 0, 4, 7, 10 } },             // first 4 intervals only
        { ChordQuality::Minor11, -1, { 0, 3, 7, 10 } },
        { ChordQuality::Major9, -1, { 0, 4, 7, 11 } },
        { ChordQuality::Major9, 4, { 0, 4, 7, 11 } },                  // the 9th is outside the core: no rotation
        { ChordQuality::Add9, -1, { 0, 4, 7, 14 } },
        { ChordQuality::Add9, 3, { 0, 10, 14, 17 } },                  // D bass: intervals 2 12 16 19
        { ChordQuality::MinorAdd9, 3, { 0, 10, 13, 17 } },             // D bass: 2 12 15 19
    };

    for (const auto& c : cases)
    {
        auto prog = makeProgression ({ chordOf (0, c.quality) }, { 4 });
        prog.slots[0].inversion = c.inversion;

        const auto ev = render (prog, arpOf (ArpShape::Climb7, RateDivision::Quarter));
        REQUIRE (ev.size() == 4);
        CHECK (relativeToFirst (ev) == c.expectedRel);

        // A missing voicing counts as empty (the inversion's chord tone is the bass): the same notes.
        CHECK (renderArp (prog, {}, arpOf (ArpShape::Climb7, RateDivision::Quarter), feelOf(), {}) == ev);
    }

    // Up 3 over C/E: E G C'.
    auto slash = makeProgression ({ chordOf (0) }, { 4 });
    slash.slots[0].inversion = 1;
    const auto ev = render (slash, arpOf (ArpShape::Up3, RateDivision::Quarter));
    REQUIRE (ev.size() == 4);
    CHECK_EQ (pc (ev[0].note), 4);
    CHECK_EQ (ev[1].note - ev[0].note, 3);
    CHECK_EQ (ev[2].note - ev[1].note, 5);

    // Fewer voicings than slots: the second slot has none and still rotates by its inversion's chord tone.
    auto two = makeProgression ({ chordOf (0), chordOf (0, ChordQuality::Add9) }, { 4, 4 });
    two.slots[1].inversion = 3;
    const auto partial = renderArp (two, { { 48, 52, 55 } }, arpOf (ArpShape::Climb7, RateDivision::Quarter), feelOf(), {});
    REQUIRE (partial.size() == 8);
    const std::vector<NoteEvent> second (partial.begin() + 4, partial.end());
    CHECK (relativeToFirst (second) == std::vector<int> ({ 0, 10, 14, 17 }));
}

TEST_CASE ("arp: centreNote is clamped to 40..96 by renderArp")
{
    const auto prog = cAmFG();
    for (auto shape : { ArpShape::Up3, ArpShape::Up4, ArpShape::TwoOctaves, ArpShape::RootPulse })
    {
        CHECK (render (prog, arpOf (shape, RateDivision::Sixteenth, ArpRhythm::EveryStep, -50))
               == render (prog, arpOf (shape, RateDivision::Sixteenth, ArpRhythm::EveryStep, 40)));
        CHECK (render (prog, arpOf (shape, RateDivision::Sixteenth, ArpRhythm::EveryStep, 39))
               == render (prog, arpOf (shape, RateDivision::Sixteenth, ArpRhythm::EveryStep, 40)));
        CHECK (render (prog, arpOf (shape, RateDivision::Sixteenth, ArpRhythm::EveryStep, 400))
               == render (prog, arpOf (shape, RateDivision::Sixteenth, ArpRhythm::EveryStep, 96)));
        CHECK (render (prog, arpOf (shape, RateDivision::Sixteenth, ArpRhythm::EveryStep, 97))
               == render (prog, arpOf (shape, RateDivision::Sixteenth, ArpRhythm::EveryStep, 96)));
    }

    // Everything is folded into 28..108 even at the extremes.
    for (int centre : { 0, 40, 96, 127 })
        for (int shape = 0; shape < numArpShapes; ++shape)
            for (const auto& n : render (cAmFG(), arpOf ((ArpShape) shape, RateDivision::Sixteenth, ArpRhythm::EveryStep, centre)))
                CHECK (n.note >= 28 && n.note <= 108);
}

namespace
{
// Independent model of the register rule: the base per slot (no bass clearance), by brute force over every pitch.
std::vector<int> modelBases (const Progression& prog, ArpShape shape, int centre, std::vector<bool>* voiceLed = nullptr)
{
    std::vector<int> bases;
    bool havePrevious = false;
    int previous = 0;
    for (const auto& slot : prog.slots)
    {
        const auto stack = modelStack (slot, {});
        const auto rel = modelCycle (shape, stack);
        long sum = 0;
        for (int r : rel)
            sum += r;

        std::vector<int> candidates;
        for (int b = 0; b <= 127; ++b)
            if (pc (b) == pc (slot.chord.root + stack.front()))
                candidates.push_back (b);

        auto offset = [&] (int b) { return std::labs ((long) rel.size() * b + sum - (long) centre * (long) rel.size()); };

        std::vector<int> ledCandidates;
        for (int b : candidates)
            if (havePrevious && std::abs (b - previous) <= 6 && offset (b) <= 7 * (long) rel.size())
                ledCandidates.push_back (b);

        int chosen = -1;
        if (voiceLed != nullptr)
            voiceLed->push_back (! ledCandidates.empty());
        if (! ledCandidates.empty())
        {
            chosen = *std::min_element (ledCandidates.begin(), ledCandidates.end(), [&] (int x, int y)
            {
                return std::abs (x - previous) != std::abs (y - previous) ? std::abs (x - previous) < std::abs (y - previous) : x < y;
            });
        }
        else
        {
            chosen = *std::min_element (candidates.begin(), candidates.end(), [&] (int x, int y)
            {
                return offset (x) != offset (y) ? offset (x) < offset (y) : x < y;
            });
        }

        bases.push_back (chosen);
        havePrevious = true;
        previous = chosen;
    }
    return bases;
}
} // namespace

TEST_CASE ("arp: register - the base follows the reference model, the mean stays within 7 of the centre")
{
    const auto prog = cAmFG();
    int voiceLedSlots = 0;

    for (int shape = 0; shape < numArpShapes; ++shape)
        for (int centre : { 53, 65, 77 })
        {
            const auto a = arpOf ((ArpShape) shape, RateDivision::Quarter, ArpRhythm::EveryStep, centre);
            const auto ev = render (prog, a);
            const auto bases = modelBases (prog, (ArpShape) shape, centre);
            REQUIRE (ev.size() == 16);

            for (int s = 0; s < 4; ++s)
            {
                const auto stack = modelStack (prog.slots[(size_t) s], {});
                const auto rel = modelCycle ((ArpShape) shape, stack);
                const bool free = arpShapeFreeRuns ((ArpShape) shape);
                // Slots are 4 beats = 4 quarter steps = one bar: restart shapes begin the cycle, free ones continue it.
                for (int j = 0; j < 4; ++j)
                {
                    const int position = (free ? 4 * s + j : j) % (int) rel.size();
                    CHECK_EQ (ev[(size_t) (4 * s + j)].note, bases[(size_t) s] + rel[(size_t) position]);
                }

                double sum = 0.0;
                for (int r : rel)
                    sum += r;
                CHECK (std::abs (bases[(size_t) s] + sum / (double) rel.size() - centre) <= 7.0 + 1.0e-9);

                if (s > 0 && std::abs (bases[(size_t) s] - bases[(size_t) s - 1]) <= 6)
                    ++voiceLedSlots;
            }
        }
    CHECK (voiceLedSlots > 0);

    // The documented Up 3 case at the default centre: C4, then A3 (voice-led, moves 3), then F4 (the voice-led option
    // would put the mean 8.3 from the centre, so the nearest register is used: a move of 8), then G4.
    CHECK (modelBases (prog, ArpShape::Up3, 65) == std::vector<int> ({ 60, 57, 65, 67 }));
}

TEST_CASE ("arp: register - arpOctave moves the notes by exactly 12")
{
    for (const auto& prog : { cAmFG(), oddSlots() })
        for (int shape = 0; shape < numArpShapes; ++shape)
            for (auto rate : { RateDivision::Eighth, RateDivision::Sixteenth })
            {
                const auto base = render (prog, arpOf ((ArpShape) shape, rate, ArpRhythm::EveryStep, 65));
                const auto up = render (prog, arpOf ((ArpShape) shape, rate, ArpRhythm::EveryStep, 77));
                const auto down = render (prog, arpOf ((ArpShape) shape, rate, ArpRhythm::EveryStep, 53));
                REQUIRE (base.size() == up.size());
                REQUIRE (base.size() == down.size());
                for (size_t i = 0; i < base.size(); ++i)
                {
                    CHECK_EQ (up[i].note, base[i].note + 12);
                    CHECK_EQ (down[i].note, base[i].note - 12);
                    CHECK (near (up[i].startBeat, base[i].startBeat));
                }
            }
}

TEST_CASE ("arp: voice-led bases move at most 6 whenever the voice-led option exists, ties pick the lower base")
{
    // Up 3 at centre 64: C4 base 60 (mean 63.7); F# major: base 54 (mean 57.7, 6.3 from the centre) and 66 (mean 69.7,
    // 5.7 from the centre) are both 6 from the previous base and both within 7: the lower wins, although 66 is
    // the nearer register.
    const auto prog = makeProgression ({ chordOf (0), chordOf (6) }, { 4, 4 });
    const auto ev = render (prog, arpOf (ArpShape::Up3, RateDivision::Quarter, ArpRhythm::EveryStep, 64));
    REQUIRE (ev.size() == 8);
    CHECK_EQ (ev[0].note, 60);
    CHECK_EQ (ev[4].note, 54 + 4);   // free-running position 4 % 3 = 1: the major third of the lower base (58, not 70)

    // On C-Am-F-G a voice-led slot moves at most 6; other slots take the nearest register (which may move more: the
    // option is only offered while the cycle mean stays within 7 of the centre). The rendered notes follow the model.
    int ledSlots = 0;
    int jumps = 0;
    for (int shape = 0; shape < numArpShapes; ++shape)
        for (int centre : { 53, 65, 77 })
        {
            std::vector<bool> led;
            const auto bases = modelBases (cAmFG(), (ArpShape) shape, centre, &led);
            REQUIRE (bases.size() == 4 && led.size() == 4);
            for (size_t s = 1; s < bases.size(); ++s)
            {
                if (led[s])
                {
                    CHECK (std::abs (bases[s] - bases[s - 1]) <= 6);
                    ++ledSlots;
                }
                else if (std::abs (bases[s] - bases[s - 1]) > 6)
                    ++jumps;
            }
        }
    CHECK (ledSlots > 0);
    CHECK (jumps >= 0);
}

TEST_CASE ("arp: bass clearance - an octave above the lowest bass note of the slot while the mean allows it")
{
    const auto prog = makeProgression ({ chordOf (0), chordOf (0) }, { 4, 4 });
    auto bassNote = [] (int note, double start) { NoteEvent e; e.note = note; e.startBeat = start; e.lengthBeats = 1.0; e.channel = 2; return e; };
    const auto a = arpOf (ArpShape::RootPulse, RateDivision::Quarter);   // the cycle mean is the base itself

    auto firstOf = [&] (const std::vector<NoteEvent>& bass, int slot)
    {
        const auto ev = render (prog, a, feelOf(), bass);
        REQUIRE (ev.size() == 8);
        return ev[(size_t) slot * 4].note;
    };

    CHECK_EQ (firstOf ({}, 0), 60);
    CHECK_EQ (firstOf ({ bassNote (36, 0.0) }, 0), 60);                   // already 24 above
    CHECK_EQ (firstOf ({ bassNote (48, 0.0) }, 0), 60);                   // exactly +12
    CHECK_EQ (firstOf ({ bassNote (55, 0.0) }, 0), 72);                   // base 60 < 55 + 12: up an octave
    CHECK_EQ (firstOf ({ bassNote (55, 0.0), bassNote (70, 2.0) }, 0), 72);   // the lowest note of the slot decides
    CHECK_EQ (firstOf ({ bassNote (55, 0.0), bassNote (40, 2.0) }, 0), 60);   // ... and here it is the 40
    CHECK_EQ (firstOf ({ bassNote (70, 0.0) }, 0), 72);                   // 84 would put the mean past centre + 12 = 77
    CHECK_EQ (firstOf ({ bassNote (55, 4.0) }, 0), 60);                   // a bass note of the next slot does not count
    CHECK_EQ (firstOf ({ bassNote (55, 4.0) }, 1), 72);

    // The next slot leads from the base after clearance: Bb after a cleared C (72) is 70, not the 58 it would be
    // from the uncleared C (60).
    const auto bb = makeProgression ({ chordOf (0), chordOf (10) }, { 4, 4 });
    const auto ev = render (bb, a, feelOf(), { bassNote (55, 0.0) });
    REQUIRE (ev.size() == 8);
    CHECK_EQ (ev[0].note, 72);
    CHECK_EQ (ev[4].note, 70);
    CHECK_EQ (render (bb, a)[4].note, 58);

    // A cycle with several tones clears its lowest note (Down 4 starts on tone 0', its lowest is tone 0). With the
    // bass at 50 the lowest must reach 62; at centre 65 the next octave would put the mean (77.75) past centre + 12
    // = 77, so the cycle stays; at centre 69 the octave up is allowed.
    const auto cOnly = makeProgression ({ chordOf (0) }, { 4 });
    const int centres[] = { 65, 69 };
    const int lowests[] = { 60, 72 };
    for (int i = 0; i < 2; ++i)
    {
        const auto climb = render (cOnly, arpOf (ArpShape::Down4, RateDivision::Quarter, ArpRhythm::EveryStep, centres[i]),
                                   feelOf(), { bassNote (50, 0.0) });
        REQUIRE (climb.size() == 4);
        const auto notes = pitches (climb);
        CHECK_EQ (*std::min_element (notes.begin(), notes.end()), lowests[i]);
    }
}

TEST_CASE ("arp: gate and length rules")
{
    // Gate 0.05 at 1/16 and at 1/8T: every sounding step has a note of 1/16 beat (nothing cuts it at swing 0).
    for (auto rate : { RateDivision::Sixteenth, RateDivision::EighthTriplet })
        for (auto prog : { cAmFG(), oddSlots() })
            for (auto shape : { ArpShape::Up3, ArpShape::RootPulse, ArpShape::TwoOctaves })
            {
                const auto a = arpOf (shape, rate, ArpRhythm::EveryStep, 65, 0.05f);
                const auto ev = render (prog, a);
                CHECK_EQ ((int) ev.size(), expectedNotes (prog, a));
                for (const auto& e : ev)
                    CHECK (nearLen (e.lengthBeats, kMinMelodyNoteBeats));
            }

    // Gate 0.5 at 1/16: half a step; gate 0.9: 0.225; gate 1 is legato (the next step's start).
    const auto prog = makeProgression ({ chordOf (0) }, { 4 });
    for (const auto& e : render (prog, arpOf (ArpShape::Up3, RateDivision::Sixteenth, ArpRhythm::EveryStep, 65, 0.5f)))
        CHECK (nearLen (e.lengthBeats, 0.125));
    for (const auto& e : render (prog, arpOf (ArpShape::Up3)))
        CHECK (nearLen (e.lengthBeats, 0.225));
    for (const auto& e : render (prog, arpOf (ArpShape::Up3, RateDivision::Sixteenth, ArpRhythm::EveryStep, 65, 1.0f)))
        CHECK (nearLen (e.lengthBeats, 0.25));

    // RootPulse at gate 1 repeats one pitch: each note ends at or before the next start, also at swing 1.
    for (float swing : { 0.0f, 1.0f })
        for (auto chordRate : { RateDivision::Eighth, RateDivision::Quarter, RateDivision::Sixteenth })
        {
            const auto ev = render (oddSlots(), arpOf (ArpShape::RootPulse, RateDivision::Sixteenth, ArpRhythm::EveryStep, 65, 1.0f),
                                    feelOf (chordRate, swing, 1.0f));
            for (size_t i = 0; i + 1 < ev.size(); ++i)
                if (ev[i].note == ev[i + 1].note)
                    CHECK (ev[i].startBeat + ev[i].lengthBeats <= ev[i + 1].startBeat + kTol);
        }

    // Swing 1 with chord rate 1/8 and 1/4 and a 1/16 arp at gate 0.35: one note per sounding step.
    for (auto chordRate : { RateDivision::Eighth, RateDivision::Quarter })
    {
        const auto a = arpOf (ArpShape::Up4, RateDivision::Sixteenth, ArpRhythm::ThreeOnOneOff, 65, 0.35f);
        const auto ev = render (oddSlots(), a, feelOf (chordRate, 1.0f));
        CHECK_EQ ((int) ev.size(), expectedNotes (oddSlots(), a));
    }

    // Velocity is flat at humanize 1: only timing jitters (and never past the slot).
    const auto a = arpOf (ArpShape::Up4);
    const auto plain = render (oddSlots(), a, feelOf (RateDivision::Eighth, 0.0f, 0.0f));
    const auto human = render (oddSlots(), a, feelOf (RateDivision::Eighth, 0.0f, 1.0f));
    REQUIRE (plain.size() == human.size());
    bool moved = false;
    for (size_t i = 0; i < plain.size(); ++i)
    {
        CHECK_EQ (human[i].velocity, 0.75f);
        CHECK_EQ (human[i].note, plain[i].note);
        CHECK (std::abs (human[i].startBeat - plain[i].startBeat) <= 0.015 + kTol);
        moved = moved || human[i].startBeat != plain[i].startBeat;
    }
    CHECK (moved);
    CHECK (human == render (oddSlots(), a, feelOf (RateDivision::Eighth, 0.0f, 1.0f)));

    // Swing and humanize do not touch an arp at 1/8T for the time grid: swing moves nothing.
    const auto triplet = arpOf (ArpShape::Up3, RateDivision::EighthTriplet);
    CHECK (render (cAmFG(), triplet, feelOf (RateDivision::Eighth, 0.0f)) == render (cAmFG(), triplet, feelOf (RateDivision::Eighth, 1.0f)));
}

TEST_CASE ("arp: empty progression, channel and sorting")
{
    CHECK (renderArp (Progression {}, {}, ArpSettings {}, RenderSettings {}, {}).empty());

    const auto ev = render (cAmFG(), arpOf (ArpShape::TwoOctaves), feelOf(), {}, 7);
    REQUIRE (! ev.empty());
    for (const auto& e : ev)
        CHECK_EQ (e.channel, 7);
    CHECK_EQ (renderArp (cAmFG(), voiced (cAmFG()), ArpSettings {}, RenderSettings {}, {}).front().channel, 4);

    // A zero-length slot is skipped without disturbing the grid.
    auto prog = makeProgression ({ chordOf (0), chordOf (9, ChordQuality::Minor) }, { 4, 4 });
    prog.slots.insert (prog.slots.begin() + 1, ProgressionSlot {});
    prog.slots[1].lengthBeats = 0;
    CHECK_EQ (render (prog, arpOf (ArpShape::Up3)).size(), (size_t) 32);
}

TEST_CASE ("arp: a 16 x 16-beat progression at 1/16 gives 1024 notes")
{
    Progression prog;
    for (int i = 0; i < kMaxSlots; ++i)
    {
        ProgressionSlot s;
        s.chord = chordOf ((i * 5) % 12, i % 2 == 0 ? ChordQuality::Major : ChordQuality::Minor7);
        s.lengthBeats = kMaxSlotBeats;
        prog.slots.push_back (s);
    }

    for (auto shape : { ArpShape::RootPulse, ArpShape::Up4, ArpShape::Up3, ArpShape::TwoOctaves })
    {
        const auto a = arpOf (shape);
        const auto ev = render (prog, a);
        CHECK_EQ (ev.size(), (size_t) 1024);
        checkInvariants (prog, ev, a, true, 0.75f);
    }
}
