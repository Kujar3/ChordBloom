#include "TestFramework.h"

#include "core/Pattern.h"
#include "core/Progression.h"
#include "core/Voicing.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Golden protection of renderProgression (owner: WP0, frozen; final-spec 8.1). The hashes below were captured from the
// unmodified phase-1.5 implementation (commit daabcbc) before any phase-2 change to Pattern.cpp. They must never be
// re-captured: a mismatch means the classic bass modes or the chord part changed, which phase 2 forbids.
//
//  Bass table:  5 classic bass modes x 4 rates x swing {0, 0.5} x humanize {0, 1} x gate {0.9, 0.5} x 3 progressions
//               (Block chords) = 480 hashes, every event of renderProgression (chords ch 1 + bass ch 2).
//  Chord table: 8 rhythm patterns x 4 rates x swing {0, 0.5} x humanize {0, 1} x 3 progressions (bass Root, gate 0.9)
//               = 384 hashes.
//  Hash: 64-bit FNV-1a over every event's (note, round (start * 1e6), round (length * 1e6), round (velocity * 1e4),
//  channel), voicings from voiceProgression (Close, centre 60, voice leading on).

using namespace cb;

namespace
{
struct Fnv
{
    std::uint64_t h = 1469598103934665603ull;

    void add (std::int64_t v)
    {
        auto u = (std::uint64_t) v;
        for (int i = 0; i < 8; ++i)
        {
            h ^= (u >> (8 * i)) & 0xffu;
            h *= 1099511628211ull;
        }
    }
};

std::uint64_t hashEvents (const std::vector<NoteEvent>& events)
{
    Fnv f;
    for (const auto& e : events)
    {
        f.add (e.note);
        f.add ((std::int64_t) std::llround (e.startBeat * 1.0e6));
        f.add ((std::int64_t) std::llround (e.lengthBeats * 1.0e6));
        f.add ((std::int64_t) std::llround ((double) e.velocity * 1.0e4));
        f.add (e.channel);
    }
    return f.h;
}

ProgressionSlot slot (int root, ChordQuality q, int beats, int inversion = -1)
{
    ProgressionSlot s;
    s.chord = Chord { root, q };
    s.lengthBeats = beats;
    s.inversion = inversion;
    return s;
}

std::vector<Progression> goldenProgressions()
{
    Progression a;   // C - Am - F - G, 4 beats each
    a.slots = { slot (0, ChordQuality::Major, 4), slot (9, ChordQuality::Minor, 4), slot (5, ChordQuality::Major, 4),
                slot (7, ChordQuality::Major, 4) };

    Progression b;   // 1, 3, 4, 2, 5 beats with colour chords
    b.slots = { slot (0, ChordQuality::Major9, 1), slot (2, ChordQuality::Minor, 3), slot (5, ChordQuality::Add9, 4),
                slot (7, ChordQuality::Dominant7, 2), slot (9, ChordQuality::Sus2, 5) };

    Progression c;   // slash chords / explicit inversions: C/E, Am7, F/C, G7/F, Dm (root position, explicit)
    c.slots = { slot (0, ChordQuality::Major, 4, 1), slot (9, ChordQuality::Minor7, 2), slot (5, ChordQuality::Major, 3, 2),
                slot (7, ChordQuality::Dominant7, 1, 3), slot (2, ChordQuality::Minor, 2, 0) };

    return { a, b, c };
}

std::vector<std::vector<int>> goldenVoicings (const Progression& p)
{
    VoicingSettings vs;
    vs.style = VoicingStyle::Close;
    vs.centerNote = 60;
    vs.voiceLeading = true;
    return voiceProgression (p, vs);
}

constexpr BassMode kClassicModes[] = { BassMode::Off, BassMode::Root, BassMode::RootFifth, BassMode::Octaves,
                                       BassMode::Pulse };
constexpr float kSwings[] = { 0.0f, 0.5f };
constexpr float kHumanizes[] = { 0.0f, 1.0f };
constexpr float kGates[] = { 0.9f, 0.5f };

// Iteration order: mode, rate, swing, humanize, gate, progression (innermost).
std::vector<std::uint64_t> computeBassTable()
{
    std::vector<std::uint64_t> out;
    const auto progs = goldenProgressions();
    for (auto mode : kClassicModes)
        for (int rate = 0; rate < 4; ++rate)
            for (float swing : kSwings)
                for (float humanize : kHumanizes)
                    for (float gate : kGates)
                        for (const auto& p : progs)
                        {
                            RenderSettings rs;
                            rs.pattern = RhythmPattern::Block;
                            rs.rate = (RateDivision) rate;
                            rs.bass = mode;
                            rs.swing = swing;
                            rs.humanize = humanize;
                            rs.gate = gate;
                            out.push_back (hashEvents (renderProgression (p, goldenVoicings (p), rs)));
                        }
    return out;
}

// Iteration order: pattern, rate, swing, humanize, progression (innermost).
std::vector<std::uint64_t> computeChordTable()
{
    std::vector<std::uint64_t> out;
    const auto progs = goldenProgressions();
    for (int pattern = 0; pattern < 8; ++pattern)
        for (int rate = 0; rate < 4; ++rate)
            for (float swing : kSwings)
                for (float humanize : kHumanizes)
                    for (const auto& p : progs)
                    {
                        RenderSettings rs;
                        rs.pattern = (RhythmPattern) pattern;
                        rs.rate = (RateDivision) rate;
                        rs.bass = BassMode::Root;
                        rs.swing = swing;
                        rs.humanize = humanize;
                        rs.gate = 0.9f;
                        out.push_back (hashEvents (renderProgression (p, goldenVoicings (p), rs)));
                    }
    return out;
}

std::string hex (std::uint64_t v)
{
    char buf[32];
    std::snprintf (buf, sizeof (buf), "0x%016llxull", (unsigned long long) v);
    return buf;
}

// Captured from daabcbc (unmodified Pattern.cpp) on 2026-10-20. Never re-capture.
const std::uint64_t kGoldenBass[] = {
    0x107d9e8014d9ebb3ull, 0xb1866aab0740c55eull, 0x50bed6aafec04c01ull, 0x07a7f6d5d0d0b4dfull,
    0x1c59cf47d7e109cbull, 0xb35e11eacba5a42dull, 0xeeb8b6cc5c769aafull, 0x40a236b5a68ddc68ull,
    0xebc4447ad3b88c33ull, 0xbe223988713b4747ull, 0x335f804e3dd6f9bdull, 0xbb777a35db319f7bull,
    0x107d9e8014d9ebb3ull, 0xb1866aab0740c55eull, 0x50bed6aafec04c01ull, 0x07a7f6d5d0d0b4dfull,
    0x1c59cf47d7e109cbull, 0xb35e11eacba5a42dull, 0xeeb8b6cc5c769aafull, 0x40a236b5a68ddc68ull,
    0xebc4447ad3b88c33ull, 0xbe223988713b4747ull, 0x335f804e3dd6f9bdull, 0xbb777a35db319f7bull,
    0x107d9e8014d9ebb3ull, 0xb1866aab0740c55eull, 0x50bed6aafec04c01ull, 0x07a7f6d5d0d0b4dfull,
    0x1c59cf47d7e109cbull, 0xb35e11eacba5a42dull, 0xeeb8b6cc5c769aafull, 0x40a236b5a68ddc68ull,
    0xebc4447ad3b88c33ull, 0xbe223988713b4747ull, 0x335f804e3dd6f9bdull, 0xbb777a35db319f7bull,
    0x107d9e8014d9ebb3ull, 0xb1866aab0740c55eull, 0x50bed6aafec04c01ull, 0x07a7f6d5d0d0b4dfull,
    0x1c59cf47d7e109cbull, 0xb35e11eacba5a42dull, 0xeeb8b6cc5c769aafull, 0x40a236b5a68ddc68ull,
    0xebc4447ad3b88c33ull, 0xbe223988713b4747ull, 0x335f804e3dd6f9bdull, 0xbb777a35db319f7bull,
    0x107d9e8014d9ebb3ull, 0xb1866aab0740c55eull, 0x50bed6aafec04c01ull, 0x07a7f6d5d0d0b4dfull,
    0x1c59cf47d7e109cbull, 0xb35e11eacba5a42dull, 0xeeb8b6cc5c769aafull, 0x40a236b5a68ddc68ull,
    0xebc4447ad3b88c33ull, 0xbe223988713b4747ull, 0x335f804e3dd6f9bdull, 0xbb777a35db319f7bull,
    0x107d9e8014d9ebb3ull, 0xb1866aab0740c55eull, 0x50bed6aafec04c01ull, 0x07a7f6d5d0d0b4dfull,
    0x1c59cf47d7e109cbull, 0xb35e11eacba5a42dull, 0xeeb8b6cc5c769aafull, 0x40a236b5a68ddc68ull,
    0xebc4447ad3b88c33ull, 0xbe223988713b4747ull, 0x335f804e3dd6f9bdull, 0xbb777a35db319f7bull,
    0x107d9e8014d9ebb3ull, 0xb1866aab0740c55eull, 0x50bed6aafec04c01ull, 0x07a7f6d5d0d0b4dfull,
    0x1c59cf47d7e109cbull, 0xb35e11eacba5a42dull, 0xeeb8b6cc5c769aafull, 0x40a236b5a68ddc68ull,
    0xebc4447ad3b88c33ull, 0xbe223988713b4747ull, 0x335f804e3dd6f9bdull, 0xbb777a35db319f7bull,
    0x107d9e8014d9ebb3ull, 0xb1866aab0740c55eull, 0x50bed6aafec04c01ull, 0x07a7f6d5d0d0b4dfull,
    0x1c59cf47d7e109cbull, 0xb35e11eacba5a42dull, 0xeeb8b6cc5c769aafull, 0x40a236b5a68ddc68ull,
    0xebc4447ad3b88c33ull, 0xbe223988713b4747ull, 0x335f804e3dd6f9bdull, 0xbb777a35db319f7bull,
    0x68205373618dbf50ull, 0x280752323234043full, 0x64c16c034f077e8eull, 0xdc8eb629d24be1d0ull,
    0x81d3ee51bfc26d36ull, 0x3a3a6e1254bb41caull, 0x8681f6156d0b9a35ull, 0xb284497e0c544dc7ull,
    0x50128e32eaa589aaull, 0x8fbac49586dd4dadull, 0xde883f79f51bca52ull, 0x665530b7ad4192faull,
    0x68205373618dbf50ull, 0x280752323234043full, 0x64c16c034f077e8eull, 0xdc8eb629d24be1d0ull,
    0x81d3ee51bfc26d36ull, 0x3a3a6e1254bb41caull, 0x8681f6156d0b9a35ull, 0xb284497e0c544dc7ull,
    0x50128e32eaa589aaull, 0x8fbac49586dd4dadull, 0xde883f79f51bca52ull, 0x665530b7ad4192faull,
    0x68205373618dbf50ull, 0x280752323234043full, 0x64c16c034f077e8eull, 0xdc8eb629d24be1d0ull,
    0x81d3ee51bfc26d36ull, 0x3a3a6e1254bb41caull, 0x8681f6156d0b9a35ull, 0xb284497e0c544dc7ull,
    0x50128e32eaa589aaull, 0x8fbac49586dd4dadull, 0xde883f79f51bca52ull, 0x665530b7ad4192faull,
    0x68205373618dbf50ull, 0x280752323234043full, 0x64c16c034f077e8eull, 0xdc8eb629d24be1d0ull,
    0x81d3ee51bfc26d36ull, 0x3a3a6e1254bb41caull, 0x8681f6156d0b9a35ull, 0xb284497e0c544dc7ull,
    0x50128e32eaa589aaull, 0x8fbac49586dd4dadull, 0xde883f79f51bca52ull, 0x665530b7ad4192faull,
    0x68205373618dbf50ull, 0x280752323234043full, 0x64c16c034f077e8eull, 0xdc8eb629d24be1d0ull,
    0x81d3ee51bfc26d36ull, 0x3a3a6e1254bb41caull, 0x8681f6156d0b9a35ull, 0xb284497e0c544dc7ull,
    0x50128e32eaa589aaull, 0x8fbac49586dd4dadull, 0xde883f79f51bca52ull, 0x665530b7ad4192faull,
    0x68205373618dbf50ull, 0x280752323234043full, 0x64c16c034f077e8eull, 0xdc8eb629d24be1d0ull,
    0x81d3ee51bfc26d36ull, 0x3a3a6e1254bb41caull, 0x8681f6156d0b9a35ull, 0xb284497e0c544dc7ull,
    0x50128e32eaa589aaull, 0x8fbac49586dd4dadull, 0xde883f79f51bca52ull, 0x665530b7ad4192faull,
    0x68205373618dbf50ull, 0x280752323234043full, 0x64c16c034f077e8eull, 0xdc8eb629d24be1d0ull,
    0x81d3ee51bfc26d36ull, 0x3a3a6e1254bb41caull, 0x8681f6156d0b9a35ull, 0xb284497e0c544dc7ull,
    0x50128e32eaa589aaull, 0x8fbac49586dd4dadull, 0xde883f79f51bca52ull, 0x665530b7ad4192faull,
    0x68205373618dbf50ull, 0x280752323234043full, 0x64c16c034f077e8eull, 0xdc8eb629d24be1d0ull,
    0x81d3ee51bfc26d36ull, 0x3a3a6e1254bb41caull, 0x8681f6156d0b9a35ull, 0xb284497e0c544dc7ull,
    0x50128e32eaa589aaull, 0x8fbac49586dd4dadull, 0xde883f79f51bca52ull, 0x665530b7ad4192faull,
    0x6a4f456d26c1002bull, 0x43aa740527ebfdddull, 0xd5177b6c958b5063ull, 0xa66266422ddeb977ull,
    0xac215909c9106f50ull, 0x4c0bd78cc5c1ccafull, 0xf05f02d27217623eull, 0x1f9517a9faa37e87ull,
    0x15c65de3c822d3f4ull, 0x9a469566769f8ca2ull, 0x8c102b69afbf387aull, 0xdd2cb145b8cc57e8ull,
    0x6a4f456d26c1002bull, 0x43aa740527ebfdddull, 0xd5177b6c958b5063ull, 0xa66266422ddeb977ull,
    0xac215909c9106f50ull, 0x4c0bd78cc5c1ccafull, 0xf05f02d27217623eull, 0x1f9517a9faa37e87ull,
    0x15c65de3c822d3f4ull, 0x9a469566769f8ca2ull, 0x8c102b69afbf387aull, 0xdd2cb145b8cc57e8ull,
    0x6a4f456d26c1002bull, 0x43aa740527ebfdddull, 0xd5177b6c958b5063ull, 0xa66266422ddeb977ull,
    0xac215909c9106f50ull, 0x4c0bd78cc5c1ccafull, 0xf05f02d27217623eull, 0x1f9517a9faa37e87ull,
    0x15c65de3c822d3f4ull, 0x9a469566769f8ca2ull, 0x8c102b69afbf387aull, 0xdd2cb145b8cc57e8ull,
    0x6a4f456d26c1002bull, 0x43aa740527ebfdddull, 0xd5177b6c958b5063ull, 0xa66266422ddeb977ull,
    0xac215909c9106f50ull, 0x4c0bd78cc5c1ccafull, 0xf05f02d27217623eull, 0x1f9517a9faa37e87ull,
    0x15c65de3c822d3f4ull, 0x9a469566769f8ca2ull, 0x8c102b69afbf387aull, 0xdd2cb145b8cc57e8ull,
    0x6a4f456d26c1002bull, 0x43aa740527ebfdddull, 0xd5177b6c958b5063ull, 0xa66266422ddeb977ull,
    0xac215909c9106f50ull, 0x4c0bd78cc5c1ccafull, 0xf05f02d27217623eull, 0x1f9517a9faa37e87ull,
    0x15c65de3c822d3f4ull, 0x9a469566769f8ca2ull, 0x8c102b69afbf387aull, 0xdd2cb145b8cc57e8ull,
    0x6a4f456d26c1002bull, 0x43aa740527ebfdddull, 0xd5177b6c958b5063ull, 0xa66266422ddeb977ull,
    0xac215909c9106f50ull, 0x4c0bd78cc5c1ccafull, 0xf05f02d27217623eull, 0x1f9517a9faa37e87ull,
    0x15c65de3c822d3f4ull, 0x9a469566769f8ca2ull, 0x8c102b69afbf387aull, 0xdd2cb145b8cc57e8ull,
    0x6a4f456d26c1002bull, 0x43aa740527ebfdddull, 0xd5177b6c958b5063ull, 0xa66266422ddeb977ull,
    0xac215909c9106f50ull, 0x4c0bd78cc5c1ccafull, 0xf05f02d27217623eull, 0x1f9517a9faa37e87ull,
    0x15c65de3c822d3f4ull, 0x9a469566769f8ca2ull, 0x8c102b69afbf387aull, 0xdd2cb145b8cc57e8ull,
    0x6a4f456d26c1002bull, 0x43aa740527ebfdddull, 0xd5177b6c958b5063ull, 0xa66266422ddeb977ull,
    0xac215909c9106f50ull, 0x4c0bd78cc5c1ccafull, 0xf05f02d27217623eull, 0x1f9517a9faa37e87ull,
    0x15c65de3c822d3f4ull, 0x9a469566769f8ca2ull, 0x8c102b69afbf387aull, 0xdd2cb145b8cc57e8ull,
    0x0831c7f68f4fa8d3ull, 0x83072309a32f3e0bull, 0x3a0e478de6353640ull, 0x2adfb5c49269ea67ull,
    0x548e9d2919b9ab3aull, 0x651a083202279ebcull, 0x65981621060cd7baull, 0x4c97748cd9313f05ull,
    0xa37bc483470963e3ull, 0xed1220078c5674b6ull, 0xa709b5e88a89df6cull, 0xc4477047123686cfull,
    0x731363f803bc2f17ull, 0x6d9ccc084e535f4eull, 0xe006385de63e5676ull, 0xbbc6c13a6fae1173ull,
    0x536e591fcf164d87ull, 0xb5568579231543faull, 0x2cde3738e1b9dc3eull, 0xe5abd77e5ddfcc18ull,
    0xa993dfbe0d91c238ull, 0x6a6c929aa2c9c02aull, 0xd38b46fb8cbf6dc1ull, 0x00a5da36db051028ull,
    0xaab4bbb37f55fb43ull, 0xcccfd0479cbaacf3ull, 0x7a784ae51a6deadfull, 0x1a8e19c5a573d747ull,
    0xf0d4463dbd6b5bf2ull, 0xe1b172dc0ec4cdd3ull, 0x5578e0dd74538ff3ull, 0xf4fda5a9bc25a50aull,
    0x0aa265d0fe10e8fcull, 0x64bb9754659f7aefull, 0xf8a55353f67fe2e7ull, 0x2afde40f8a260904ull,
    0x31889bc45d185400ull, 0xf8e34a0c2ce67779ull, 0x5bbbf2fe7ae8d70eull, 0xad86ebfb696b1e8cull,
    0x0f26c308c7971b8cull, 0xac1eb53e802dadbaull, 0xf706e491079ce2beull, 0xac7e6ebfa8e417c8ull,
    0xcdd125d2a4b13a57ull, 0x1abe82b7808447c6ull, 0x16fd115d7777f141ull, 0xdae9134b230a3197ull,
    0xde5b571cdc639229ull, 0x89f2bbf93487c372ull, 0x4b7f6a17c3608c0aull, 0x18c0214bf728caf5ull,
    0xdeef41fa60a4a69bull, 0x1523e209eedb541aull, 0x5af15c1d8daa0ae9ull, 0x7d80063742988ab7ull,
    0x90b03104695dc469ull, 0x6cddda5cdd529ad1ull, 0xd015e74006f8ac7eull, 0xb99583912bda1131ull,
    0x1ca92dda2c1a4f1eull, 0x0ee775fdc0e22e8aull, 0x405f83bc5eba952aull, 0xaf3ccafc64237bbeull,
    0xaf0d62f7a4f3dc37ull, 0x51ac352016516052ull, 0x0ff8bce332472951ull, 0x2e9a09345cf9dd63ull,
    0x92148c0c19e77868ull, 0xbad381382fd47f9dull, 0x92417e7690f7ab22ull, 0xf03e5fe96d69fa30ull,
    0x6c7fed20f3e61371ull, 0x7ed1e5aba63c37f2ull, 0x3ca65dc568e2ae03ull, 0x7e937dd75037090dull,
    0x9dcb5aa23fc18fceull, 0x9e9e15524e936127ull, 0xbaee5af569573911ull, 0x08f616d12d2b90c2ull,
    0x2055a7766581d713ull, 0xa22abb07f8dc01b5ull, 0x493005c4d6ceb39eull, 0x65a2a0b33619d73bull,
    0x6c7fed20f3e61371ull, 0x7ed1e5aba63c37f2ull, 0x3ca65dc568e2ae03ull, 0x7e937dd75037090dull,
    0x9dcb5aa23fc18fceull, 0x9e9e15524e936127ull, 0xbaee5af569573911ull, 0x08f616d12d2b90c2ull,
    0x2055a7766581d713ull, 0xa22abb07f8dc01b5ull, 0x493005c4d6ceb39eull, 0x65a2a0b33619d73bull,
    0x4d53518fd857f3d3ull, 0xc7e3af891dae55abull, 0x5f6ac998c9585a24ull, 0x1e696fac9639b647ull,
    0x26659e4f30ab8ad2ull, 0x9e8ecbc05ed0c700ull, 0xc223b3813c34fbeeull, 0x80ccd9e0b9032987ull,
    0xe2ad856101e1e3a8ull, 0x4a5cccc7c0365a3aull, 0x2fd928e945c39d52ull, 0x02cd9d32edd55f20ull,
    0x4c5e10289b38bbb7ull, 0x46cf135aa9ce3326ull, 0x594625afe3cbd61aull, 0x6743de7dfeda13abull,
    0xb236ca68f86bd2efull, 0xe885b3559a41cc1eull, 0x4303c7ccd5fd9ff8ull, 0x442392ab37865285ull,
    0x76dbfc32f26694cbull, 0x96ccc4ef95d26e38ull, 0x9465d48288133df0ull, 0xedc34c8a89462cd3ull,
    0xb4c65305c6afda83ull, 0xbb7b4f8e270bda1full, 0x979cfe08808a2f67ull, 0xea9fa42b0ec8604full,
    0x1d07e779968e7bf6ull, 0x48446afc33372fc3ull, 0xb9e5140ce06d895eull, 0xcfad9616859b74feull,
    0x2a2c7774c3cf28c0ull, 0xdebb7659efc282e6ull, 0x7bce1bb290fcdab7ull, 0xf90e77a8acb8f314ull,
    0x3f851b10c3ae9440ull, 0xea96a1cafbda579dull, 0x9d72349c260052beull, 0x292e296b2b192514ull,
    0x271dfd753959da48ull, 0x01a403b5cb1a907aull, 0x94ae134945968f34ull, 0x3658603f34449f58ull,
    0x299209964f1b02fdull, 0xc02b3d362a193fc4ull, 0x1c5364b7dc4cc2b1ull, 0x4c8c2fc3f95ebec1ull,
    0xb76b5014f3bf1199ull, 0xf7d0cb4449b77f42ull, 0x017c5b3542485842ull, 0x3902a0b3ea13e8adull,
    0x7f1c5e2d0b6e2a0bull, 0x50813baea57fa772ull, 0x1e5eb7a412746f40ull, 0xa2c2c733dd70fbd6ull,
    0x10c9ce6a50e98392ull, 0x78f1b6e67e662a1cull, 0x655460da6285831bull, 0xf2ef711c670cce26ull,
    0x5f5559cf362cabc6ull, 0x6319476b7406278aull, 0x60e2e17d2d1ca24aull, 0xa9672d31c70f5f16ull,
    0xedd231a7f665aebfull, 0x993824a9a886cc8aull, 0x86055de6d04c35c9ull, 0x26563f9bef332127ull,
    0x24fc1aa4a118e565ull, 0x93fd0d0b3d9402d1ull, 0x72cf35212a96ed96ull, 0xa0e82a9d30791bd1ull,
    0xfe18392d6910cb01ull, 0x917d9671367cccceull, 0xed42fd2f74989a97ull, 0x6cdae758400f711dull,
    0x087f4f13cbee3642ull, 0xd2e0478186d7e003ull, 0x8197b0c00ea27fb3ull, 0x94c73a44fc9817a7ull,
    0x0c99118bfcd6dd6eull, 0xe5aca86ecac8c257ull, 0x7011e000dc3014cfull, 0xc0c48714f83d6c5eull,
    0xfe18392d6910cb01ull, 0x917d9671367cccceull, 0xed42fd2f74989a97ull, 0x6cdae758400f711dull,
    0x087f4f13cbee3642ull, 0xd2e0478186d7e003ull, 0x8197b0c00ea27fb3ull, 0x94c73a44fc9817a7ull,
    0x0c99118bfcd6dd6eull, 0xe5aca86ecac8c257ull, 0x7011e000dc3014cfull, 0xc0c48714f83d6c5eull
};
const std::uint64_t kGoldenChords[] = {
    0x68205373618dbf50ull, 0x280752323234043full, 0x64c16c034f077e8eull, 0x8681f6156d0b9a35ull,
    0xb284497e0c544dc7ull, 0x50128e32eaa589aaull, 0x68205373618dbf50ull, 0x280752323234043full,
    0x64c16c034f077e8eull, 0x8681f6156d0b9a35ull, 0xb284497e0c544dc7ull, 0x50128e32eaa589aaull,
    0x68205373618dbf50ull, 0x280752323234043full, 0x64c16c034f077e8eull, 0x8681f6156d0b9a35ull,
    0xb284497e0c544dc7ull, 0x50128e32eaa589aaull, 0x68205373618dbf50ull, 0x280752323234043full,
    0x64c16c034f077e8eull, 0x8681f6156d0b9a35ull, 0xb284497e0c544dc7ull, 0x50128e32eaa589aaull,
    0x68205373618dbf50ull, 0x280752323234043full, 0x64c16c034f077e8eull, 0x8681f6156d0b9a35ull,
    0xb284497e0c544dc7ull, 0x50128e32eaa589aaull, 0x68205373618dbf50ull, 0x280752323234043full,
    0x64c16c034f077e8eull, 0x8681f6156d0b9a35ull, 0xb284497e0c544dc7ull, 0x50128e32eaa589aaull,
    0x68205373618dbf50ull, 0x280752323234043full, 0x64c16c034f077e8eull, 0x8681f6156d0b9a35ull,
    0xb284497e0c544dc7ull, 0x50128e32eaa589aaull, 0x68205373618dbf50ull, 0x280752323234043full,
    0x64c16c034f077e8eull, 0x8681f6156d0b9a35ull, 0xb284497e0c544dc7ull, 0x50128e32eaa589aaull,
    0x858c39c72c948a87ull, 0x7373a63b3f39ee82ull, 0x63195104119de64bull, 0xd04fd79fcce16676ull,
    0x65c23d62ee915c0full, 0xac18420fb8d734ffull, 0x858c39c72c948a87ull, 0x7373a63b3f39ee82ull,
    0x63195104119de64bull, 0xd04fd79fcce16676ull, 0x65c23d62ee915c0full, 0xac18420fb8d734ffull,
    0x858c39c72c948a87ull, 0x7373a63b3f39ee82ull, 0x63195104119de64bull, 0xd04fd79fcce16676ull,
    0x65c23d62ee915c0full, 0xac18420fb8d734ffull, 0x858c39c72c948a87ull, 0x7373a63b3f39ee82ull,
    0x63195104119de64bull, 0xd04fd79fcce16676ull, 0x65c23d62ee915c0full, 0xac18420fb8d734ffull,
    0x858c39c72c948a87ull, 0x7373a63b3f39ee82ull, 0x63195104119de64bull, 0xd04fd79fcce16676ull,
    0x65c23d62ee915c0full, 0xac18420fb8d734ffull, 0x858c39c72c948a87ull, 0x7373a63b3f39ee82ull,
    0x63195104119de64bull, 0xd04fd79fcce16676ull, 0x65c23d62ee915c0full, 0xac18420fb8d734ffull,
    0x858c39c72c948a87ull, 0x7373a63b3f39ee82ull, 0x63195104119de64bull, 0xd04fd79fcce16676ull,
    0x65c23d62ee915c0full, 0xac18420fb8d734ffull, 0x858c39c72c948a87ull, 0x7373a63b3f39ee82ull,
    0x63195104119de64bull, 0xd04fd79fcce16676ull, 0x65c23d62ee915c0full, 0xac18420fb8d734ffull,
    0x9c2dd58e6d121000ull, 0x91122294ac10d7b0ull, 0x0425be66cec321ebull, 0xfef786f7d72fa59dull,
    0x13e6f3b5abced015ull, 0xa3b13ecc4696a678ull, 0x015e410d0a85bdc8ull, 0x7ea3189bc5f91e27ull,
    0x470e6e4c62f34aecull, 0xa251c6906c5c6975ull, 0xa6afd3bc38b0d26eull, 0x50f082e54b6037adull,
    0xe23bc63cb66ac56cull, 0x74398835f2b5ec47ull, 0xbffa9d236fc47266ull, 0x70904e761d502cdfull,
    0x45a1ef4fa94483f8ull, 0x32236f994af6ac26ull, 0xe4509e637706866bull, 0x5d98aca9853fb145ull,
    0x6bec2028a0a596c9ull, 0x888ff261a5236185ull, 0x49406b015aa2fcf3ull, 0xd3dc703adc8b851bull,
    0xc358347a8b1ea2baull, 0xe7ddefb9eb4c32b5ull, 0x97566b755768d822ull, 0x1cb41f585bd955d0ull,
    0xd0f983c57ce3f59bull, 0x909e460f64e313a8ull, 0xd8631ddc0db169a5ull, 0x5dd55cc62a21b86eull,
    0x9aede7846b16eb5aull, 0xccad82884c1a25ddull, 0x70c3047374e167b0ull, 0x607171fea04f09eeull,
    0x337fe4e1b3b2618eull, 0x09e39177b6f623afull, 0x321f7b0214f8af72ull, 0x244bac45ca716526ull,
    0x493ca4f7611f91a2ull, 0x0b1a1197f3c0b757ull, 0x337fe4e1b3b2618eull, 0x09e39177b6f623afull,
    0x321f7b0214f8af72ull, 0x244bac45ca716526ull, 0x493ca4f7611f91a2ull, 0x0b1a1197f3c0b757ull,
    0xc3450fe8d535f270ull, 0x763bce9f7b1eaf6cull, 0x94fbcb5ea7fb3508ull, 0xeb1a2fc5718ede87ull,
    0x779fdf3f0a0def49ull, 0x6396faaf9b99fc6eull, 0xc3450fe8d535f270ull, 0x763bce9f7b1eaf6cull,
    0x94fbcb5ea7fb3508ull, 0xeb1a2fc5718ede87ull, 0x779fdf3f0a0def49ull, 0x6396faaf9b99fc6eull,
    0xc3450fe8d535f270ull, 0x763bce9f7b1eaf6cull, 0x94fbcb5ea7fb3508ull, 0xeb1a2fc5718ede87ull,
    0x779fdf3f0a0def49ull, 0x6396faaf9b99fc6eull, 0x8490faf9bcbe22b3ull, 0xfe0d5f41d338db70ull,
    0xbb2dd0715143ca4dull, 0xa0405a8a7987b401ull, 0x7577d583759ad0a5ull, 0x38cb59a2aa683015ull,
    0xc3450fe8d535f270ull, 0x763bce9f7b1eaf6cull, 0x94fbcb5ea7fb3508ull, 0xeb1a2fc5718ede87ull,
    0x779fdf3f0a0def49ull, 0x6396faaf9b99fc6eull, 0xc3450fe8d535f270ull, 0x763bce9f7b1eaf6cull,
    0x94fbcb5ea7fb3508ull, 0xeb1a2fc5718ede87ull, 0x779fdf3f0a0def49ull, 0x6396faaf9b99fc6eull,
    0xc3450fe8d535f270ull, 0x763bce9f7b1eaf6cull, 0x94fbcb5ea7fb3508ull, 0xeb1a2fc5718ede87ull,
    0x779fdf3f0a0def49ull, 0x6396faaf9b99fc6eull, 0xc3450fe8d535f270ull, 0x763bce9f7b1eaf6cull,
    0x94fbcb5ea7fb3508ull, 0xeb1a2fc5718ede87ull, 0x779fdf3f0a0def49ull, 0x6396faaf9b99fc6eull,
    0xa43146cf104d2a1full, 0xb08c81802f38717eull, 0xd5665581c1e13a86ull, 0x2e589fb2bf2da659ull,
    0x6e357db5f9d7317dull, 0x0c708649a3dfdeccull, 0xb01f05624ef52a6full, 0x22b8be0731c9115full,
    0x1be8a248115a4ffcull, 0x52702fbb6a20943cull, 0xb299c764e89f073dull, 0x93220b67af7d56d2ull,
    0x57db5df6baf1b514ull, 0x8b5529ac5071d3f5ull, 0x1b6ff5ff1b2d4b5dull, 0x69dfbf6aaf7c7342ull,
    0x5426228abdfe7207ull, 0x6951d40f3184685full, 0x6fa9e3142f917383ull, 0x3ae3574ee01f46cfull,
    0x2072f47be20797a8ull, 0x149c022dd2849e84ull, 0x56ecbed8cb6751c8ull, 0x2a40480ca90cf94dull,
    0xf38ed820c4b0a8a5ull, 0xa082ebe463b06a89ull, 0x89360c2e50f1513cull, 0xb8060ae61b7124bbull,
    0xc9be191c9137c3ecull, 0x11f8b0a9f0c164f6ull, 0x06f92c9336778a2aull, 0x90a23c3612857f45ull,
    0xdef089329f1f6accull, 0x6ff2ed9184100cd5ull, 0xd476c752a0b2a1c1ull, 0x431c5ed53869e8b4ull,
    0xe0bfec4e29940e8aull, 0x5a11250028600448ull, 0x43a3f48f2ee6353full, 0x25ccadbfcefaff37ull,
    0xb97a3d1b3a49e266ull, 0xba58eb9650a87dc3ull, 0xe0bfec4e29940e8aull, 0x5a11250028600448ull,
    0x43a3f48f2ee6353full, 0x25ccadbfcefaff37ull, 0xb97a3d1b3a49e266ull, 0xba58eb9650a87dc3ull,
    0xcf2314a11bc48780ull, 0x47281ceb8a034e91ull, 0x4454ccd2432ea528ull, 0xf4c4117f2ccf92bfull,
    0xf56c7a625fb6fe19ull, 0x9f7dd9bd910cf37cull, 0x1a851c6c615a110cull, 0x6a81409f816547dcull,
    0x9e5d875005371f7aull, 0x909cc6e61b038862ull, 0x4dcbabdf91b1641eull, 0x8bcbd7103bf949c1ull,
    0xb723edf0a474444full, 0xd0c649a0a94042aaull, 0x22c00a9433be9c89ull, 0x55376f86584d4a75ull,
    0xef3d5f93c137ade6ull, 0x32b5769eb358dfffull, 0x72e60c77ba0b41d8ull, 0xb4ae0f487cd82a04ull,
    0x657dbc12c66f9380ull, 0x0a2cd10d177bc678ull, 0x554162cad69ccc4eull, 0x991b71fe2e20c72bull,
    0xa257a4494a0f7976ull, 0x15a86b779b104539ull, 0x85a21fcfd78bd0a7ull, 0x7328d33ce63ce80cull,
    0x531ab9ebfc6cd4b6ull, 0x0afe26384577a378ull, 0x1a1eb33ab36344c5ull, 0x04ed25fc67522d35ull,
    0x65524e00e26e1777ull, 0xec69ad8766e2baebull, 0x7011d25e303d47b7ull, 0x9b934cb6cb762a3dull,
    0x7d70d89cf6639036ull, 0x79315f25c5f2b82aull, 0x7183d10746226786ull, 0xfd219aa6e500c11dull,
    0x15a21cda00932ee9ull, 0xb6563611d38a77feull, 0x7d70d89cf6639036ull, 0x79315f25c5f2b82aull,
    0x7183d10746226786ull, 0xfd219aa6e500c11dull, 0x15a21cda00932ee9ull, 0xb6563611d38a77feull,
    0xc6d9baccd37b4133ull, 0x4671756afd6e78aaull, 0x724fa642d966f47dull, 0x8289d19e3f264640ull,
    0x2acc988e4fe56d94ull, 0xceba77facdd9f5b8ull, 0x0ae36feabbf18e07ull, 0x035a95d49bee2b8full,
    0x37711b8effefe6e3ull, 0xad86be0cea16b5d0ull, 0xec6555b387f7dab3ull, 0x8a6fe04ab3818180ull,
    0xf0db55695ddcb2fcull, 0xb6632d13f0be77e0ull, 0x585efdc3275ab171ull, 0x746b0ef423f5b035ull,
    0x3481e9d61540945cull, 0xdade966af1b7cc42ull, 0xb964a9f292accaf3ull, 0x8c287373b18207b6ull,
    0xa9447bdfb2edbb78ull, 0xa7ee7b6dd56338bcull, 0x17fabbf70b362ca7ull, 0x751ba6c08542a2bfull,
    0xc0522bcdbb1dd6d6ull, 0x36c968e6abb7a7c1ull, 0xd29c8496b16b794eull, 0xaf2b62c489ab74a7ull,
    0x53ee6d07bd97b51full, 0xb9a105408a639917ull, 0x8297dff0a325b989ull, 0xe85f4a857c2c48a1ull,
    0x23c2543c1a9642aaull, 0x43af2772064e6f93ull, 0x6234a83049cc78a4ull, 0xe134cdfb2225a27dull,
    0x036b6d54863364c9ull, 0x3d6e89e418bed317ull, 0x6971bd5e6201ed4aull, 0x4fca843bac24aeceull,
    0xb5190fd562e9c6c4ull, 0x74dda9ce9e309640ull, 0x036b6d54863364c9ull, 0x3d6e89e418bed317ull,
    0x6971bd5e6201ed4aull, 0x4fca843bac24aeceull, 0xb5190fd562e9c6c4ull, 0x74dda9ce9e309640ull,
    0x97e1e653fa39b9c4ull, 0x4310ba23ed3cdc91ull, 0x022e8bff6c50170aull, 0xb5131d1ed37ff5c1ull,
    0x11a398e175421d6dull, 0xd39cbcd2baade6fdull, 0x5f302fa8c6a5a100ull, 0x071753c256310556ull,
    0x5f55c05324fec88bull, 0x871febe991d9af1dull, 0xc9d1129e1fb71b0dull, 0xc62ec7aa79bbe574ull,
    0x97e1e653fa39b9c4ull, 0x4310ba23ed3cdc91ull, 0x022e8bff6c50170aull, 0xb5131d1ed37ff5c1ull,
    0x11a398e175421d6dull, 0xd39cbcd2baade6fdull, 0x5f302fa8c6a5a100ull, 0x071753c256310556ull,
    0x5f55c05324fec88bull, 0x871febe991d9af1dull, 0xc9d1129e1fb71b0dull, 0xc62ec7aa79bbe574ull,
    0x591b2b0cc1202ee0ull, 0xf9891336f783f0eaull, 0x07cd3b4c91e85513ull, 0xf6677d27fdbfefc8ull,
    0xd7a99167ffa5925bull, 0xb1a4ab98017413e4ull, 0x4fefeba2c57bef30ull, 0xe0189507f9208dbfull,
    0x92d10cf31a543bd9ull, 0xcf3f081880cf1eeaull, 0x8a90734a2ae478f6ull, 0x927b8442a31a6d7eull,
    0xbaa910fa8abc4464ull, 0x47dd9c165f1a4909ull, 0x3c554f4514dee03aull, 0x2acd4d2c1bc7c518ull,
    0xd7cfdf40ec3bbe51ull, 0xae52d208b9c5083aull, 0xbaa910fa8abc4464ull, 0x47dd9c165f1a4909ull,
    0x3c554f4514dee03aull, 0x2acd4d2c1bc7c518ull, 0xd7cfdf40ec3bbe51ull, 0xae52d208b9c5083aull
};

int compareTable (const char* name, const std::vector<std::uint64_t>& got, const std::uint64_t* want, size_t wantSize)
{
    int mismatches = 0;
    if (got.size() != wantSize)
    {
        std::cout << "    " << name << ": " << got.size() << " hashes computed, " << wantSize << " golden\n";
        return 1;
    }
    for (size_t i = 0; i < got.size(); ++i)
        if (got[i] != want[i])
        {
            if (++mismatches <= 10)
                std::cout << "    " << name << "[" << i << "] got " << hex (got[i]) << " want " << hex (want[i]) << "\n";
        }
    return mismatches;
}
} // namespace

TEST_CASE ("golden: classic bass modes render bit-identically (480 configurations)")
{
    const auto got = computeBassTable();
    CHECK_EQ (got.size(), (size_t) 480);
    CHECK_EQ (compareTable ("bass", got, kGoldenBass, sizeof (kGoldenBass) / sizeof (kGoldenBass[0])), 0);
}

TEST_CASE ("golden: chord rhythm patterns render bit-identically (384 configurations)")
{
    const auto got = computeChordTable();
    CHECK_EQ (got.size(), (size_t) 384);
    CHECK_EQ (compareTable ("chords", got, kGoldenChords, sizeof (kGoldenChords) / sizeof (kGoldenChords[0])), 0);
}

TEST_CASE ("golden: the matrix is not degenerate")
{
    // Different settings must give different hashes often enough that the tables actually protect something.
    const auto bass = computeBassTable();
    int distinct = 0;
    for (size_t i = 0; i < bass.size(); ++i)
    {
        bool seen = false;
        for (size_t j = 0; j < i && ! seen; ++j)
            seen = bass[j] == bass[i];
        distinct += seen ? 0 : 1;
    }
    CHECK (distinct > 200);
}
