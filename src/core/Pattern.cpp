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
//  - Block: whole chord for gate * slot length. Strum: the same, note i (low -> high) starts 0.03 * i later.
//  - Pulse: whole chord on every step. Arp Up / Down / Up-Down: one note per step (Up-Down: 1 2 3 4 3 2 | 1 2 ...).
//  - Offbeat: whole chord on the "&" of every beat, gate * half a beat long. The "&" is swung like an odd step when
//    it is one (1/8 rate); at other rates it is not on an odd step and stays straight.
//  - Broken: the grid runs at min(rate, 1/8) so a 1/4 rate still has room inside the beat; on the first step of
//    every beat the lowest note, on the following steps of the beat the remaining notes together.
//  - Humanize: deterministic hash of (slot, step, voice, channel); timing +-0.015 * humanize beats (clamped to the
//    slot), velocity +-0.12 * humanize. Velocities always stay in [1/127, 1].
//  - Bass (channel 2): chord root in [lowest voicing note - 12, lowest - 1], folded into [28, 55] by octaves.
//    Root: held for gate * slot. Root + 5th: alternating every beat (root first), gate * 1 beat.
//    Octaves: low/high octave alternating every step. Pulse: root on every step.
//    For Root + 5th and Octaves the whole figure moves down an octave when its upper note would exceed 55, so
//    every bass note stays in [28, 55].
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
    static const char* names[] = { "Off", "Root", "Root + 5th", "Octaves", "Pulse" };
    return names[std::clamp ((int) m, 0, numBassModes - 1)];
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

int bassRootNote (int rootPc, int lowestVoicingNote)
{
    const int low = lowestVoicingNote - 12;
    int n = low + pc (rootPc - low);   // in [lowest - 12, lowest - 1]
    while (n < kBassLow)
        n += 12;
    while (n > kBassHigh)
        n -= 12;
    return n;
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
            for (int i = 0; i < n; ++i)
                r.emit (notes[(size_t) i], kStrumOffset * i, gate * slotLength, kChordChannel, 0, i);
            break;

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
                r.emit (notes[(size_t) i], 0.0, gate * slotLength, kChordChannel, 0, i);
            break;
    }
}

void renderBass (const SlotRenderer& r, int rootPc, int lowestVoicingNote, const RenderSettings& s, double slotLength,
                 double gate, double swing)
{
    if (s.bass == BassMode::Off || (int) s.bass < 0 || (int) s.bass >= numBassModes)
        return;

    int root = bassRootNote (rootPc, lowestVoicingNote);

    const double rate = rateInBeats (s.rate);
    const double swingDelay = s.rate == RateDivision::EighthTriplet ? 0.0 : swing * rate / 3.0;

    switch (s.bass)
    {
        case BassMode::Root:
            r.emit (root, 0.0, gate * slotLength, kBassChannel, 0, 0);
            break;

        case BassMode::RootFifth:
        {
            if (root + 7 > kBassHigh && root - 12 >= kBassLow)
                root -= 12;
            const int beats = (int) std::ceil (slotLength - 1.0e-6);
            for (int b = 0; b < beats; ++b)
            {
                const double span = std::min (1.0, slotLength - b);
                r.emit ((b & 1) == 0 ? root : root + 7, (double) b, gate * span, kBassChannel, b, b & 1);
            }
            break;
        }

        case BassMode::Octaves:
            if (root + 12 > kBassHigh && root - 12 >= kBassLow)
                root -= 12;
            for (auto& st : stepGrid (slotLength, rate, swingDelay))
                r.emit ((st.index & 1) == 0 ? root : root + 12, st.start, gate * st.span, kBassChannel, st.index, st.index & 1);
            break;

        case BassMode::Pulse:
            for (auto& st : stepGrid (slotLength, rate, swingDelay))
                r.emit (root, st.start, gate * st.span, kBassChannel, st.index, 0);
            break;

        case BassMode::Off:
        case BassMode::NumModes:
        default:
            break;
    }
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
        renderBass (r, pc (slot.chord.root), notes.empty() ? 60 : notes.front(), settings, slotLength, gate, swing);

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
