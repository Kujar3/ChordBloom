#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <bitset>
#include <optional>

#include "core/Palette.h"
#include "core/Pattern.h"
#include "core/Progression.h"
#include "core/Suggest.h"
#include "core/Theory.h"
#include "core/Voicing.h"

//==============================================================================
// Parameter IDs (APVTS). All parameters are automatable.
namespace cb::ParamIDs
{
    inline constexpr const char* keyRoot         = "keyRoot";         // choice: C, C#/Db, D, ... B           default C
    inline constexpr const char* scale           = "scale";           // choice: scaleInfo(i).name            default Major
    inline constexpr const char* octave          = "octave";          // int -3..+3 (moves VoicingSettings.centerNote by 12)  default 0
    inline constexpr const char* voicing         = "voicing";         // choice: voicingStyleName(i)          default Close
    inline constexpr const char* voiceLeading    = "voiceLeading";    // bool                                 default true
    inline constexpr const char* pattern         = "pattern";         // choice: rhythmPatternName(i)         default Block
    inline constexpr const char* rate            = "rate";            // choice: rateDivisionName(i)          default 1/8
    inline constexpr const char* bassMode        = "bassMode";        // choice: bassModeName(i)              default Root
    inline constexpr const char* velocity        = "velocity";        // float 0.05..1                        default 0.75
    inline constexpr const char* gate            = "gate";            // float 0.05..1                        default 0.9
    inline constexpr const char* swing           = "swing";           // float 0..1                           default 0
    inline constexpr const char* humanize        = "humanize";        // float 0..1                           default 0
    inline constexpr const char* previewOn       = "previewOn";       // bool: built-in sound audible         default true
    inline constexpr const char* previewTone     = "previewTone";     // choice: "Soft Keys", "Warm Pad", "Glass Pluck"  default Soft Keys
    inline constexpr const char* previewVolume   = "previewVolume";   // float dB -48..0                      default -9
    inline constexpr const char* midiOut         = "midiOut";         // bool: send generated MIDI to the host default true
    inline constexpr const char* syncToHost      = "syncToHost";      // bool: loop progression with host transport  default true
    inline constexpr const char* scaleLock       = "scaleLock";       // bool: snap incoming MIDI notes to the scale  default false
    inline constexpr const char* chordTrigger    = "chordTrigger";    // bool: incoming white keys C..B play degree chords 1..7  default false
    inline constexpr const char* triggerSevenths = "triggerSevenths"; // bool: chord trigger uses seventh chords   default false
}

//==============================================================================
/**
    Chordbloom: chord palette / progression / MIDI generator.

    Audio: stereo output from the built-in preview synth (when previewOn). No audio input.
    MIDI out (when midiOut): chords on channel 1, bass on channel 2, plus transformed live input.

    THREADING CONTRACT
      - Every public method below is message-thread only unless marked "any thread".
      - The audio thread never allocates, locks (except try-locks) or calls into src/core.
        The message thread pre-renders everything the audio thread needs (progression events, chord-trigger
        voicings) and hands it over lock-free; preview notes go through a lock-free FIFO.
      - The processor is a juce::ChangeBroadcaster: sendChangeMessage() (message thread) whenever the progression,
        the key/palette, or undo state changes. The editor listens and refreshes.
*/
class ChordbloomProcessor : public juce::AudioProcessor,
                            public juce::ChangeBroadcaster
{
public:
    ChordbloomProcessor();
    ~ChordbloomProcessor() override;

    static constexpr int kChordChannel = 1;
    static constexpr int kBassChannel = 2;

    //==============================================================================
    // juce::AudioProcessor
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    // Host bypass: note-offs for everything Chordbloom holds (no stuck notes downstream), input MIDI passes through,
    // the preview sound's release tails fade out. Playback re-syncs when the bypass ends. (Added by the integrator.)
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlockBypassed;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Chordbloom"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;   // APVTS state + progression + editor size, versioned
    void setStateInformation (const void*, int) override;

    //==============================================================================
    // Parameters & derived settings
    juce::AudioProcessorValueTreeState& getAPVTS() noexcept;

    cb::Key getKey() const;
    const std::vector<cb::PaletteSection>& getPalette() const;   // rebuilt when key changes (then sendChangeMessage)
    cb::VoicingSettings getVoicingSettings() const;
    cb::RenderSettings getRenderSettings() const;

    //==============================================================================
    // Progression editing. All edits are undoable (snapshot stack, max 100) and trigger re-render + sendChangeMessage.
    const cb::Progression& getProgression() const;
    void setProgression (const cb::Progression&);
    bool addChord (const cb::Chord&, int insertIndex = -1);   // -1 = append. false when already kMaxSlots.
    void removeSlot (int index);
    void moveSlot (int fromIndex, int toIndex);               // toIndex is the final index of the moved slot
    void updateSlot (int index, const cb::ProgressionSlot&);
    bool duplicateSlot (int index);                           // inserts copy after index; false when full
    void clearProgression();

    bool undo();
    bool redo();
    bool canUndo() const;
    bool canRedo() const;

    std::vector<std::vector<int>> getProgressionVoicings() const;   // voiceProgression (progression, voicing settings)
    std::vector<cb::NoteEvent> renderProgressionEvents() const;     // one loop, current render settings

    //==============================================================================
    // Auditioning (hand-off to the audio thread is lock-free). Preview notes sound through the built-in synth
    // (if previewOn) and are sent as MIDI out (if midiOut), chords on channel 1.
    void startPreviewChord (const cb::Chord&);   // stops any sounding preview first; voice-led from the previous preview
    void startPreviewSlot (int slotIndex);       // uses the slot's progression voicing
    void startPreviewNote (int midiNote);        // single note (keyboard clicks)
    void stopPreview();                          // note-offs for whatever preview is sounding
    std::optional<cb::Chord> getPreviewChord() const;   // chord currently being previewed (chord/slot previews), if any
    std::vector<int> getPreviewNotes() const;           // MIDI notes of the preview currently held (empty when none). Added by the integrator.

    //==============================================================================
    // Transport
    void setLocalPlayback (bool shouldPlay);     // loop the progression without the host (standalone / host stopped)
    bool isLocalPlaybackOn() const noexcept;     // any thread
    bool isHostPlaying() const noexcept;         // any thread
    int getPlayingSlot() const noexcept;         // any thread: slot index sounding in playback, -1 when not playing
    double getPlaybackBeat() const noexcept;     // any thread: position inside the loop in beats (valid while playing)
    double getHostBpm() const noexcept;          // any thread: last host tempo, 120 when unknown

    //==============================================================================
    // Live display (any thread)
    std::bitset<128> getSoundingNotes() const noexcept;   // notes currently on from any source (preview, playback, live input)
    int getTriggeredDegree() const noexcept;              // chord-trigger mode: degree 0..6 being held, -1 when none

    //==============================================================================
    // MIDI export
    enum class ExportPart { Chords, Bass, Both };
    // Writes one loop of the progression at the host tempo into <temp>/Chordbloom/, file name like
    // "Chordbloom - C Major - Chords.mid". Returns the file, or File() when the progression is empty / on failure.
    juce::File writeMidiToTempFile (ExportPart) const;
    bool saveMidiFile (const juce::File& destination, ExportPart) const;

    //==============================================================================
    // Editor size persistence (stored in the plugin state)
    juce::Point<int> getSavedEditorSize() const;   // {0,0} when never saved
    void setSavedEditorSize (juce::Point<int>);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChordbloomProcessor)
};
