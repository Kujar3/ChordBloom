#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

// Built-in preview instrument so Chordbloom makes sound without any MIDI routing.
// Owned by the processor; every method except prepare() is audio-thread safe (no allocation, no locks).
class PreviewSynth
{
public:
    enum class Tone { SoftKeys = 0, WarmPad, GlassPluck, NumTones };

    PreviewSynth();
    ~PreviewSynth();

    void prepare (double sampleRate, int maxBlockSize);   // message thread / prepareToPlay
    void reset();
    void setTone (Tone);
    void setGainDecibels (float dB);

    // Renders every MIDI note on/off in `midi` (all channels) and ADDS stereo audio into buffer[0..numSamples).
    void render (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi, int numSamples);

    void allNotesOff (bool allowTailOff);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
