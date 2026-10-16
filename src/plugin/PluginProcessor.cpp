#include "plugin/PluginProcessor.h"

#include "plugin/MidiExport.h"
#include "plugin/PreviewSynth.h"
#include "ui/PluginEditor.h"

// STUB - to be implemented (owner: processor agent). The parameter layout below is final (UI attaches to it).

namespace
{
juce::StringArray namesFrom (int count, const std::function<const char* (int)>& nameOf)
{
    juce::StringArray names;
    for (int i = 0; i < count; ++i)
        names.add (nameOf (i));
    return names;
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    using namespace juce;
    namespace id = cb::ParamIDs;

    AudioProcessorValueTreeState::ParameterLayout layout;
    auto pid = [] (const char* s) { return ParameterID { s, 1 }; };

    layout.add (std::make_unique<AudioParameterChoice> (pid (id::keyRoot), "Key",
        StringArray { "C", "C#/Db", "D", "D#/Eb", "E", "F", "F#/Gb", "G", "G#/Ab", "A", "A#/Bb", "B" }, 0));
    layout.add (std::make_unique<AudioParameterChoice> (pid (id::scale), "Scale",
        namesFrom (cb::numScaleTypes, [] (int i) { return cb::scaleInfo ((cb::ScaleType) i).name; }), 0));
    layout.add (std::make_unique<AudioParameterInt> (pid (id::octave), "Octave", -3, 3, 0));
    layout.add (std::make_unique<AudioParameterChoice> (pid (id::voicing), "Voicing",
        namesFrom (cb::numVoicingStyles, [] (int i) { return cb::voicingStyleName ((cb::VoicingStyle) i); }), 0));
    layout.add (std::make_unique<AudioParameterBool> (pid (id::voiceLeading), "Voice Leading", true));
    layout.add (std::make_unique<AudioParameterChoice> (pid (id::pattern), "Pattern",
        namesFrom (cb::numRhythmPatterns, [] (int i) { return cb::rhythmPatternName ((cb::RhythmPattern) i); }), 0));
    layout.add (std::make_unique<AudioParameterChoice> (pid (id::rate), "Rate",
        namesFrom (cb::numRateDivisions, [] (int i) { return cb::rateDivisionName ((cb::RateDivision) i); }), 1));
    layout.add (std::make_unique<AudioParameterChoice> (pid (id::bassMode), "Bass",
        namesFrom (cb::numBassModes, [] (int i) { return cb::bassModeName ((cb::BassMode) i); }), 1));
    layout.add (std::make_unique<AudioParameterFloat> (pid (id::velocity), "Velocity", NormalisableRange<float> (0.05f, 1.0f), 0.75f));
    layout.add (std::make_unique<AudioParameterFloat> (pid (id::gate), "Gate", NormalisableRange<float> (0.05f, 1.0f), 0.9f));
    layout.add (std::make_unique<AudioParameterFloat> (pid (id::swing), "Swing", NormalisableRange<float> (0.0f, 1.0f), 0.0f));
    layout.add (std::make_unique<AudioParameterFloat> (pid (id::humanize), "Humanize", NormalisableRange<float> (0.0f, 1.0f), 0.0f));
    layout.add (std::make_unique<AudioParameterBool> (pid (id::previewOn), "Preview Sound", true));
    layout.add (std::make_unique<AudioParameterChoice> (pid (id::previewTone), "Preview Tone",
        StringArray { "Soft Keys", "Warm Pad", "Glass Pluck" }, 0));
    layout.add (std::make_unique<AudioParameterFloat> (pid (id::previewVolume), "Preview Volume",
        NormalisableRange<float> (-48.0f, 0.0f), -9.0f));
    layout.add (std::make_unique<AudioParameterBool> (pid (id::midiOut), "MIDI Out", true));
    layout.add (std::make_unique<AudioParameterBool> (pid (id::syncToHost), "Sync To Host", true));
    layout.add (std::make_unique<AudioParameterBool> (pid (id::scaleLock), "Scale Lock", false));
    layout.add (std::make_unique<AudioParameterBool> (pid (id::chordTrigger), "Chord Trigger", false));
    layout.add (std::make_unique<AudioParameterBool> (pid (id::triggerSevenths), "Trigger Sevenths", false));
    return layout;
}
} // namespace

struct ChordbloomProcessor::Impl
{
    explicit Impl (ChordbloomProcessor& p)
        : apvts (p, nullptr, "Params", createParameterLayout())
    {
    }

    juce::AudioProcessorValueTreeState apvts;
    cb::Progression progression;
    std::vector<cb::PaletteSection> palette = cb::buildPalette ({});
};

ChordbloomProcessor::ChordbloomProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    impl = std::make_unique<Impl> (*this);
}

ChordbloomProcessor::~ChordbloomProcessor() = default;

void ChordbloomProcessor::prepareToPlay (double, int) {}
void ChordbloomProcessor::releaseResources() {}

bool ChordbloomProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void ChordbloomProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    buffer.clear();
}

juce::AudioProcessorEditor* ChordbloomProcessor::createEditor() { return new ChordbloomEditor (*this); }

void ChordbloomProcessor::getStateInformation (juce::MemoryBlock&) {}
void ChordbloomProcessor::setStateInformation (const void*, int) {}

juce::AudioProcessorValueTreeState& ChordbloomProcessor::getAPVTS() noexcept { return impl->apvts; }
cb::Key ChordbloomProcessor::getKey() const { return {}; }
const std::vector<cb::PaletteSection>& ChordbloomProcessor::getPalette() const { return impl->palette; }
cb::VoicingSettings ChordbloomProcessor::getVoicingSettings() const { return {}; }
cb::RenderSettings ChordbloomProcessor::getRenderSettings() const { return {}; }

const cb::Progression& ChordbloomProcessor::getProgression() const { return impl->progression; }
void ChordbloomProcessor::setProgression (const cb::Progression& p) { impl->progression = p; sendChangeMessage(); }
bool ChordbloomProcessor::addChord (const cb::Chord&, int) { return false; }
void ChordbloomProcessor::removeSlot (int) {}
void ChordbloomProcessor::moveSlot (int, int) {}
void ChordbloomProcessor::updateSlot (int, const cb::ProgressionSlot&) {}
bool ChordbloomProcessor::duplicateSlot (int) { return false; }
void ChordbloomProcessor::clearProgression() {}
bool ChordbloomProcessor::undo() { return false; }
bool ChordbloomProcessor::redo() { return false; }
bool ChordbloomProcessor::canUndo() const { return false; }
bool ChordbloomProcessor::canRedo() const { return false; }

std::vector<std::vector<int>> ChordbloomProcessor::getProgressionVoicings() const { return {}; }
std::vector<cb::NoteEvent> ChordbloomProcessor::renderProgressionEvents() const { return {}; }

void ChordbloomProcessor::startPreviewChord (const cb::Chord&) {}
void ChordbloomProcessor::startPreviewSlot (int) {}
void ChordbloomProcessor::startPreviewNote (int) {}
void ChordbloomProcessor::stopPreview() {}
std::optional<cb::Chord> ChordbloomProcessor::getPreviewChord() const { return std::nullopt; }

void ChordbloomProcessor::setLocalPlayback (bool) {}
bool ChordbloomProcessor::isLocalPlaybackOn() const noexcept { return false; }
bool ChordbloomProcessor::isHostPlaying() const noexcept { return false; }
int ChordbloomProcessor::getPlayingSlot() const noexcept { return -1; }
double ChordbloomProcessor::getPlaybackBeat() const noexcept { return 0.0; }
double ChordbloomProcessor::getHostBpm() const noexcept { return 120.0; }

std::bitset<128> ChordbloomProcessor::getSoundingNotes() const noexcept { return {}; }
int ChordbloomProcessor::getTriggeredDegree() const noexcept { return -1; }

juce::File ChordbloomProcessor::writeMidiToTempFile (ExportPart) const { return {}; }
bool ChordbloomProcessor::saveMidiFile (const juce::File&, ExportPart) const { return false; }

juce::Point<int> ChordbloomProcessor::getSavedEditorSize() const { return {}; }
void ChordbloomProcessor::setSavedEditorSize (juce::Point<int>) {}

#if ! CHORDBLOOM_DEV_TOOL
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new ChordbloomProcessor(); }
#endif
