#include "plugin/PreviewSynth.h"

// STUB - to be implemented (owner: processor agent)

struct PreviewSynth::Impl
{
};

PreviewSynth::PreviewSynth() : impl (std::make_unique<Impl>()) {}
PreviewSynth::~PreviewSynth() = default;

void PreviewSynth::prepare (double, int) {}
void PreviewSynth::reset() {}
void PreviewSynth::setTone (Tone) {}
void PreviewSynth::setGainDecibels (float) {}
void PreviewSynth::render (juce::AudioBuffer<float>&, const juce::MidiBuffer&, int) {}
void PreviewSynth::allNotesOff (bool) {}
