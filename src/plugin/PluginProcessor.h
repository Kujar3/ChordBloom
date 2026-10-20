#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <bitset>
#include <optional>

#include "core/Arp.h"
#include "core/Layers.h"
#include "core/Melody.h"
#include "core/MelodyGen.h"
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
    // choice, filters playback MIDI out (previews and live input count as chords, melody auditions as melody):
    //   0 "All parts" (was "Chords + Bass" before the melody existed), 1 "Chords only", 2 "Bass only",
    //   3 "Melody only", 4 "Chords + Bass" (no melody)       default All parts
    inline constexpr const char* midiOutPart     = "midiOutPart";
    inline constexpr const char* melodyOn        = "melodyOn";        // bool: the melody plays with the progression  default true
    // Phase 2 (appended; ParameterID version 2). bassMode: choices 5..14 appended (bassModeName, the 16th-grid
    // patterns); Off (0) also silences the bass. midiOutPart: 5 "Arp only" appended; 0 "All parts" includes the arp,
    // 4 "Chords + Bass" does not.
    inline constexpr const char* arpOn     = "arpOn";     // bool: the arp layer plays (built-in sound + MIDI ch 4)   default false
    inline constexpr const char* arpShape  = "arpShape";  // choice: arpShapeName(i)                                 default Up 3
    inline constexpr const char* arpRate   = "arpRate";   // choice: rateDivisionName(i)                             default 1/16
    inline constexpr const char* arpRhythm = "arpRhythm"; // choice: arpRhythmName(i)                                default Every step
    inline constexpr const char* arpOctave = "arpOctave"; // int -2..+2 (arp centre = voicing centre + 5 + 12 * n)   default 0
    inline constexpr const char* arpGate   = "arpGate";   // float 0.05..1 (shown as %)                              default 0.9
    inline constexpr const char* bassOn    = "bassOn";    // bool: the bass layer plays (with bassMode != Off)       default true
}

//==============================================================================
/**
    Chordbloom: chord palette / progression / MIDI generator.

    Audio: stereo output from the built-in preview synth (when previewOn). No audio input.
    MIDI out (when midiOut): chords on channel 1, bass on channel 2, melody on channel 3, arp on channel 4 (phase 2),
    plus transformed live input.

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
    static constexpr int kMelodyChannel = 3;
    static constexpr int kArpChannel = 4;   // (phase 2)

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

    void getStateInformation (juce::MemoryBlock&) override;   // APVTS state + progression + melody + editor size, versioned
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
    // (Phase 2) renderProgressionEvents = chords + the bass layer's content (custom notes when custom; independent of
    // bassOn; no bass when auto with bassMode Off), identical to phase 1.5 for auto layers.

    //==============================================================================
    // Melody (added with the melody roll). Drawn in the editor's Melody view; plays with the progression on MIDI
    // channel 3 (built-in sound + MIDI out) while melodyOn is on; stored in the plugin state; exported by the Melody
    // chip (ExportPart::Melody). Notes are in beats from the start of the progression; the loop is the progression,
    // so nothing plays without chords. Notes at or past the loop end stay in the melody but are not played.
    //
    // Melody edits share the progression's undo stack: undo() / redo() step back through both in the order they
    // happened. Structural progression edits move the melody along with its chords in the same undo step
    // (cb::melodyFollowingSlots): addChord with an insert index, removeSlot, moveSlot, duplicateSlot, and updateSlot
    // when the length changes. setProgression and clearProgression (Spark, presets, Clear) leave the melody where it is
    // in time; transposeProgression transposes it with the chords (cb::transposeMelody).
    const cb::Melody& getMelody() const;
    void setMelody (const cb::Melody&);                      // sanitised, undoable; no undo step when nothing changes
    void clearMelody();                                      // setMelody ({})
    std::vector<cb::NoteEvent> renderMelodyEvents() const;   // one loop: cb::renderMelody (melody, totalBeats, 3), even
                                                             // when melodyOn is off (export); empty without chords

    // Ghost notes: every instance publishes its melody to a process-wide registry, so each melody roll can show the
    // other Chordbloom instances' melodies faintly (e.g. a bass line or a counter-melody on another channel).
    struct GhostMelody
    {
        int instanceNumber = 0;   // that instance's number (getInstanceNumber)
        cb::Melody melody;
        double loopBeats = 0.0;   // its progression length in beats (0 = no chords)
    };
    std::vector<GhostMelody> getGhostMelodies() const;   // other live instances with melody notes, by instance number
    uint64_t getGhostSerial() const noexcept;            // any thread: changes whenever any instance publishes or goes away
    int getInstanceNumber() const noexcept;              // any thread: 1, 2, 3 ... in creation order within the process

    //==============================================================================
    // Layers (phase 2). The roll shows and edits three layers (cb::Layer): Melody (= the melody API, channel 3), Arp
    // (channel 4) and Bass (channel 2, renderProgression's bass). Arp and Bass are "auto" (rendered from the chords and
    // their parameters on every change; nothing stored) until a note is edited, then "custom" (stored notes play
    // instead, in the plugin state and the undo stack) until resetLayer. Custom layers follow every progression edit in
    // the same undo step (cb::layerNotesFollowing): structural edits move them, in-place chord changes and a
    // same-structure setProgression remap them by chord role, new slots get the auto pattern; setProgression with
    // another slot structure and clearProgression reset them; transposeProgression transposes them. Parameters are
    // never undoable; host automation and state loads never reset a custom layer (its notes win).
    // Undo entries hold { progression, melody, arp, bass }.
    bool isLayerCustom (cb::Layer) const;   // Melody: always false
    // What the layer plays, one loop from beat 0, as editable notes, even while muted: the melody; a custom layer's
    // notes; or the auto render without humanize (swing kept) through cb::notesFromEvents. Bass auto with bassMode Off:
    // empty. Cached per getLayerSerial (cheap enough to call from paint).
    cb::Melody getLayerNotes (cb::Layer) const;
    // Melody: setMelody. Arp / Bass: sanitised for cb::layerLimits; when the result equals the layer's auto notes the
    // layer becomes / stays auto, otherwise it is custom with these notes. One undo step when anything changes.
    void setLayerNotes (cb::Layer, const cb::Melody&);
    // Arp / Bass, undoable. slotIndex -1: back to auto. Otherwise "Reset this chord": the notes starting in that slot
    // become its auto notes (cb::replaceSlotNotes); the layer becomes auto when the result equals the auto notes.
    // Melody, auto layers and invalid slots: no-op.
    void resetLayer (cb::Layer, int slotIndex = -1);
    std::vector<bool> getEditedSlots (cb::Layer) const;   // per slot (cb::editedSlots); all false when auto / Melody
    // The layer's play switch (the roll tab's LED): melodyOn / arpOn / bassOn && bassMode != Off.
    bool isLayerOn (cb::Layer) const;
    // Host-notifying parameter gesture. Bass on: bassOn = true and, when bassMode is Off, bassMode = Root. Bass off:
    // bassOn = false (bassMode unchanged). Melody / Arp: melodyOn / arpOn.
    void setLayerOn (cb::Layer, bool shouldBeOn);
    bool isLayerPlaying (cb::Layer) const;   // isLayerOn and at least one of its notes starts inside the loop
    // Call right after applying a real user pick (popup result, menu result, browser arrow; never from onChange or a
    // parameter listener) of one of the layer's pattern settings (Arp: shape, rate, rhythm, gate, octave; Bass: a
    // bassMode other than Off): the UI first sets the parameter with a host gesture (begin / setValueNotifyingHost /
    // end; the raw value updates synchronously), then calls this. A custom layer is reset to auto (one undo step whose
    // single publish already renders the new pattern, so no note of the old one is struck) and the layer is switched on.
    void beginUserLayerPick (cb::Layer);
    // One loop of the layer exactly as it plays when on (humanize, custom notes), on its channel, sorted, even while
    // muted (export). Bass auto with bassMode Off: empty. Empty without chords.
    std::vector<cb::NoteEvent> renderLayerEvents (cb::Layer) const;
    cb::ArpSettings getArpSettings() const;   // from the arp parameters; centreNote = voicing centerNote + 5 + 12 * arpOctave
                                              // (clamped 40..96, as cb::ArpSettings says)
    // Any thread: changes whenever the notes getLayerNotes returns or a custom flag may have changed for any layer,
    // including auto re-renders after parameter changes (which send no change message), and whenever a parameter that
    // layer UI shows changes (melodyOn, arpOn, bassOn, bassMode, any arp* parameter; every re-render bumps it: commits,
    // undo / redo, state loads, parameter refreshes). The editor and the roll poll it at 30 Hz and refresh the layer
    // UI (tabs, pills, Controls caption, export chips, view-switch dot) when it changes. It may change without any note
    // changing, so the roll clears its selection only when the current layer's notes differ as well.
    uint64_t getLayerSerial() const noexcept;
    // Audition on the layer's channel; counts as that layer's part for midiOutPart; stopped by stopPreview().
    void startPreviewLayerNote (cb::Layer, int midiNote);

    //==============================================================================
    // Melody generator (phase 2). Replaces the melody notes that start inside the loop with cb::generateMelody
    // (progression, key, getMelodyGenContext(), seed, settings); notes at or after the loop end stay. One undo step.
    // false (nothing changes, no undo step) without chords or when the result equals the current melody.
    bool generateMelody (const cb::MelodyGenSettings&, std::uint32_t seed);
    // True when the last successful generateMelody result is remembered and the melody still equals it. Any other melody
    // commit, undo, redo and every state load forget it (false afterwards, also for a loaded generated melody, and
    // already while a load made off the message thread waits to be applied).
    bool isMelodyFromGenerator() const;
    // A generator setting changed: applies a pending load first, then, only while isMelodyFromGenerator, regenerates
    // with getLastGeneratorSeed (generateMelody: one undo step). false when the melody is not the untouched result.
    bool regenerateLastIdea (const cb::MelodyGenSettings&);
    std::uint32_t getLastGeneratorSeed() const;   // seed of the last successful generateMelody; 0 before one (not saved)
    // voicings = getProgressionVoicings(); accompaniment = one loop of what plays besides the melody: the chords, the
    // bass when isLayerOn (Bass) (custom notes included), the arp when arpOn.
    cb::MelodyGenContext getMelodyGenContext() const;

    //==============================================================================
    // Auditioning (hand-off to the audio thread is lock-free). Preview notes sound through the built-in synth
    // (if previewOn) and are sent as MIDI out (if midiOut), chords on channel 1.
    void startPreviewChord (const cb::Chord&);   // stops any sounding preview first; voice-led from the previous preview
    void startPreviewSlot (int slotIndex);       // uses the slot's progression voicing
    void startPreviewNote (int midiNote);        // single note (keyboard clicks)
    // (Added with the melody roll.) Single note on the melody channel (3): drawn / dragged melody notes and roll key
    // clicks. It counts as the melody part for midiOutPart, so a "Melody only" instance passes it on. Stopped by
    // stopPreview() like every preview; getPreviewChord() stays empty.
    void startPreviewMelodyNote (int midiNote);
    void stopPreview();                          // note-offs for whatever preview is sounding
    std::optional<cb::Chord> getPreviewChord() const;   // chord currently being previewed (chord/slot previews), if any
    std::vector<int> getPreviewNotes() const;           // MIDI notes of the preview currently held (empty when none). Added by the integrator.
    // (Added in round 3.) Changes with every preview start and stop. A timed audition (e.g. "play the new chord for
    // 0.6 s") reads it right after starting and stops the preview later only if it is still unchanged, so it never cuts
    // a newer audition.
    uint32_t getPreviewSerial() const;

    //==============================================================================
    // Transport
    void setLocalPlayback (bool shouldPlay);     // loop the progression without the host (standalone / host stopped)
    bool isLocalPlaybackOn() const noexcept;     // any thread
    bool isHostPlaying() const noexcept;         // any thread
    int getPlayingSlot() const noexcept;         // any thread: slot index sounding in playback, -1 when not playing
    double getPlaybackBeat() const noexcept;     // any thread: position inside the loop in beats (valid while playing)
    double getHostBpm() const noexcept;          // any thread: last host tempo, 120 when unknown

    // Any thread (added in round 2). While the host plays, a backward host jump onto the loop start / a bar line by a
    // whole number of bars shorter than the progression (e.g. FL Studio's pattern mode with a 1-bar pattern under a
    // 4-bar progression) is treated as a host loop: the progression keeps running across it instead of restarting.
    // Returns that host loop length in beats while such loops are being seen (0 = none since the transport started,
    // or since the last relocation: a backward jump the progression does not continue across clears it).
    // The lane can show "FL pattern is N bars, progression is M bars".
    double getHostLoopBeats() const noexcept;

    //==============================================================================
    // Live display (any thread)
    std::bitset<128> getSoundingNotes() const noexcept;   // notes currently on from any source (preview, playback, live input)
    int getTriggeredDegree() const noexcept;              // chord-trigger mode: degree 0..6 being held, -1 when none
    // (Added in round 2, B26.) Message thread: the exact MIDI notes the held chord-trigger key is playing (voice-led and
    // octave-shifted as sent), ascending; empty when no chord-trigger key is held.
    std::vector<int> getTriggeredNotes() const;

    //==============================================================================
    // MIDI export
    // Melody (added with the melody roll): one loop of renderMelodyEvents() as a one-track file ("Chordbloom Melody",
    // channel 3). Both stays chords + bass.
    // (Phase 2: Arp and All appended.) Arp = one loop of renderLayerEvents (Arp) as one track "Chordbloom Arp"
    // (channel 4), even while the arp is muted. All = one named track per part that plays (Chords; then Bass, Melody,
    // Arp when isLayerPlaying). Bass and Both use the bass layer's content (custom notes when custom, independent of
    // bassOn); Both is otherwise unchanged. File names "... - Arp.mid" / "... - All parts.mid".
    enum class ExportPart { Chords, Bass, Both, Melody, Arp, All };
    // Writes one loop of the progression at the host tempo into <temp>/Chordbloom/, file name like
    // "Chordbloom - C Major - Chords.mid". Returns the file, or File() when the progression is empty / on failure
    // (Melody: also when no melody note starts inside the loop; phase 2: any part without notes).
    juce::File writeMidiToTempFile (ExportPart) const;
    bool saveMidiFile (const juce::File& destination, ExportPart) const;

    // (Added in round 2.) The file name writeMidiToTempFile uses, built from the progression, e.g.
    // "Am F C G - A minor - Chords.mid" (at most 60 characters, '#' kept, no characters Windows forbids). Use it as
    // the default name of a Save dialog too. "Chordbloom - <key> - <part>.mid" when the progression is empty.
    juce::String getExportFileName (ExportPart) const;

    // Single-chord drags (pad or lane block dragged straight out of the plugin into FL). Writes one chord as a short
    // clip into <temp>/Chordbloom/ using the current voicing/render settings; returns File() on failure.
    juce::File writeChordMidiToTempFile (const cb::Chord&) const;   // one 4-beat chord, voiced like a pad preview
    juce::File writeSlotMidiToTempFile (int slotIndex) const;        // that slot's events (its voicing), shifted to beat 0

    //==============================================================================
    // Transpose every slot's root by `semitones` (undoable, sendChangeMessage), and the melody with it
    // (cb::transposeMelody, same undo step; phase 2: custom layers too, inside their cb::layerLimits). Used by the lane's "Move chords to key" pill and its Transpose menu. bestTranspositionForKey() = the shift in -5..+6 that makes the most slots diatonic
    // in the current key (0 when the current placement is already best or the progression is empty).
    void transposeProgression (int semitones);
    int bestTranspositionForKey() const;

    //==============================================================================
    // Editor size persistence (stored in the plugin state)
    juce::Point<int> getSavedEditorSize() const;   // {0,0} when never saved
    void setSavedEditorSize (juce::Point<int>);

    //==============================================================================
    // User defaults (added in round 2): a freshly created instance starts with the key, scale and preview tone the
    // user last picked in the editor (stored in <user app data>/Chordbloom/Chordbloom Defaults.settings). A loaded
    // project state always wins. Test hook: redirect the settings file; File() disables the feature. The plugin uses
    // the default location; the dev tool (CHORDBLOOM_DEV_TOOL) has it disabled unless a file is set here.
    static void setUserDefaultsFile (const juce::File&);

    // (Added by the integrator, round 2.) Message thread: the user just picked the key, scale or preview tone in the
    // editor's menus - remember the resulting key / scale / tone as the defaults (written ~0.3 s later, once the pick
    // has reached the parameters). Nothing else records defaults: host automation, presets, state loads, undo and
    // validators that move parameters while the editor is open are ignored.
    void rememberUserDefaults();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChordbloomProcessor)
};
