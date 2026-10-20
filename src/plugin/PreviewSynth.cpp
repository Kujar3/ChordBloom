#include "plugin/PreviewSynth.h"
#include <array>
#include <cmath>
#include <cstdint>

struct PreviewSynth::Impl
{
    static constexpr double twoPi = juce::MathConstants<double>::twoPi;
    struct Voice
    {
        Tone tone = Tone::SoftKeys;
        int note = 0, channel = 0;
        std::uint64_t age = 0;
        bool active = false, released = false;
        bool sustained = false;   // note-off arrived while the sustain pedal (CC64) was down: keeps ringing until pedal up
        double phase = 0.0, partialPhase = 0.0, thirdPhase = 0.0, increment = 0.0;
        float velocity = 0.0f, envelope = 0.0f, attackStep = 0.0f;
        float decay = 1.0f, release = 1.0f, modulation = 0.0f, modulationDecay = 1.0f;
        float panLeft = 0.7071f, panRight = 0.7071f, filterState = 0.0f, filterG = 0.0f;
        float lastLeft = 0.0f, lastRight = 0.0f;
        int attackRemaining = 0, fadeRemaining = 0, fadeLength = 1;
        int holdRemaining = 0;    // samples of full sustain before `decay` applies (Warm Pad)
        float maxIndex = 0.0f;    // Soft Keys: phase-modulation index cap that keeps the sidebands below ~0.45 * sampleRate
    };
    // Fixed tail storage preserves the stolen oscillator during its 3 ms fade.
    struct Slot
    {
        Voice voice, tail;
        float residualLeft = 0.0f, residualRight = 0.0f;
        int residualRemaining = 0;
    };
    std::array<Slot, 24> slots {};
    std::array<bool, 17> sustainDown {};   // per MIDI channel 1..16 (index 0 unused)
    juce::AudioBuffer<float> scratch;
    juce::Reverb reverb;
    juce::SmoothedValue<float> gain { 1.0f };
    double sampleRate = 44100.0;
    float targetGain = 1.0f;
    Tone selectedTone = Tone::SoftKeys;
    std::uint64_t nextAge = 0;
    int capacity = 0, stealSamples = 133;

    float coefficient (double seconds) const
    {
        // Envelope times denote decay to -60 dB.
        return static_cast<float> (std::exp (std::log (0.001) / (seconds * sampleRate)));
    }
    static double advance (double phase, double step)
    {
        phase += step;
        return phase - std::floor (phase);
    }
    static double saw (double phase, double step)
    {
        double correction = 0.0;
        if (phase < step)
        {
            const auto x = phase / step;
            correction = x + x - x * x - 1.0;
        }
        else if (phase > 1.0 - step)
        {
            const auto x = (phase - 1.0) / step;
            correction = x * x + x + x + 1.0;
        }
        return 2.0 * phase - 1.0 - correction;
    }
    void quickRelease (Voice& voice)
    {
        if (voice.active && voice.fadeRemaining == 0)
        {
            voice.fadeLength = stealSamples;
            voice.fadeRemaining = stealSamples;
        }
    }
    void noteOn (int note, int channel, float velocity)
    {
        auto* chosen = &slots.front();
        for (auto& slot : slots)
        {
            if (! slot.voice.active)
            {
                chosen = &slot;
                break;
            }
            if (slot.voice.age < chosen->voice.age)
                chosen = &slot;
        }
        if (chosen->voice.active)
        {
            // Repeated stealing within 3 ms retains the interrupted tail's
            // instantaneous value, then fades it without an unbounded tail pool.
            const auto remaining = static_cast<float> (chosen->residualRemaining)
                                 / static_cast<float> (stealSamples);
            chosen->residualLeft = chosen->residualLeft * remaining + chosen->tail.lastLeft;
            chosen->residualRight = chosen->residualRight * remaining + chosen->tail.lastRight;
            chosen->residualRemaining = stealSamples;
            chosen->tail = chosen->voice;
            quickRelease (chosen->tail);
        }
        auto& voice = chosen->voice;
        voice = Voice {};
        voice.active = true;
        voice.note = note;
        voice.channel = channel;
        voice.age = nextAge++;
        voice.tone = selectedTone; // Tone changes affect new notes only.
        voice.velocity = velocity;
        voice.increment = juce::jmin (0.45, juce::MidiMessage::getMidiNoteInHertz (note) / sampleRate);
        const auto pan = 0.5 + 0.18 * std::sin (static_cast<double> (note) * 0.73);
        voice.panLeft = static_cast<float> (std::cos (pan * twoPi * 0.25));
        voice.panRight = static_cast<float> (std::sin (pan * twoPi * 0.25));
        const auto isPad = voice.tone == Tone::WarmPad;
        voice.attackRemaining = juce::jmax (1, static_cast<int> (std::ceil (sampleRate * (isPad ? 0.25 : 0.003))));
        voice.attackStep = 1.0f / static_cast<float> (voice.attackRemaining);
        // Warm Pad: full sustain for 20 s, then a 10 s fade (to -60 dB), so even a lost note-off can never drone forever.
        voice.holdRemaining = isPad ? static_cast<int> (sampleRate * 20.0) : 0;
        voice.decay = coefficient (isPad ? 10.0 : (voice.tone == Tone::SoftKeys ? 2.4 : 0.8));
        voice.release = coefficient (isPad ? 1.2 : (voice.tone == Tone::SoftKeys ? 0.35 : 0.18));
        voice.modulation = 0.25f + 2.2f * velocity * velocity;
        // Carson-style bound: the strongest sidebands sit at (2 * index + 3) * f, keep them below 0.45 * sampleRate.
        voice.maxIndex = static_cast<float> (juce::jmax (0.0, (0.45 / juce::jmax (1.0e-9, voice.increment) - 3.0) * 0.5));
        voice.modulationDecay = coefficient (0.7);
        const auto cutoff = juce::jmin (sampleRate * 0.18, 1300.0 + 1600.0 * velocity);
        const auto g = std::tan (juce::MathConstants<double>::pi * cutoff / sampleRate);
        voice.filterG = static_cast<float> (g / (1.0 + g));
        if (isPad)
        {
            voice.partialPhase = 0.31;
            voice.thirdPhase = 0.67;
        }
    }
    // respectSustain: a plain note-off while the channel's sustain pedal is down only marks the voice as sustained.
    void stopNotes (bool allowTailOff, int channel = 0, int note = -1, bool respectSustain = false)
    {
        for (auto& slot : slots)
            for (auto* voice : { &slot.voice, &slot.tail })
                if (voice->active && (channel == 0 || voice->channel == channel)
                    && (note < 0 || voice->note == note))
                {
                    if (respectSustain && voice->channel >= 1 && voice->channel <= 16
                        && sustainDown[static_cast<size_t> (voice->channel)])
                    {
                        voice->sustained = true;
                        continue;
                    }
                    voice->sustained = false;
                    if (allowTailOff)
                        voice->released = true;
                    else
                        quickRelease (*voice);
                }
    }
    void setSustain (int channel, bool down)
    {
        if (channel < 1 || channel > 16)
            return;
        sustainDown[static_cast<size_t> (channel)] = down;
        if (down)
            return;
        for (auto& slot : slots)
            for (auto* voice : { &slot.voice, &slot.tail })
                if (voice->active && voice->sustained && voice->channel == channel)
                {
                    voice->sustained = false;
                    voice->released = true;
                }
    }
    void tick (Voice& voice, float& left, float& right)
    {
        if (! voice.active)
            return;
        if (voice.released)
            voice.envelope *= voice.release;
        else if (voice.attackRemaining > 0)
        {
            voice.envelope = juce::jmin (1.0f, voice.envelope + voice.attackStep);
            --voice.attackRemaining;
        }
        else if (voice.holdRemaining > 0)
            --voice.holdRemaining;
        else
            voice.envelope *= voice.decay;
        double signal = 0.0;
        const auto step = voice.increment;
        if (voice.tone == Tone::WarmPad)
        {
            signal = (saw (voice.phase, step) + saw (voice.partialPhase, step * 0.996)
                    + saw (voice.thirdPhase, step * 1.004)) / 3.0;
            voice.partialPhase = advance (voice.partialPhase, step * 0.996);
            voice.thirdPhase = advance (voice.thirdPhase, step * 1.004);
            // One-pole trapezoidal/ZDF lowpass.
            const auto v = (static_cast<float> (signal) - voice.filterState) * voice.filterG;
            signal = v + voice.filterState;
            voice.filterState = static_cast<float> (signal) + v;
            signal *= 1.45;
        }
        else if (voice.tone == Tone::GlassPluck)
        {
            signal = std::sin (twoPi * voice.phase);
            if (step * 2.71 < 0.45)
                signal += 0.45 * std::sin (twoPi * voice.partialPhase);
            if (step * 4.13 < 0.45)
                signal += 0.22 * std::sin (twoPi * voice.thirdPhase);
            signal *= 0.65;
            voice.partialPhase = advance (voice.partialPhase, step * 2.71);
            voice.thirdPhase = advance (voice.thirdPhase, step * 4.13);
        }
        else
        {
            const auto index = static_cast<double> (juce::jmin (voice.modulation, voice.maxIndex));
            signal = std::sin (twoPi * voice.phase + index * std::sin (twoPi * voice.partialPhase));
            if (step * 4.0 < 0.45)
                signal += 0.07 * voice.velocity * voice.modulation * std::sin (twoPi * voice.thirdPhase);
            signal *= 0.88;
            voice.modulation *= voice.modulationDecay;
            voice.partialPhase = advance (voice.partialPhase, step * 2.0);
            voice.thirdPhase = advance (voice.thirdPhase, step * 4.0);
        }
        voice.phase = advance (voice.phase, step);
        auto level = voice.envelope * voice.velocity * 0.16f;
        if (voice.fadeRemaining > 0)
        {
            level *= static_cast<float> (voice.fadeRemaining) / static_cast<float> (voice.fadeLength);
            if (--voice.fadeRemaining == 0)
                voice.active = false;
        }
        if (voice.envelope < 0.00001f && (voice.released || voice.attackRemaining == 0))
            voice.active = false;
        const auto sample = static_cast<float> (signal) * level;
        voice.lastLeft = sample * voice.panLeft;
        voice.lastRight = sample * voice.panRight;
        left += voice.lastLeft;
        right += voice.lastRight;
        if (! voice.active)
            voice.lastLeft = voice.lastRight = 0.0f;
    }
    void renderRange (juce::AudioBuffer<float>& output, int start, int end)
    {
        while (start < end)
        {
            const auto count = juce::jmin (capacity, end - start);
            auto* left = scratch.getWritePointer (0);
            auto* right = scratch.getWritePointer (1);
            for (int i = 0; i < count; ++i)
            {
                left[i] = right[i] = 0.0f;
                for (auto& slot : slots)
                {
                    tick (slot.voice, left[i], right[i]);
                    tick (slot.tail, left[i], right[i]);
                    if (slot.residualRemaining > 0)
                    {
                        const auto fade = static_cast<float> (slot.residualRemaining--)
                                        / static_cast<float> (stealSamples);
                        left[i] += slot.residualLeft * fade;
                        right[i] += slot.residualRight * fade;
                    }
                }
            }
            reverb.processStereo (left, right, count);
            for (int i = 0; i < count; ++i)
            {
                const auto volume = gain.getNextValue();
                const auto l = std::tanh (left[i] * volume);
                const auto r = std::tanh (right[i] * volume);
                if (output.getNumChannels() == 1)
                    output.addSample (0, start + i, 0.5f * (l + r));
                else if (output.getNumChannels() >= 2)
                {
                    output.addSample (0, start + i, l);
                    output.addSample (1, start + i, r);
                }
            }
            start += count;
        }
    }
};

PreviewSynth::PreviewSynth() : impl (std::make_unique<Impl>()) {}
PreviewSynth::~PreviewSynth() = default;

void PreviewSynth::prepare (double sampleRate, int maxBlockSize)
{
    impl->sampleRate = std::isfinite (sampleRate) && sampleRate > 0.0 ? sampleRate : 44100.0;
    impl->capacity = juce::jmax (1, maxBlockSize);
    impl->scratch.setSize (2, impl->capacity);
    impl->stealSamples = juce::jmax (1, static_cast<int> (std::ceil (impl->sampleRate * 0.003)));
    impl->gain.reset (impl->sampleRate, 0.025);
    impl->reverb.setSampleRate (impl->sampleRate);
    juce::Reverb::Parameters parameters;
    parameters.roomSize = 0.25f;
    parameters.damping = 0.6f;
    parameters.wetLevel = 0.25f;
    parameters.dryLevel = 0.75f;
    parameters.width = 0.8f;
    parameters.freezeMode = 0.0f;
    impl->reverb.setParameters (parameters);
    reset();
}
void PreviewSynth::reset()
{
    impl->slots = {};
    impl->sustainDown.fill (false);
    impl->nextAge = 0;
    impl->reverb.reset();
    impl->gain.setCurrentAndTargetValue (impl->targetGain);
}
void PreviewSynth::setTone (Tone tone)
{
    if (tone >= Tone::SoftKeys && tone < Tone::NumTones)
        impl->selectedTone = tone;
}
void PreviewSynth::setGainDecibels (float dB)
{
    if (std::isfinite (dB))
    {
        impl->targetGain = juce::Decibels::decibelsToGain (juce::jlimit (-100.0f, 24.0f, dB));
        impl->gain.setTargetValue (impl->targetGain);
    }
}
void PreviewSynth::render (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi, int numSamples)
{
    if (impl->capacity == 0)
        return;
    const juce::ScopedNoDenormals noDenormals;
    const auto count = juce::jlimit (0, buffer.getNumSamples(), numSamples);
    int position = 0;
    for (const auto metadata : midi)
    {
        // Events at or beyond the end of the block (including every event of a 0-sample block) are still applied, at
        // the block end: the synth's note state must always match what was sent as MIDI out.
        const auto eventPosition = juce::jlimit (position, count, metadata.samplePosition);
        impl->renderRange (buffer, position, eventPosition);
        position = eventPosition;
        // Avoid MidiMessage construction: arbitrary SysEx messages can allocate.
        if (metadata.numBytes < 3)
            continue;
        const auto* data = metadata.data;
        const int status = data[0] & 0xf0;
        const int channel = (data[0] & 0x0f) + 1;
        const int note = data[1] & 0x7f;
        const int value = data[2] & 0x7f;
        if (status == 0x90 && value != 0)
            impl->noteOn (note, channel, static_cast<float> (value) / 127.0f);
        else if (status == 0x80 || status == 0x90)
            impl->stopNotes (true, channel, note, true);
        else if (status == 0xb0 && note == 64)
            impl->setSustain (channel, value >= 64);
        else if (status == 0xb0 && note == 121)
            impl->setSustain (channel, false);                 // reset all controllers
        else if (status == 0xb0 && (note == 120 || note == 123))
            impl->stopNotes (note == 123, channel);
    }
    impl->renderRange (buffer, position, count);
}
void PreviewSynth::allNotesOff (bool allowTailOff)
{
    impl->sustainDown.fill (false);   // the pedal-up may never arrive (bypass, sound switched off): never keep sustaining
    impl->stopNotes (allowTailOff);
}

// WP0 STUB (WP4 adds the counter with the 48-slot voice stealing of final-spec 6.5).
int PreviewSynth::getHeldVoiceSteals() const noexcept { return 0; }
