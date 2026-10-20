#include "core/Pattern.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

//==============================================================================
// Pattern rendering rules
//
// Every slot is rendered on its own: steps restart at each slot start and no event extends past the slot end.
//  - Step grid: rate steps k = 0, 1, 2 ... inside the slot. Swing delays every second (odd) step by
//    swing * step / 3; each step's "span" runs to the next step's (swung) start or the slot end, and a note lasts
//    gate * span, so swung notes never overlap the next step. Triplet rate (1/8T) is already a shuffle and is not
//    swung (swinging every second triplet would push every other downbeat late).
//  - Block/Strum: release at slotLength - (1-gate)*min(slotLength,1); strum onsets stagger by 0.03 beats, shrunk to
//    held / (2 * (notes - 1)) when the held length is shorter than that, so every note sounds for at least half of it.
//  - Pulse: whole chord on every step. Arp Up / Down / Up-Down: one note per step (Up-Down: 1 2 3 4 3 2 | 1 2 ...).
//  - Offbeat: whole chord on the "&" of every beat, gate * half a beat long. The "&" is swung like an odd step when
//    it is one (1/8 rate); at other rates it is not on an odd step and stays straight.
//  - Broken: the grid runs at min(rate, 1/8) so a 1/4 rate still has room inside the beat; on the first step of
//    every beat the lowest note, on the following steps of the beat the remaining notes together.
//  - Humanize: deterministic hash of (slot, step, voice, channel); timing +-0.015 * humanize beats (clamped to the
//    slot), velocity +-0.12 * humanize. Velocities always stay in [1/127, 1].
//  - Bass (channel 2): explicit inversion bass, otherwise root. Octave choice in [28,55]: first by separation from
//    the lowest chord note (at least 5 below > below > unison > above), then by |movement from the previous bass
//    note (initially 40)| + 0.5 * |note - 41|, so lines stay voice-led but are pulled back to the middle of the range
//    instead of drifting down (C G Am F -> C2 G2 A2 F2); ties go lower, above the chord the lowest note wins. When no
//    note in [28,55] is below the chord, C1..D#1 (24..27) may be used.
//    Root uses the held release above; Pulse uses rate steps. Root+5th alternates the slot's bass note with the
//    chord's fifth (b5/#5 of dim/aug), or the chord root for slash chords, chosen by the same rule from that note.
//    Octaves alternates with an upper/lower octave in range; the last played bass note carries into the next slot.
//  - Safety: a note never overlaps another instance of the same note on the same channel (an earlier one is
//    shortened), so hosts never see a second note-on before the note-off.
// Output is sorted by (startBeat, channel, note).
//==============================================================================

namespace cb
{
const char* rhythmPatternName (RhythmPattern p)
{
    static const char* names[] = { "Block", "Strum", "Pulse", "Offbeat", "Arp Up", "Arp Down", "Arp Up/Down", "Broken" };
    return names[std::clamp ((int) p, 0, numRhythmPatterns - 1)];
}

const char* bassModeName (BassMode m)
{
    static const char* names[] = { "Off", "Root", "Root + 5th", "Octaves", "Pulse",
                                   "Offbeat 8ths", "Rolling 16ths", "Rolling, kick gap", "Offbeat pairs", "Gallop",
                                   "Octave bounce", "Driving 8ths", "Off-kick", "Tresillo 3-3-2", "Dotted quarters" };
    static_assert (sizeof (names) / sizeof (names[0]) == (size_t) numBassModes, "one name per BassMode");
    return names[std::clamp ((int) m, 0, numBassModes - 1)];
}

//==============================================================================
// Phase 2 bass pattern data (final-spec 4.2; written by WP0, the renderer is WP2's).
bool isBassPattern (BassMode m)
{
    return (int) m >= (int) BassMode::Offbeat8 && (int) m < numBassModes;
}

const std::vector<BassPatternStep>& bassPatternSteps (BassMode m)
{
    using S = BassPatternStep;
    constexpr auto R = BassRole::Root;
    constexpr auto O = BassRole::Octave;

    static const std::vector<BassPatternStep> none;
    static const std::vector<BassPatternStep> offbeat8 { S { 2, 2.0f, R }, S { 6, 2.0f, R }, S { 10, 2.0f, R },
                                                         S { 14, 2.0f, R } };
    static const std::vector<BassPatternStep> rolling16 = []
    {
        std::vector<BassPatternStep> v;
        for (int i = 0; i < 16; ++i)
            v.push_back ({ i, 0.96f, BassRole::Root });
        return v;
    }();
    static const std::vector<BassPatternStep> rollingKickGap = []
    {
        std::vector<BassPatternStep> v;
        for (int b = 0; b < 4; ++b)
        {
            v.push_back ({ 4 * b + 1, 0.96f, BassRole::Root });
            v.push_back ({ 4 * b + 2, 0.96f, BassRole::Octave });
            v.push_back ({ 4 * b + 3, 0.96f, BassRole::Root });
        }
        return v;
    }();
    static const std::vector<BassPatternStep> offbeatPairs { S { 2, 0.96f, R }, S { 3, 0.96f, R }, S { 6, 0.96f, R },
                                                             S { 7, 0.96f, R }, S { 10, 0.96f, R }, S { 11, 0.96f, R },
                                                             S { 14, 0.96f, R }, S { 15, 0.96f, R } };
    static const std::vector<BassPatternStep> gallop { S { 0, 1.0f, R }, S { 2, 0.8f, R }, S { 3, 0.96f, R },
                                                       S { 5, 0.96f, R }, S { 6, 1.6f, R }, S { 8, 1.0f, R },
                                                       S { 10, 0.8f, R }, S { 11, 0.96f, R }, S { 13, 0.96f, R },
                                                       S { 14, 1.6f, R } };
    static const std::vector<BassPatternStep> octaveBounce8 { S { 0, 1.6f, R }, S { 2, 1.6f, O }, S { 4, 1.6f, R },
                                                              S { 6, 1.6f, O }, S { 8, 1.6f, R }, S { 10, 1.6f, O },
                                                              S { 12, 1.6f, R }, S { 14, 1.6f, O } };
    static const std::vector<BassPatternStep> driving8 { S { 0, 1.6f, R }, S { 2, 1.6f, R }, S { 4, 1.6f, R },
                                                         S { 6, 1.6f, R }, S { 8, 1.6f, R }, S { 10, 1.6f, R },
                                                         S { 12, 1.6f, R }, S { 14, 1.6f, R } };
    static const std::vector<BassPatternStep> offKick { S { 1, 0.96f, R }, S { 3, 1.0f, R }, S { 6, 2.0f, R },
                                                        S { 9, 0.96f, R }, S { 11, 1.0f, R }, S { 14, 2.0f, R } };
    static const std::vector<BassPatternStep> tresillo { S { 0, 2.0f, R }, S { 3, 2.0f, R }, S { 6, 2.0f, R },
                                                         S { 8, 2.0f, R }, S { 11, 2.0f, R }, S { 14, 2.0f, R } };
    static const std::vector<BassPatternStep> dottedQuarter { S { 0, 2.0f, R }, S { 6, 2.0f, R }, S { 12, 2.0f, R } };

    switch (m)
    {
        case BassMode::Offbeat8:       return offbeat8;
        case BassMode::Rolling16:      return rolling16;
        case BassMode::RollingKickGap: return rollingKickGap;
        case BassMode::OffbeatPairs:   return offbeatPairs;
        case BassMode::Gallop:         return gallop;
        case BassMode::OctaveBounce8:  return octaveBounce8;
        case BassMode::Driving8:       return driving8;
        case BassMode::OffKick:        return offKick;
        case BassMode::Tresillo:       return tresillo;
        case BassMode::DottedQuarter:  return dottedQuarter;
        case BassMode::Off:
        case BassMode::Root:
        case BassMode::RootFifth:
        case BassMode::Octaves:
        case BassMode::Pulse:
        case BassMode::NumModes:
        default:                       break;
    }
    return none;
}

std::string bassPatternGrid (BassMode m)
{
    const auto& steps = bassPatternSteps (m);
    if (steps.empty())
        return {};

    std::string grid (16, '-');
    for (const auto& s : steps)
        if (s.step >= 0 && s.step < 16)
            grid[(size_t) s.step] = s.role == BassRole::Octave ? 'o' : 'x';
    return grid;
}

bool bassPatternAvoidsKick (BassMode m)
{
    return m == BassMode::Offbeat8 || m == BassMode::RollingKickGap || m == BassMode::OffbeatPairs
        || m == BassMode::OffKick;
}

const char* bassModeFamily (BassMode m)
{
    switch (m)
    {
        case BassMode::Root:
        case BassMode::RootFifth:      return "Held";
        case BassMode::Octaves:
        case BassMode::Pulse:          return "Follows Rate";
        case BassMode::Offbeat8:
        case BassMode::Rolling16:
        case BassMode::RollingKickGap:
        case BassMode::OffbeatPairs:
        case BassMode::Gallop:         return "Trance";
        case BassMode::OctaveBounce8:
        case BassMode::Driving8:
        case BassMode::OffKick:
        case BassMode::Tresillo:
        case BassMode::DottedQuarter:  return "Groove";
        case BassMode::Off:
        case BassMode::NumModes:
        default:                       break;
    }
    return "";
}

std::vector<BassMode> bassModeBrowseOrder()
{
    std::vector<BassMode> order;
    for (int i = (int) BassMode::Root; i < numBassModes; ++i)
        order.push_back ((BassMode) i);
    return order;
}

int slotBassPitchClass (const ProgressionSlot& slot, const std::vector<int>& voicing)
{
    if (slot.inversion < 0)
        return pc (slot.chord.root);

    int lowest = -1;
    for (int n : voicing)
        if (n >= 0 && n <= 127 && (lowest < 0 || n < lowest))
            lowest = n;

    if (lowest >= 0)
        return pc (lowest);

    const auto chordPcs = slot.chord.pitchClasses();
    if (chordPcs.empty())
        return pc (slot.chord.root);
    return chordPcs[(size_t) std::min (slot.inversion, (int) chordPcs.size() - 1)];
}

double grooveGridBeats (const RenderSettings& s)
{
    switch (s.rate)
    {
        case RateDivision::Quarter:       return 0.5;
        case RateDivision::Eighth:        return 0.5;
        case RateDivision::Sixteenth:     return 0.25;
        case RateDivision::EighthTriplet: return 0.0;
        case RateDivision::NumRates:      break;
    }
    return 0.5;
}

double swingWarp (double beat, double gridBeats, double swing)
{
    swing = std::clamp (swing, 0.0, 1.0);
    if (! (gridBeats > 0.0) || ! (swing > 0.0) || ! std::isfinite (beat) || ! std::isfinite (gridBeats))
        return beat;

    const double window = 2.0 * gridBeats;
    const double k = std::floor (beat / window);
    const double windowStart = k * window;
    const double local = beat - windowStart;
    const double mid = gridBeats + swing * gridBeats / 3.0;

    if (local <= 0.0)
        return windowStart;
    if (local < gridBeats)
        return windowStart + local * mid / gridBeats;
    return windowStart + mid + (local - gridBeats) * (window - mid) / gridBeats;
}

const char* rateDivisionName (RateDivision r)
{
    static const char* names[] = { "1/4", "1/8", "1/16", "1/8T" };
    return names[std::clamp ((int) r, 0, numRateDivisions - 1)];
}

double rateInBeats (RateDivision r)
{
    switch (r)
    {
        case RateDivision::Quarter:       return 1.0;
        case RateDivision::Eighth:        return 0.5;
        case RateDivision::Sixteenth:     return 0.25;
        case RateDivision::EighthTriplet: return 1.0 / 3.0;
        case RateDivision::NumRates:      break;
    }
    return 0.5;
}

namespace
{
constexpr double kEps = 1.0e-9;
constexpr double kStrumOffset = 0.03;
constexpr double kHumanizeTiming = 0.015;
constexpr float kHumanizeVelocity = 0.12f;
constexpr float kMinVelocity = 1.0f / 127.0f;
constexpr double kMinLength = 1.0e-4;
constexpr int kBassLow = 28;
constexpr int kBassHigh = 55;
constexpr int kBassCentre = 41;   // F2: the bass line's centre of gravity
constexpr int kBassFloor = 24;    // C1: only when no note in [kBassLow, kBassHigh] fits below the chord
constexpr int kChordChannel = 1;
constexpr int kBassChannel = 2;

uint32_t mix32 (uint32_t h)
{
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

// Deterministic value in [-1, 1].
double jitter (int slot, int step, int voice, int channel, int salt)
{
    uint32_t h = 0x9e3779b9u;
    h = mix32 (h ^ ((uint32_t) slot * 0x27d4eb2du));
    h = mix32 (h ^ ((uint32_t) step * 0x165667b1u));
    h = mix32 (h ^ ((uint32_t) voice * 0xd3a2646cu));
    h = mix32 (h ^ ((uint32_t) channel * 0xfd7046c5u));
    h = mix32 (h ^ ((uint32_t) salt * 0xb55a4f09u));
    return (double) h / 4294967295.0 * 2.0 - 1.0;
}

struct Step
{
    double start;   // relative to the slot start
    double span;    // to the next step's start (or the slot end)
    int index;
};

std::vector<Step> stepGrid (double slotLength, double step, double swingDelay)
{
    std::vector<Step> steps;
    if (step <= 0.0 || slotLength <= 0.0)
        return steps;

    const int count = std::max (1, (int) std::ceil (slotLength / step - 1.0e-6));
    auto startOf = [&] (int k) { return k * step + ((k & 1) != 0 ? swingDelay : 0.0); };

    for (int k = 0; k < count; ++k)
    {
        const double s = startOf (k);
        const double next = k + 1 < count ? std::min (startOf (k + 1), slotLength) : slotLength;
        if (s < slotLength - kEps)
            steps.push_back ({ s, next - s, k });
    }

    return steps;
}

struct SlotRenderer
{
    std::vector<NoteEvent>& out;
    int slotIndex = 0;
    double slotStart = 0.0;
    double slotEnd = 0.0;
    float velocity = 0.75f;
    double humanize = 0.0;

    void emit (int note, double relStart, double length, int channel, int step, int voice) const
    {
        double start = slotStart + relStart;
        float vel = velocity;

        if (humanize > 0.0)
        {
            start += jitter (slotIndex, step, voice, channel, 0) * kHumanizeTiming * humanize;
            vel += (float) (jitter (slotIndex, step, voice, channel, 1) * (double) kHumanizeVelocity * humanize);
        }

        start = std::max (start, std::max (slotStart, 0.0));
        const double end = std::min (start + length, slotEnd);
        if (end - start < kMinLength)
            return;

        NoteEvent e;
        e.note = std::clamp (note, 0, 127);
        e.velocity = std::clamp (vel, kMinVelocity, 1.0f);
        e.startBeat = start;
        e.lengthBeats = end - start;
        e.channel = channel;
        out.push_back (e);
    }
};

// Separation from the chord first (tier 0: at least 5 below its lowest note, 1: below, 2: unison, 3: above), then
// cost = |movement| + 0.5 * |distance from kBassCentre| (kept in integers as twice that); ties choose the lower note.
// Above the chord the lowest candidate wins. A unison / above-chord result falls back to C1..D#1 when that is below.
int bassRootNote (int bassPc, int lowestVoicingNote, int previous)
{
    auto tier = [lowestVoicingNote] (int n)
    {
        return n <= lowestVoicingNote - 5 ? 0 : (n < lowestVoicingNote ? 1 : (n == lowestVoicingNote ? 2 : 3));
    };
    auto cost = [previous] (int n) { return 2 * std::abs (n - previous) + std::abs (n - kBassCentre); };

    int best = -1;
    for (int n = kBassLow; n <= kBassHigh; ++n)
    {
        if (pc (n) != bassPc)
            continue;

        if (best < 0 || tier (n) < tier (best) || (tier (n) == tier (best) && tier (n) < 3 && cost (n) < cost (best)))
            best = n;   // ascending scan: equal costs (and the above-chord tier) keep the lower note
    }

    if (best >= 0 && tier (best) >= 2)
        for (int n = kBassFloor; n < kBassLow; ++n)
            if (pc (n) == bassPc && n < lowestVoicingNote)
                return n;

    return best;
}

double heldLength (double slotLength, double gate)
{
    return slotLength - (1.0 - gate) * std::min (slotLength, 1.0);
}

void renderChords (const SlotRenderer& r, const std::vector<int>& notes, const RenderSettings& s, double slotLength,
                   double gate, double swing)
{
    const int n = (int) notes.size();
    if (n == 0)
        return;

    const double rate = rateInBeats (s.rate);
    const bool triplet = s.rate == RateDivision::EighthTriplet;
    const double swingDelay = triplet ? 0.0 : swing * rate / 3.0;

    switch (s.pattern)
    {
        case RhythmPattern::Strum:
        {
            // The stagger shrinks to fit short held lengths (1-beat slots, low gate): every note keeps at least half.
            const double held = heldLength (slotLength, gate);
            const double offset = n > 1 ? std::min (kStrumOffset, 0.5 * held / (n - 1)) : 0.0;
            for (int i = 0; i < n; ++i)
                r.emit (notes[(size_t) i], offset * i, held - offset * i, kChordChannel, 0, i);
            break;
        }

        case RhythmPattern::Pulse:
            for (auto& st : stepGrid (slotLength, rate, swingDelay))
                for (int i = 0; i < n; ++i)
                    r.emit (notes[(size_t) i], st.start, gate * st.span, kChordChannel, st.index, i);
            break;

        case RhythmPattern::Offbeat:
        {
            // The "&" is an odd step only on the 1/8 grid, so only there does swing move it.
            const double delay = s.rate == RateDivision::Eighth ? swing * 0.5 / 3.0 : 0.0;
            const int beats = (int) std::ceil (slotLength - 1.0e-6);
            for (int b = 0; b < beats; ++b)
                for (int i = 0; i < n; ++i)
                    r.emit (notes[(size_t) i], b + 0.5 + delay, gate * 0.5, kChordChannel, b, i);
            break;
        }

        case RhythmPattern::ArpUp:
        case RhythmPattern::ArpDown:
        case RhythmPattern::ArpUpDown:
            for (auto& st : stepGrid (slotLength, rate, swingDelay))
            {
                int idx = st.index % n;
                if (s.pattern == RhythmPattern::ArpDown)
                {
                    idx = n - 1 - idx;
                }
                else if (s.pattern == RhythmPattern::ArpUpDown)
                {
                    const int period = n > 1 ? 2 * n - 2 : 1;
                    const int p = st.index % period;
                    idx = p < n ? p : period - p;
                }
                r.emit (notes[(size_t) idx], st.start, gate * st.span, kChordChannel, st.index, idx);
            }
            break;

        case RhythmPattern::Broken:
        {
            const double step = std::min (rate, 0.5);
            const int perBeat = std::max (1, (int) std::lround (1.0 / step));
            const double delay = triplet ? 0.0 : swing * step / 3.0;

            for (auto& st : stepGrid (slotLength, step, delay))
            {
                if (st.index % perBeat == 0)
                    r.emit (notes[0], st.start, gate * st.span, kChordChannel, st.index, 0);
                else
                    for (int i = 1; i < n; ++i)
                        r.emit (notes[(size_t) i], st.start, gate * st.span, kChordChannel, st.index, i);
            }
            break;
        }

        case RhythmPattern::Block:
        case RhythmPattern::NumPatterns:
        default:
            for (int i = 0; i < n; ++i)
                r.emit (notes[(size_t) i], 0.0, heldLength (slotLength, gate), kChordChannel, 0, i);
            break;
    }
}

void renderBass (const SlotRenderer& r, int rootPc, int bassPc, int fifthPc, int lowestVoicingNote, int& previousBass,
                 const RenderSettings& s, double slotLength, double gate, double swing)
{
    if (s.bass == BassMode::Off || (int) s.bass < 0 || (int) s.bass >= numBassModes)
        return;

    int root = bassRootNote (bassPc, lowestVoicingNote, previousBass);
    int last = root;

    const double rate = rateInBeats (s.rate);
    const double swingDelay = s.rate == RateDivision::EighthTriplet ? 0.0 : swing * rate / 3.0;

    // WP0 STUB (WP2 renders the phase-2 pattern modes): every pattern mode plays exactly like Root for now.
    const BassMode mode = isBassPattern (s.bass) ? BassMode::Root : s.bass;

    switch (mode)
    {
        case BassMode::Root:
            r.emit (root, 0.0, heldLength (slotLength, gate), kBassChannel, 0, 0);
            break;

        case BassMode::RootFifth:
        {
            // The slot's bass note on even beats, the other note (chosen from it by the same rule) on odd beats.
            // Re-choosing the bass note every beat would let the centre pull flip it by an octave (C2 G2 C3 G2).
            const int otherPc = bassPc == rootPc ? fifthPc : rootPc;
            const int other = bassRootNote (otherPc, lowestVoicingNote, root);
            const int beats = (int) std::ceil (slotLength - 1.0e-6);
            for (int b = 0; b < beats; ++b)
            {
                const double span = std::min (1.0, slotLength - b);
                last = (b & 1) == 0 ? root : other;
                r.emit (last, (double) b, gate * span, kBassChannel, b, b & 1);
            }
            break;
        }

        case BassMode::Octaves:
        {
            // Keep the voice-led entry note. Alternate upwards when clear and in range,
            // otherwise downwards when available, without moving the entry an octave.
            int other = root + 12;
            if (other > kBassHigh || (other > lowestVoicingNote - 5 && root - 12 >= kBassLow))
                other = root - 12;
            for (auto& st : stepGrid (slotLength, rate, swingDelay))
            {
                last = (st.index & 1) == 0 ? root : other;
                r.emit (last, st.start, gate * st.span, kBassChannel, st.index, st.index & 1);
            }
            break;
        }

        case BassMode::Pulse:
            for (auto& st : stepGrid (slotLength, rate, swingDelay))
                r.emit (root, st.start, gate * st.span, kBassChannel, st.index, 0);
            break;

        case BassMode::Off:
        case BassMode::NumModes:
        default:
            break;
    }
    previousBass = last;
}

// An earlier instance of a note is shortened so it ends before the same note (same channel) starts again.
void removeSameNoteOverlaps (std::vector<NoteEvent>& events)
{
    std::sort (events.begin(), events.end(), [] (const NoteEvent& a, const NoteEvent& b)
    {
        if (a.channel != b.channel) return a.channel < b.channel;
        if (a.note != b.note)       return a.note < b.note;
        return a.startBeat < b.startBeat;
    });

    std::vector<NoteEvent> kept;
    kept.reserve (events.size());

    for (size_t i = 0; i < events.size(); ++i)
    {
        auto e = events[i];
        if (i + 1 < events.size())
        {
            const auto& next = events[i + 1];
            if (next.channel == e.channel && next.note == e.note && e.startBeat + e.lengthBeats > next.startBeat)
                e.lengthBeats = next.startBeat - e.startBeat;
        }

        if (e.lengthBeats >= kMinLength)
            kept.push_back (e);
    }

    events.swap (kept);
}
} // namespace

//==============================================================================
std::vector<NoteEvent> renderProgression (const Progression& progression, const std::vector<std::vector<int>>& voicings,
                                          const RenderSettings& settings)
{
    std::vector<NoteEvent> events;

    const double gate = std::clamp ((double) settings.gate, 0.05, 1.0);
    const double swing = std::clamp ((double) settings.swing, 0.0, 1.0);
    const double humanize = std::clamp ((double) settings.humanize, 0.0, 1.0);
    const float velocity = std::clamp (settings.velocity, kMinVelocity, 1.0f);

    double slotStart = 0.0;
    int previousBass = 40;

    for (int i = 0; i < (int) progression.slots.size(); ++i)
    {
        const auto& slot = progression.slots[(size_t) i];
        const double slotLength = (double) slot.lengthBeats;
        if (slotLength <= 0.0)
            continue;

        std::vector<int> notes;
        if (i < (int) voicings.size())
            notes = voicings[(size_t) i];

        notes.erase (std::remove_if (notes.begin(), notes.end(), [] (int n) { return n < 0 || n > 127; }), notes.end());
        std::sort (notes.begin(), notes.end());
        notes.erase (std::unique (notes.begin(), notes.end()), notes.end());

        SlotRenderer r { events, i, slotStart, slotStart + slotLength, velocity, humanize };

        renderChords (r, notes, settings, slotLength, gate, swing);
        const int rootPc = pc (slot.chord.root);
        const auto chordPcs = slot.chord.pitchClasses();
        const int bassPc = slot.inversion >= 0
            ? (notes.empty() ? chordPcs[(size_t) std::min (slot.inversion, (int) chordPcs.size() - 1)] : pc (notes.front()))
            : rootPc;
        // Root + 5th uses the chord's own fifth (b5 of dim / m7b5 chords, #5 of augmented), never a clashing P5.
        const auto& intervals = qualityIntervals (slot.chord.quality);
        int fifthInterval = 7;
        for (int candidate : { 7, 6, 8 })
            if (std::find (intervals.begin(), intervals.end(), candidate) != intervals.end())
            {
                fifthInterval = candidate;
                break;
            }
        renderBass (r, rootPc, bassPc, pc (rootPc + fifthInterval), notes.empty() ? 60 : notes.front(), previousBass,
                    settings, slotLength, gate, swing);

        slotStart += slotLength;
    }

    removeSameNoteOverlaps (events);

    std::sort (events.begin(), events.end(), [] (const NoteEvent& a, const NoteEvent& b)
    {
        if (a.startBeat != b.startBeat) return a.startBeat < b.startBeat;
        if (a.channel != b.channel)     return a.channel < b.channel;
        return a.note < b.note;
    });

    return events;
}
} // namespace cb
