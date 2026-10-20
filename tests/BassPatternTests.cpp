#include "TestFramework.h"

#include "core/Arp.h"
#include "core/Pattern.h"
#include "core/Voicing.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>

// Bass pattern modes and the shared groove (owner: WP2; final-spec 4.2, 5.1, 9 WP2). WP0 seeded this file with checks
// of the frozen pattern data; the rest tests the renderer against a reference model written from the spec.

using namespace cb;

TEST_CASE ("bass patterns: WP0 data matches the spec grids")
{
    CHECK_EQ (numBassModes, 15);
    CHECK_EQ (std::string (bassModeName (BassMode::Pulse)), std::string ("Pulse"));
    CHECK_EQ (std::string (bassModeName (BassMode::DottedQuarter)), std::string ("Dotted quarters"));

    CHECK_EQ (bassPatternGrid (BassMode::Offbeat8), std::string ("--x---x---x---x-"));
    CHECK_EQ (bassPatternGrid (BassMode::Rolling16), std::string ("xxxxxxxxxxxxxxxx"));
    CHECK_EQ (bassPatternGrid (BassMode::RollingKickGap), std::string ("-xox-xox-xox-xox"));
    CHECK_EQ (bassPatternGrid (BassMode::OffbeatPairs), std::string ("--xx--xx--xx--xx"));
    CHECK_EQ (bassPatternGrid (BassMode::Gallop), std::string ("x-xx-xx-x-xx-xx-"));
    CHECK_EQ (bassPatternGrid (BassMode::OctaveBounce8), std::string ("x-o-x-o-x-o-x-o-"));
    CHECK_EQ (bassPatternGrid (BassMode::Driving8), std::string ("x-x-x-x-x-x-x-x-"));
    CHECK_EQ (bassPatternGrid (BassMode::OffKick), std::string ("-x-x--x--x-x--x-"));
    CHECK_EQ (bassPatternGrid (BassMode::Tresillo), std::string ("x--x--x-x--x--x-"));
    CHECK_EQ (bassPatternGrid (BassMode::DottedQuarter), std::string ("x-----x-----x---"));

    for (int m = 0; m < numBassModes; ++m)
    {
        const auto mode = (BassMode) m;
        CHECK_EQ (isBassPattern (mode), m >= 5);
        CHECK_EQ (bassPatternSteps (mode).empty(), m < 5);
        if (bassPatternAvoidsKick (mode))
            for (const auto& s : bassPatternSteps (mode))
                CHECK (s.step % 4 != 0);
    }

    CHECK_EQ (bassModeBrowseOrder().size(), (size_t) 14);
    CHECK_EQ (std::string (bassModeFamily (BassMode::Off)), std::string (""));
    CHECK_EQ (std::string (bassModeFamily (BassMode::Gallop)), std::string ("Trance"));
    CHECK_EQ (std::string (bassModeFamily (BassMode::Tresillo)), std::string ("Groove"));
}

TEST_CASE ("groove: WP0 swingWarp and grooveGridBeats basics")
{
    RenderSettings rs;
    rs.rate = RateDivision::Sixteenth;
    CHECK_EQ (grooveGridBeats (rs), 0.25);
    rs.rate = RateDivision::EighthTriplet;
    CHECK_EQ (grooveGridBeats (rs), 0.0);

    CHECK_EQ (swingWarp (1.25, 0.25, 0.0), 1.25);
    CHECK_EQ (swingWarp (1.0, 0.25, 1.0), 1.0);
    CHECK (std::abs (swingWarp (0.5, 0.5, 0.5) - (0.5 + 0.5 * 0.5 / 3.0)) < 1.0e-12);
}

namespace
{
constexpr double kTol = 1.0e-9;
constexpr int kBassChannelNumber = 2;

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

// 14 beats in slots of 1, 2, 3, 5 and 3 beats: slot starts at beats 0, 1, 3, 6, 11 (bar lines inside slots).
Progression oddSlots()
{
    return makeProgression ({ chordOf (0, ChordQuality::Major9), chordOf (9, ChordQuality::Minor), chordOf (5, ChordQuality::Add9),
                              chordOf (7, ChordQuality::Dominant7), chordOf (2, ChordQuality::Sus2) },
                            { 1, 2, 3, 5, 3 });
}

RenderSettings bassSettings (BassMode mode, RateDivision rate = RateDivision::Eighth, float swing = 0.0f,
                             float humanize = 0.0f, float gate = 0.9f)
{
    RenderSettings s;
    s.pattern = RhythmPattern::Block;
    s.rate = rate;
    s.bass = mode;
    s.velocity = 0.75f;
    s.gate = gate;
    s.swing = swing;
    s.humanize = humanize;
    return s;
}

std::vector<std::vector<int>> voiced (const Progression& p) { return voiceProgression (p, VoicingSettings {}); }

std::vector<NoteEvent> onChannel (const std::vector<NoteEvent>& events, int channel)
{
    std::vector<NoteEvent> out;
    for (const auto& e : events)
        if (e.channel == channel)
            out.push_back (e);
    return out;
}

std::vector<NoteEvent> bassOf (const Progression& p, const std::vector<std::vector<int>>& v, const RenderSettings& s)
{
    return onChannel (renderProgression (p, v, s), kBassChannelNumber);
}

// Reference model of the bass octave rule (Pattern.h).
int referenceRoot (int bassPc, int lowest, int previous)
{
    auto tier = [lowest] (int n) { return n <= lowest - 5 ? 0 : (n < lowest ? 1 : (n == lowest ? 2 : 3)); };
    auto cost = [previous] (int n) { return 2 * std::abs (n - previous) + std::abs (n - 41); };
    int best = -1;
    for (int n = 28; n <= 55; ++n)
    {
        if (pc (n) != bassPc)
            continue;
        if (best < 0 || tier (n) < tier (best) || (tier (n) == tier (best) && tier (n) < 3 && cost (n) < cost (best)))
            best = n;
    }
    if (tier (best) >= 2)
        for (int n = 24; n < 28; ++n)
            if (pc (n) == bassPc && n < lowest)
                return n;
    return best;
}

int referencePartner (int root, int lowest)
{
    int other = root + 12;
    if (other > 55 || (other > lowest - 5 && root - 12 >= 28))
        other = root - 12;
    return other;
}

bool patternHasStep (BassMode mode, int step)
{
    for (const auto& s : bassPatternSteps (mode))
        if (s.step == step)
            return true;
    return false;
}

// True when a pattern onset lies on the straight grid within [slot start, slot start + 0.5 beat].
bool patternHasEarlyOnset (BassMode mode, int slotStartStep)
{
    for (int d = 0; d <= 2; ++d)
        if (patternHasStep (mode, (slotStartStep + d) % 16))
            return true;
    return false;
}

struct Expected
{
    double start = 0.0;
    double length = 0.0;
    int note = 0;
    bool strike = false;
};

// The spec's pattern renderer for humanize 0: onsets on the absolute grid, swingWarp, slot-start strike for on-beat
// patterns, lengths cut at the next onset and the slot end. Needs non-empty voicings.
std::vector<Expected> expectedBass (const Progression& prog, const std::vector<std::vector<int>>& voicings,
                                    const RenderSettings& s)
{
    std::vector<Expected> out;
    const double gate = std::clamp ((double) s.gate, 0.05, 1.0);
    const double swing = std::clamp ((double) s.swing, 0.0, 1.0);
    const double grid = grooveGridBeats (s);

    int previous = 40;
    double slotStart = 0.0;
    for (size_t i = 0; i < prog.slots.size(); ++i)
    {
        const auto& slot = prog.slots[i];
        const double slotEnd = slotStart + slot.lengthBeats;
        const int lowest = voicings[i].front();
        const int root = referenceRoot (slotBassPitchClass (slot, voicings[i]), lowest, previous);
        const int partner = referencePartner (root, lowest);

        std::vector<Expected> notes;
        const int g0 = (int) std::lround (slotStart * 4.0);
        const int g1 = (int) std::lround (slotEnd * 4.0);
        for (int g = g0; g < g1; ++g)
            for (const auto& st : bassPatternSteps (s.bass))
                if (st.step == g % 16)
                {
                    Expected e;
                    e.start = swingWarp ((double) g * 0.25, grid, swing);
                    e.length = (double) st.lengthSteps * 0.25 * gate / 0.9;
                    e.note = st.role == BassRole::Octave ? partner : root;
                    notes.push_back (e);
                }

        if (! bassPatternAvoidsKick (s.bass) && ! patternHasEarlyOnset (s.bass, g0))
        {
            Expected strike;
            strike.start = slotStart;
            strike.note = root;
            strike.strike = true;
            strike.length = ((notes.empty() ? slotEnd : notes.front().start) - slotStart) * std::min (1.0, gate / 0.9);
            notes.insert (notes.begin(), strike);
        }

        for (size_t j = 0; j < notes.size(); ++j)
        {
            const double next = j + 1 < notes.size() ? notes[j + 1].start : slotEnd;
            notes[j].length = std::min ({ notes[j].length, next - notes[j].start, slotEnd - notes[j].start });
        }

        previous = notes.back().note;
        out.insert (out.end(), notes.begin(), notes.end());
        slotStart = slotEnd;
    }
    return out;
}

constexpr BassMode kPatternModes[] = { BassMode::Offbeat8, BassMode::Rolling16, BassMode::RollingKickGap,
                                       BassMode::OffbeatPairs, BassMode::Gallop, BassMode::OctaveBounce8,
                                       BassMode::Driving8, BassMode::OffKick, BassMode::Tresillo,
                                       BassMode::DottedQuarter };

constexpr RateDivision kAllRates[] = { RateDivision::Quarter, RateDivision::Eighth, RateDivision::Sixteenth,
                                       RateDivision::EighthTriplet };
} // namespace

TEST_CASE ("bass patterns: each pattern on C-Am-F-G plays exactly its grid")
{
    const auto prog = cAmFG();
    const auto voicings = voiced (prog);

    for (auto mode : kPatternModes)
        for (float gate : { 0.9f, 0.45f, 1.0f })
        {
            const auto s = bassSettings (mode, RateDivision::Eighth, 0.0f, 0.0f, gate);
            const auto ev = bassOf (prog, voicings, s);
            const auto expected = expectedBass (prog, voicings, s);
            REQUIRE (ev.size() == expected.size());

            for (size_t i = 0; i < ev.size(); ++i)
            {
                CHECK (near (ev[i].startBeat, expected[i].start));
                CHECK (nearLen (ev[i].lengthBeats, expected[i].length));
                CHECK_EQ (ev[i].note, expected[i].note);
                CHECK_EQ (ev[i].channel, 2);
                CHECK (! expected[i].strike);   // 4-beat slots start on the bar line: patterns never need a strike here
            }

            // The onset grid and the 'o' steps, bar by bar (every slot is one bar).
            int previous = 40;
            for (int bar = 0; bar < 4; ++bar)
            {
                const auto& slot = prog.slots[(size_t) bar];
                const int lowest = voicings[(size_t) bar].front();
                const int root = referenceRoot (pc (slot.chord.root), lowest, previous);
                const int partner = referencePartner (root, lowest);

                std::string grid (16, '-');
                for (const auto& e : ev)
                    if (e.startBeat >= bar * 4.0 - kTol && e.startBeat < (bar + 1) * 4.0 - kTol)
                    {
                        const int step = (int) std::lround (e.startBeat * 4.0) - 16 * bar;
                        REQUIRE (step >= 0 && step < 16);
                        CHECK (near (e.startBeat * 4.0, std::round (e.startBeat * 4.0)));
                        CHECK (e.note == root || e.note == partner);
                        grid[(size_t) step] = e.note == root ? 'x' : 'o';
                        previous = e.note;
                    }
                CHECK_EQ (grid, bassPatternGrid (mode));
            }
        }
}

TEST_CASE ("bass patterns: lengths follow lengthSteps, the gate and the cuts")
{
    const auto prog = cAmFG();
    const auto voicings = voiced (prog);

    // Offbeat8 at gate 0.9: 2 sixteenths = 0.5 beat.
    auto ev = bassOf (prog, voicings, bassSettings (BassMode::Offbeat8));
    REQUIRE (ev.size() == 16);
    for (const auto& e : ev)
        CHECK (nearLen (e.lengthBeats, 0.5));
    CHECK (near (ev[0].startBeat, 0.5));

    // Rolling16: 0.96 sixteenth = 0.24 beat.
    ev = bassOf (prog, voicings, bassSettings (BassMode::Rolling16));
    REQUIRE (ev.size() == 64);
    for (const auto& e : ev)
        CHECK (nearLen (e.lengthBeats, 0.24));

    // Gallop: step 0 = 1 sixteenth, step 2 = 0.8, step 6 = 1.6 (no cut: 6 + 1.6 < 8), last step 14 + 1.6 < 16.
    ev = bassOf (prog, voicings, bassSettings (BassMode::Gallop));
    REQUIRE (ev.size() == 40);
    CHECK (nearLen (ev[0].lengthBeats, 0.25));
    CHECK (nearLen (ev[1].lengthBeats, 0.2));
    CHECK (nearLen (ev[4].lengthBeats, 0.4));
    CHECK (nearLen (ev[9].lengthBeats, 0.4));

    // Gate scaling is gate / 0.9 on the pattern length: gate 0.45 halves it.
    ev = bassOf (prog, voicings, bassSettings (BassMode::Offbeat8, RateDivision::Eighth, 0.0f, 0.0f, 0.45f));
    for (const auto& e : ev)
        CHECK (nearLen (e.lengthBeats, 0.25));

    // Rolling16 at gate 1 would last 0.2667 beat: cut at the next onset (and at the slot end) to 0.25.
    ev = bassOf (prog, voicings, bassSettings (BassMode::Rolling16, RateDivision::Eighth, 0.0f, 0.0f, 1.0f));
    for (const auto& e : ev)
        CHECK (nearLen (e.lengthBeats, 0.25));

    // Octave bounce at gate 1: 1.6 / 0.9 * 0.25 = 0.444 beat < the 0.5 spacing: no cut.
    ev = bassOf (prog, voicings, bassSettings (BassMode::OctaveBounce8, RateDivision::Eighth, 0.0f, 0.0f, 1.0f));
    for (const auto& e : ev)
        CHECK (nearLen (e.lengthBeats, 1.6 * 0.25 / 0.9));

    // Dotted quarters: 0, 6, 12 per bar, each 2 sixteenths.
    ev = bassOf (prog, voicings, bassSettings (BassMode::DottedQuarter));
    REQUIRE (ev.size() == 12);
    CHECK (near (ev[1].startBeat, 1.5));
    CHECK (nearLen (ev[1].lengthBeats, 0.5));
    CHECK (near (ev[3].startBeat, 4.0));
}

TEST_CASE ("bass patterns: the 'o' steps play the Octaves mode's partner of R, voicing-proximity clause included")
{
    // i-VI-III-VII in all 12 keys at the default voicing, both patterns with an 'o' step: every 'x' step of a slot plays
    // R, every 'o' step plays referencePartner (R, lowest voicing note), which is R - 12 whenever R + 12 is not at least
    // 5 below the chord.
    int rises = 0;
    int drops = 0;
    int proximityDrops = 0;
    for (int key = 0; key < 12; ++key)
    {
        const auto prog = makeProgression ({ chordOf ((9 + key) % 12, ChordQuality::Minor), chordOf ((5 + key) % 12),
                                             chordOf (key), chordOf ((7 + key) % 12) });
        const auto voicings = voiced (prog);

        for (auto mode : { BassMode::OctaveBounce8, BassMode::RollingKickGap })
        {
            const auto ev = bassOf (prog, voicings, bassSettings (mode));
            const auto grid = bassPatternGrid (mode);
            REQUIRE (! ev.empty());

            for (int bar = 0; bar < 4; ++bar)
            {
                const int lowest = voicings[(size_t) bar].front();
                int root = -1;
                for (const auto& e : ev)
                {
                    const int step = (int) std::lround (e.startBeat * 4.0) - 16 * bar;
                    if (step < 0 || step > 15 || grid[(size_t) step] != 'x')
                        continue;
                    if (root < 0)
                        root = e.note;
                    CHECK_EQ (e.note, root);
                }
                REQUIRE (root >= 28);
                REQUIRE (root <= 55);

                for (const auto& e : ev)
                {
                    const int step = (int) std::lround (e.startBeat * 4.0) - 16 * bar;
                    if (step < 0 || step > 15 || grid[(size_t) step] != 'o')
                        continue;
                    CHECK_EQ (e.note, referencePartner (root, lowest));
                    CHECK_EQ (pc (e.note), pc (root));
                    CHECK (e.note >= 28 && e.note <= 55);
                    if (e.note > root)
                        ++rises;
                    else
                    {
                        ++drops;
                        proximityDrops += root + 12 <= 55 ? 1 : 0;
                    }
                    // An upper partner is always at least 5 below the chord (unless R - 12 would leave the range).
                    CHECK (e.note <= lowest - 5 || e.note < root || root - 12 < 28);
                }
            }
        }
    }
    CHECK (rises > 0);
    CHECK (drops > 0);
    CHECK (proximityDrops > 0);   // the proximity clause really decides some partners

    // The reported case: F voiced F3 A3 C4 chooses R = F2 (41); F3 would double the pad's bottom note, so the partner
    // is F1 (29).
    const auto fProg = makeProgression ({ chordOf (5) });
    const auto fEv = bassOf (fProg, { { 53, 57, 60 } }, bassSettings (BassMode::OctaveBounce8));
    REQUIRE (fEv.size() >= 2);
    CHECK_EQ (fEv[0].note, 41);
    CHECK_EQ (fEv[1].note, 29);

    // The same partner as the classic Octaves mode's second note of the same slot.
    for (int key = 0; key < 12; ++key)
    {
        const auto one = makeProgression ({ chordOf (key) });
        const auto v = voiced (one);
        const auto classic = bassOf (one, v, bassSettings (BassMode::Octaves));
        const auto pattern = bassOf (one, v, bassSettings (BassMode::OctaveBounce8));
        REQUIRE (classic.size() >= 2);
        REQUIRE (pattern.size() >= 2);
        CHECK_EQ (pattern[0].note, classic[0].note);
        CHECK_EQ (pattern[1].note, classic[1].note);
    }
}

TEST_CASE ("bass patterns: note lengths are not swing-warped (lengthSteps * 0.25 * gate / 0.9, cut at the actual next onset)")
{
    const auto prog = makeProgression ({ chordOf (0), chordOf (5) });
    const auto v = voiced (prog);

    for (float swing : { 0.5f, 1.0f })
    {
        // Offbeat8 (chord rate 1/8): each onset is swung, the length stays 2 sixteenths = 0.5 beat (the last note of a
        // 4-beat slot is cut at the slot end).
        const auto ev = bassOf (prog, v, bassSettings (BassMode::Offbeat8, RateDivision::Eighth, swing));
        REQUIRE (ev.size() == 8);
        for (const auto& e : ev)
        {
            CHECK (near (e.startBeat, std::floor (e.startBeat) + 0.5 + (double) swing * 0.5 / 3.0));
            const double slotEnd = 4.0 * (std::floor (e.startBeat / 4.0) + 1.0);
            CHECK (nearLen (e.lengthBeats, std::min (0.5, slotEnd - e.startBeat)));
        }

        // Rolling16 (chord rate 1/16): the even 16th keeps 0.24 beat, the swung odd 16th is cut at the next onset.
        const auto roll = bassOf (prog, v, bassSettings (BassMode::Rolling16, RateDivision::Sixteenth, swing));
        REQUIRE (roll.size() >= 4);
        const double longSpan = 0.25 * (1.0 + (double) swing / 3.0);
        CHECK (nearLen (roll[0].lengthBeats, 0.24));
        CHECK (nearLen (roll[1].lengthBeats, std::min (0.24, 0.5 - longSpan)));
        CHECK (nearLen (roll[2].lengthBeats, 0.24));
        CHECK (roll[0].startBeat + roll[0].lengthBeats <= roll[1].startBeat + kTol);
        CHECK (roll[1].startBeat + roll[1].lengthBeats <= roll[2].startBeat + kTol);
    }
}

TEST_CASE ("bass patterns: slot lengths 1, 2, 3, 5 (14 beats) - grid, strikes, entry, ranges, no overlaps")
{
    const auto prog = oddSlots();
    const auto voicings = voiced (prog);
    REQUIRE (prog.totalBeats() == 14);

    int renders = 0;
    for (auto mode : kPatternModes)
        for (auto rate : kAllRates)
            for (float swing : { 0.0f, 0.5f, 1.0f })
                for (float humanize : { 0.0f, 1.0f })
                    for (float gate : { 0.05f, 0.9f, 1.0f })
                    {
                        const auto s = bassSettings (mode, rate, swing, humanize, gate);
                        const auto ev = bassOf (prog, voicings, s);
                        ++renders;
                        REQUIRE (! ev.empty());

                        // Every slot has at least one bass note; entry = chooser with previous = the last emitted note.
                        int previous = 40;
                        std::vector<int> perSlot (prog.slots.size(), 0);
                        for (size_t i = 0; i < ev.size(); ++i)
                        {
                            const auto& e = ev[i];
                            const int slot = prog.slotAtBeat (e.startBeat);
                            REQUIRE (slot >= 0);
                            const auto& sl = prog.slots[(size_t) slot];
                            const int lowest = voicings[(size_t) slot].front();

                            CHECK (e.startBeat >= prog.slotStartBeat (slot) - kTol);
                            CHECK (e.startBeat + e.lengthBeats <= prog.slotStartBeat (slot + 1) + kTol);
                            CHECK (e.lengthBeats > 0.0);
                            CHECK (e.velocity > 0.0f && e.velocity <= 1.0f);
                            CHECK (e.note <= 55);
                            CHECK (e.note >= 28 || (e.note >= 24 && e.note < lowest));
                            CHECK_EQ (pc (e.note), pc (sl.chord.root));
                            if (i > 0)
                                CHECK (ev[i - 1].startBeat < e.startBeat);

                            if (perSlot[(size_t) slot]++ == 0)
                                CHECK_EQ (e.note, referenceRoot (pc (sl.chord.root), lowest, previous));
                            previous = e.note;

                            for (size_t j = i + 1; j < ev.size(); ++j)
                                if (ev[j].note == e.note)
                                {
                                    CHECK (e.startBeat + e.lengthBeats <= ev[j].startBeat + kTol);
                                    break;
                                }
                        }
                        for (int n : perSlot)
                            CHECK (n >= 1);

                        // Offbeat family: never an onset at a slot start.
                        if (bassPatternAvoidsKick (mode) && humanize == 0.0f)
                            for (int slot = 0; slot < (int) prog.slots.size(); ++slot)
                                for (const auto& e : ev)
                                    CHECK (! near (e.startBeat, prog.slotStartBeat (slot)));

                        // Humanize 0: exactly the reference model (absolute grid, swingWarp, strikes, lengths).
                        if (humanize == 0.0f)
                        {
                            const auto expected = expectedBass (prog, voicings, s);
                            REQUIRE (ev.size() == expected.size());
                            for (size_t i = 0; i < ev.size(); ++i)
                            {
                                CHECK (near (ev[i].startBeat, expected[i].start));
                                CHECK (nearLen (ev[i].lengthBeats, expected[i].length));
                                CHECK_EQ (ev[i].note, expected[i].note);
                            }
                        }
                    }
    CHECK_EQ (renders, 10 * 4 * 3 * 2 * 3);
}

TEST_CASE ("bass patterns: on-beat patterns strike R at a slot start exactly when no onset lies in the first half beat")
{
    const auto prog = oddSlots();
    const auto voicings = voiced (prog);

    for (auto mode : kPatternModes)
    {
        std::vector<std::vector<bool>> strikes;
        for (float swing : { 0.0f, 0.5f, 1.0f })
        {
            std::vector<bool> perSlot;
            const auto ev = bassOf (prog, voicings, bassSettings (mode, RateDivision::Sixteenth, swing));
            for (int slot = 0; slot < (int) prog.slots.size(); ++slot)
            {
                const int g0 = (int) std::lround (prog.slotStartBeat (slot) * 4.0);

                bool atStart = false;
                for (const auto& e : ev)
                    atStart = atStart || near (e.startBeat, prog.slotStartBeat (slot));

                // A note at the slot start that is not a pattern onset is the strike.
                const bool strike = atStart && ! patternHasStep (mode, g0 % 16);
                perSlot.push_back (strike);
                CHECK_EQ (strike, ! bassPatternAvoidsKick (mode) && ! patternHasEarlyOnset (mode, g0));
                if (bassPatternAvoidsKick (mode))
                    CHECK (! atStart);
            }
            strikes.push_back (perSlot);
        }
        CHECK (strikes[0] == strikes[1]);
        CHECK (strikes[0] == strikes[2]);
    }

    // DottedQuarter (0, 6, 12): slot 0 = beats 0-2 (onsets 0 and 6), slot 1 = beat 2 (steps 8..11: no onset, so R is
    // struck and lasts the whole slot), slot 2 = beat 3 (step 12).
    const std::vector<std::vector<int>> cChord { { 48, 52, 55 }, { 48, 52, 55 }, { 48, 52, 55 } };
    const auto dotted = bassOf (makeProgression ({ chordOf (0), chordOf (0), chordOf (0) }, { 2, 1, 1 }), cChord,
                                bassSettings (BassMode::DottedQuarter));
    REQUIRE (dotted.size() == 4);
    CHECK (near (dotted[0].startBeat, 0.0));
    CHECK (near (dotted[1].startBeat, 1.5));
    CHECK (near (dotted[2].startBeat, 2.0));
    CHECK (nearLen (dotted[2].lengthBeats, 1.0));
    CHECK (near (dotted[3].startBeat, 3.0));

    // Tresillo (0, 3, 6, 8, 11, 14): a 1-beat slot at beat 1 has step 6 within half a beat: no strike.
    const auto tresillo = bassOf (makeProgression ({ chordOf (0), chordOf (0) }, { 1, 1 }), { { 48, 52, 55 }, { 48, 52, 55 } },
                                  bassSettings (BassMode::Tresillo));
    REQUIRE (tresillo.size() == 3);   // slot 0: steps 0, 3; slot 1: step 6
    CHECK (near (tresillo[2].startBeat, 1.5));

    // The strike lasts to the first onset (or the slot end) times min (1, gate / 0.9).
    const auto lowGate = bassOf (makeProgression ({ chordOf (0), chordOf (0) }, { 2, 1 }), { { 48, 52, 55 }, { 48, 52, 55 } },
                                 bassSettings (BassMode::DottedQuarter, RateDivision::Eighth, 0.0f, 0.0f, 0.45f));
    REQUIRE (lowGate.size() == 3);
    CHECK (near (lowGate[2].startBeat, 2.0));
    CHECK (nearLen (lowGate[2].lengthBeats, 0.5));
}

TEST_CASE ("bass patterns: slash chords and explicit inversions follow slotBassPitchClass")
{
    for (auto mode : kPatternModes)
        for (int inv = 0; inv < 3; ++inv)
            for (bool withVoicing : { true, false })
            {
                auto prog = makeProgression ({ chordOf (0), chordOf (7, ChordQuality::Dominant7) }, { 4, 3 });
                prog.slots[0].inversion = inv;
                prog.slots[1].inversion = inv;
                const auto v = voiced (prog);
                const std::vector<std::vector<int>> given = withVoicing ? v : std::vector<std::vector<int>> {};

                const auto ev = bassOf (prog, given, bassSettings (mode));
                REQUIRE (! ev.empty());
                for (const auto& e : ev)
                {
                    const int slot = prog.slotAtBeat (e.startBeat);
                    const int expectedPc = slotBassPitchClass (prog.slots[(size_t) slot],
                                                               withVoicing ? v[(size_t) slot] : std::vector<int> {});
                    CHECK_EQ (pc (e.note), expectedPc);
                    CHECK (e.note >= 28 && e.note <= 55);
                }

                const int firstPc = slotBassPitchClass (prog.slots[0], withVoicing ? v[0] : std::vector<int> {});
                CHECK_EQ (pc (ev.front().note), firstPc);
                if (withVoicing)
                    CHECK_EQ (firstPc, pc (v[0].front()));
            }

    // C/E explicitly: the bass is E (the chooser's note with previous 40) in a pattern mode, and the data function
    // agrees with the voicing, ignores out-of-range notes and falls back to the inversion's chord tone.
    auto slash = makeProgression ({ chordOf (0) });
    slash.slots[0].inversion = 1;
    const std::vector<std::vector<int>> cOverE { { 52, 55, 60 } };
    CHECK_EQ (slotBassPitchClass (slash.slots[0], cOverE[0]), 4);
    const auto ev = bassOf (slash, cOverE, bassSettings (BassMode::Driving8));
    REQUIRE (! ev.empty());
    CHECK_EQ (pc (ev.front().note), 4);
    CHECK_EQ (ev.front().note, referenceRoot (4, 52, 40));
    CHECK_EQ (slotBassPitchClass (slash.slots[0], { 200, -3, 55, 60 }), 7);
    CHECK_EQ (slotBassPitchClass (slash.slots[0], {}), 4);
    slash.slots[0].inversion = -1;
    CHECK_EQ (slotBassPitchClass (slash.slots[0], cOverE[0]), 0);
}

TEST_CASE ("groove: swingWarp is monotonic, fixes the window edges and is the identity without swing or grid")
{
    for (double grid : { 0.25, 0.5, 1.0 / 3.0, 1.0 })
        for (double swing : { 0.0, 0.1, 0.5, 1.0 })
        {
            double previous = swingWarp (0.0, grid, swing);
            CHECK_EQ (previous, 0.0);
            for (int i = 1; i <= 2048; ++i)
            {
                const double t = (double) i / 128.0;
                const double w = swingWarp (t, grid, swing);
                CHECK (w >= previous);
                previous = w;
            }

            for (int k = 0; k < 8; ++k)
            {
                const double edge = k * 2.0 * grid;
                CHECK (near (swingWarp (edge, grid, swing), edge, 1.0e-12));
                CHECK (near (swingWarp (edge + grid, grid, swing), edge + grid + swing * grid / 3.0, 1.0e-12));
            }

            if (swing == 0.0)
                for (int i = 0; i < 100; ++i)
                    CHECK_EQ (swingWarp (i * 0.123, grid, swing), i * 0.123);
        }

    for (int i = 0; i < 100; ++i)
    {
        const double t = i * 0.0771;
        CHECK_EQ (swingWarp (t, 0.0, 1.0), t);
        CHECK_EQ (swingWarp (t, -0.5, 1.0), t);
        CHECK_EQ (swingWarp (t, 0.5, -1.0), t);
        CHECK_EQ (swingWarp (t, 0.5, 7.0), swingWarp (t, 0.5, 1.0));
    }

    const std::pair<RateDivision, double> table[] = { { RateDivision::Quarter, 0.5 }, { RateDivision::Eighth, 0.5 },
                                                      { RateDivision::Sixteenth, 0.25 },
                                                      { RateDivision::EighthTriplet, 0.0 } };
    RenderSettings rs;
    for (const auto& row : table)
    {
        rs.rate = row.first;
        CHECK_EQ (grooveGridBeats (rs), row.second);
    }
}

namespace
{
// Straight start (in 1/48 beats) -> swung start of every chord step a Pulse chord renders at this rate and swing.
std::map<long, double> chordStepTimes (const Progression& prog, const std::vector<std::vector<int>>& v,
                                       RateDivision rate, float swing)
{
    RenderSettings straight = bassSettings (BassMode::Off, rate);
    straight.pattern = RhythmPattern::Pulse;
    RenderSettings swung = straight;
    swung.swing = swing;

    const auto a = onChannel (renderProgression (prog, v, straight), 1);
    const auto b = onChannel (renderProgression (prog, v, swung), 1);

    std::map<long, double> times;
    if (a.size() == b.size())
        for (size_t i = 0; i < a.size(); ++i)
            times[std::lround (a[i].startBeat * 48.0)] = b[i].startBeat;
    return times;
}

// Checks every onset of `straight` / `swung` (the same render at swing 0 and at some swing) that coincides with a chord
// step. Returns the number of onsets checked; `swungCount` counts those the swing actually moved.
int checkAgainstChordSteps (const std::vector<NoteEvent>& straight, const std::vector<NoteEvent>& swung,
                            const std::map<long, double>& chordSteps, int& swungCount)
{
    int matched = 0;
    if (straight.size() != swung.size())
    {
        CHECK_EQ (straight.size(), swung.size());
        return 0;
    }

    for (size_t i = 0; i < straight.size(); ++i)
    {
        const double t = straight[i].startBeat;
        const auto it = chordSteps.find (std::lround (t * 48.0));
        if (it == chordSteps.end() || std::abs (t * 48.0 - std::round (t * 48.0)) > 1.0e-6)
            continue;

        ++matched;
        CHECK (near (swung[i].startBeat, it->second));
        if (std::abs (it->second - t) > 1.0e-6)
            ++swungCount;
    }
    return matched;
}
} // namespace

TEST_CASE ("groove: bass onsets coincident with a chord Pulse step start at the chord's swung time")
{
    const std::vector<Progression> progressions { cAmFG(), oddSlots() };

    int matchedTotal = 0;
    int swungTotal = 0;
    for (const auto& prog : progressions)
    {
        const auto v = voiced (prog);
        for (auto rate : { RateDivision::Eighth, RateDivision::Sixteenth })
            for (float swing : { 0.5f, 1.0f })
            {
                const auto steps = chordStepTimes (prog, v, rate, swing);
                REQUIRE (! steps.empty());
                for (auto mode : kPatternModes)
                {
                    const auto straight = bassOf (prog, v, bassSettings (mode, rate, 0.0f));
                    const auto swung = bassOf (prog, v, bassSettings (mode, rate, swing));
                    matchedTotal += checkAgainstChordSteps (straight, swung, steps, swungTotal);
                }
            }
    }
    CHECK (matchedTotal > 1000);
    CHECK (swungTotal > 400);
}

TEST_CASE ("groove: arp onsets coincident with a chord Pulse step start at the chord's swung time")
{
    const std::vector<Progression> progressions { cAmFG(), oddSlots() };

    int matchedTotal = 0;
    int swungTotal = 0;
    for (const auto& prog : progressions)
    {
        const auto v = voiced (prog);
        for (auto chordRate : { RateDivision::Eighth, RateDivision::Sixteenth })
            for (auto arpRate : { RateDivision::Eighth, RateDivision::Sixteenth })
                for (float swing : { 0.5f, 1.0f })
                {
                    const auto steps = chordStepTimes (prog, v, chordRate, swing);
                    ArpSettings a;
                    a.rate = arpRate;
                    a.gate = 0.5f;
                    const auto straight = renderArp (prog, v, a, bassSettings (BassMode::Off, chordRate, 0.0f), {});
                    const auto swung = renderArp (prog, v, a, bassSettings (BassMode::Off, chordRate, swing), {});
                    matchedTotal += checkAgainstChordSteps (straight, swung, steps, swungTotal);
                }
    }
    CHECK (matchedTotal > 500);
    CHECK (swungTotal > 100);
}

TEST_CASE ("groove: at chord rate 1/4 the layers swing their 8ths and at 1/8T nothing swings")
{
    const auto prog = cAmFG();
    const auto v = voiced (prog);

    for (float swing : { 0.5f, 1.0f })
    {
        // Offbeat8 has its onsets on the 8th offbeats (x.5): swung by swing * 0.5 / 3 at chord rate 1/4.
        const auto ev = bassOf (prog, v, bassSettings (BassMode::Offbeat8, RateDivision::Quarter, swing));
        REQUIRE (ev.size() == 16);
        for (size_t i = 0; i < ev.size(); ++i)
            CHECK (near (ev[i].startBeat, (double) i + 0.5 + (double) swing * 0.5 / 3.0));

        // Rolling16: step 2 is the swung 8th, step 4 the next beat.
        const auto rolling = bassOf (prog, v, bassSettings (BassMode::Rolling16, RateDivision::Quarter, swing));
        CHECK (near (rolling[2].startBeat, 0.5 + (double) swing * 0.5 / 3.0));
        CHECK (near (rolling[4].startBeat, 1.0));

        ArpSettings a;
        a.rate = RateDivision::Eighth;
        const auto arp = renderArp (prog, v, a, bassSettings (BassMode::Off, RateDivision::Quarter, swing), {});
        REQUIRE (arp.size() == 32);
        CHECK (near (arp[1].startBeat, 0.5 + (double) swing * 0.5 / 3.0));
        CHECK (near (arp[2].startBeat, 1.0));
    }

    for (auto mode : kPatternModes)
    {
        const auto a = bassOf (prog, v, bassSettings (mode, RateDivision::EighthTriplet, 0.0f));
        const auto b = bassOf (prog, v, bassSettings (mode, RateDivision::EighthTriplet, 1.0f));
        REQUIRE (a.size() == b.size());
        for (size_t i = 0; i < a.size(); ++i)
            CHECK (a[i] == b[i]);
    }
}

TEST_CASE ("bass patterns: Rate does not apply to patterns, classic modes keep following it")
{
    const auto prog = cAmFG();
    const auto v = voiced (prog);

    CHECK_EQ (bassOf (prog, v, bassSettings (BassMode::Pulse, RateDivision::Sixteenth)).size(), (size_t) 64);
    CHECK_EQ (bassOf (prog, v, bassSettings (BassMode::Pulse, RateDivision::Quarter)).size(), (size_t) 16);
    for (auto rate : kAllRates)
        CHECK (bassOf (prog, v, bassSettings (BassMode::Gallop, rate)) == bassOf (prog, v, bassSettings (BassMode::Gallop)));

    CHECK (bassOf (prog, v, bassSettings (BassMode::Off)).empty());
    CHECK_EQ (bassOf (prog, v, bassSettings (BassMode::Root)).size(), (size_t) 4);
}

TEST_CASE ("bass patterns: humanize jitters timing but never leaves the slot, and is deterministic")
{
    const auto prog = oddSlots();
    const auto v = voiced (prog);

    for (auto mode : kPatternModes)
    {
        const auto plain = bassOf (prog, v, bassSettings (mode, RateDivision::Eighth, 0.0f, 0.0f));
        const auto human = bassOf (prog, v, bassSettings (mode, RateDivision::Eighth, 0.0f, 1.0f));
        REQUIRE (plain.size() == human.size());

        bool anyMoved = false;
        for (size_t i = 0; i < plain.size(); ++i)
        {
            CHECK_EQ (plain[i].note, human[i].note);
            CHECK (std::abs (plain[i].startBeat - human[i].startBeat) <= 0.015 + kTol);
            anyMoved = anyMoved || plain[i].startBeat != human[i].startBeat;
        }
        CHECK (anyMoved);

        CHECK (human == bassOf (prog, v, bassSettings (mode, RateDivision::Eighth, 0.0f, 1.0f)));
    }
}
