# Chordbloom: architecture & team contract

Chordbloom is a music-theory MIDI plugin (VST3 + Standalone, JUCE 8, C++17), inspired by tools like Klimper.
It helps producers find chord progressions: a palette of chords for the chosen key (diatonic, colour,
borrowed and secondary dominants), suggestions for which chord to play next, a progression timeline with rhythm
patterns, a bass line and voice leading, a built-in preview sound, and drag-and-drop of the result as MIDI
into the DAW. Main target host: **FL Studio 2026 on Windows at 1920x1080**.

## Repository layout

```
CMakeLists.txt              targets: Chordbloom (VST3, Standalone), CoreTests, ChordbloomDev
scripts/build.bat           scripts\build.bat <buildDir> <targets...>   (Ninja + MSVC 2022 x64 Release)
src/core/                   pure C++17 music theory, no JUCE, unit-tested by CoreTests
  Theory.h/.cpp             pitch classes, scales, keys, spelling, chord qualities, roman numerals, functions
  Palette.h/.cpp            palette sections for a key
  Suggest.h/.cpp            next-chord transition scores
  Progression.h/.cpp        progression model (done)
  Voicing.h/.cpp            voicing styles, inversions, voice leading
  Pattern.h/.cpp            rhythm patterns + bass -> NoteEvents (beats)
  Generate.h/.cpp           Spark (mood-based progressions) and presets
  Melody.h/.cpp             melody model, harmonic-guide tone roles, pitch/grid snapping, melody render, follow-slots,
                            NoteLimits overloads (phase 2: the same functions for the arp / bass ranges and caps)
  Arp.h/.cpp                (phase 2) arp shapes, rhythm masks, renderArp
  Layers.h/.cpp             (phase 2) cb::Layer, layer limits / channels, LayerState, auto notes, follow + remap
  NoteEdit.h/.cpp           (phase 2) pure roll edits on a note list + selection (move, resize, duplicate, clipboard..)
  MelodyGen.h/.cpp          (phase 2) melody generator + analyseMelody; MelodyGenTables.h = its private tuning tables
src/plugin/                 JUCE audio side
  PluginProcessor.h/.cpp    ChordbloomProcessor (public API = contract for the UI; private Impl is free)
  PreviewSynth.h/.cpp       built-in preview instrument
  MidiExport.h/.cpp         NoteEvents -> juce::MidiFile (phase 2: buildTracksMidiFile, one named track per part)
src/ui/                     JUCE editor
  Theme.h/.cpp              colours, Inter fonts, LookAndFeel
  DragPayload.h             internal drag descriptions (done)
  PluginEditor.h/.cpp       layout + coordination (Chords | Melody view switch; phase 2: "Chords | Roll")
  HeaderBar, ChordPaletteView, ProgressionLane, ControlsPanel, KeyboardGuide, ExportPanel
  MelodyRoll                "harmonic guide" piano roll of the note layers (shown in place of the palette)
  RollToolbar               (phase 2) the roll's toolbar row: layer tabs + LEDs, Generate, pattern browser, Snap/Grid
tests/                      CoreTests (tiny framework in TestFramework.h); BassGoldenTests.cpp = frozen golden hashes
tools/dev/                  ChordbloomDev: `selftest` (processor integration), `snapshot <dir>` (editor PNGs),
                            `demos <dir>` (phase 2 listening demos). SelfTestUtil.h = shared selftest helpers;
                            SelfTest.cpp (legacy), SelfTestEngine.cpp, SelfTestRoll.cpp, SelfTestShell.cpp
resources/fonts             Inter 400/600/700 (embedded via juce_add_binary_data -> ChordbloomBinary.h,
                            symbols ChordbloomBinary::Inter400_ttf / Inter600_ttf / Inter700_ttf)
resources/images            optional PNGs (embedded the same way, e.g. ChordbloomBinary::logo_png)
third_party/JUCE            JUCE 8.0.16 (gitignored)
tools/pluginval             pluginval.exe 1.0.4 (gitignored)
```

## Rules for everyone

1. **Headers are contracts.** The public parts of the headers in `src/core`, `src/plugin/PluginProcessor.h` and
   `src/ui/*.h` were written up front so modules can be built in parallel. Implement them as specified. You may
   *add* things (private members, helpers, new public methods) but do not change or remove existing public
   signatures. If a contract is genuinely wrong, make the smallest fix and mention it in your final report.
2. **Only edit the files you own** (your task lists them). Everyone else is editing other files at the same time.
3. **Build in your own build directory** (`build-<yourname>`), e.g.
   `cmd //c "scripts\\build.bat build-theory CoreTests"` from Git Bash in the repo root. The first JUCE build of a
   directory takes a few minutes. If the build fails in a file you do not own, it is someone else's work in
   progress: re-run later, or check your own files compile by building just their objects, e.g.
   `cmake --build build-ui --target src/ui/CMakeFiles/...` (see `ninja -C build-ui -t targets all | grep <file>`).
4. **Audio thread**: no allocation, no locks (try-lock at most), no calls into `src/core`, no logging, no
   `juce::String` building. Pre-render on the message thread and hand over lock-free.
5. **Determinism**: core functions are pure and deterministic (no rand, no time).
6. Code style: JUCE-like (4-space indent, braces on new lines, `camelCase` methods, `Type& name`), C++17,
   warnings clean under MSVC `/W4` where practical.
7. Never commit; the coordinator commits.
8. **Append-only enums and choice lists.** Enum values and parameter choice lists that reach a project (BassMode,
   ArpShape, ArpRhythm, MelodyStyle, MelodyContour, ExportPart, ChordQuality, midiOutPart, ...) only ever grow at the
   end. `cb::Layer` is stored as its key string ("melody" / "arp" / "bass"), never as an int. Every switch over these
   enums handles every value (MSVC C4062 at /W4).

## Phase 2 work packages (generators and layers)

The binding spec is `out/phase2/final-spec.md` (section 9 lists the packages). WP0 (coordinator) wrote every phase-2
header, compiling stubs, the parameters, the selftest split and the golden hashes; after that these files are frozen
for the work packages: the headers of final-spec sections 4, 6.1, 6.4, 6.5 and 7.13, `CMakeLists.txt`,
`tools/dev/SelfTestUtil.h`, `tools/dev/DevMain.cpp`, `tests/BassGoldenTests.cpp` and this file (exception: WP6 may
append private members to `src/ui/PluginEditor.h`). A contract that is genuinely wrong goes to the coordinator
(smallest fix, applied to every worktree). File ownership:

| WP | Owns |
|---|---|
| WP1 melody generator | `src/core/MelodyGen.cpp`, `src/core/MelodyGenTables.h`, `tests/MelodyGenTests.cpp` |
| WP2 bass patterns, groove, arp | `src/core/Pattern.cpp`, `src/core/Arp.cpp`, `tests/BassPatternTests.cpp`, `tests/ArpTests.cpp` |
| WP3 layers, follow, note edits | `src/core/Melody.cpp`, `src/core/Layers.cpp`, `src/core/NoteEdit.cpp`, `tests/LayersTests.cpp`, `tests/NoteEditTests.cpp` |
| WP4 processor, export, synth | `src/plugin/PluginProcessor.cpp`, `src/plugin/MidiExport.cpp`, `src/plugin/PreviewSynth.cpp`, `tools/dev/SelfTest.cpp`, `tools/dev/SelfTestEngine.cpp` |
| WP5 roll canvas | `src/ui/MelodyRoll.cpp`, `tools/dev/SelfTestRoll.cpp` |
| WP6 toolbar, shell, docs | `src/ui/RollToolbar.cpp`, `src/ui/PluginEditor.cpp`, `src/ui/ExportPanel.cpp`, `src/ui/HeaderBar.cpp`, `src/ui/ControlsPanel.cpp`, `tools/dev/Snapshot.cpp`, `tools/dev/Demos.cpp`, `tools/dev/SelfTestShell.cpp`, `README.md` |

Merge order WP2, WP3, WP1, then WP4, WP5, WP6; the full gate (0 warnings, CoreTests, selftest, pluginval 10,
snapshots) runs after every merge. Functions marked "WP0 STUB" in a .cpp are placeholders that keep the plugin behaving
exactly like phase 1.5 until their owner implements them; `tests/BassGoldenTests.cpp` (classic bass modes and chord
patterns bit-identical) must stay green unchanged.

## Threading model (processor)

- UI calls processor API on the message thread.
- Progression edits / parameter changes -> message thread re-voices and re-renders one loop of `NoteEvent`s
  (in beats) and publishes it to the audio thread (release-pool / atomic pointer swap; the audio thread never
  frees). Parameter changes are picked up via an APVTS listener or a 20 Hz message-thread timer.
- Preview notes (pads, slots, keyboard clicks) -> lock-free SPSC FIFO -> merged into the block's MIDI at sample 0.
- Playback: when the host is playing and `syncToHost` is on, loop position = host ppq mod loop length; when the
  host is stopped and local playback is on, an internal beat clock at the host tempo (120 when unknown).
  Transport stop / jumps / loop edits -> note-offs for every active note (no hanging notes, ever).
- Everything generated (preview, playback, transformed live input) goes into one MidiBuffer which (a) drives the
  built-in PreviewSynth when `previewOn`, and (b) is the plugin's MIDI output when `midiOut`.
  Chords on MIDI channel 1, bass on channel 2, melody on channel 3, arp on channel 4 (phase 2).
- Live MIDI input: passed through; with `scaleLock` notes snap to the nearest scale tone (remember the mapping so
  note-offs match); with `chordTrigger` white keys C..B (any octave) play the diatonic chord of degree 1..7
  (triads, or sevenths with `triggerSevenths`), voiced by the message thread in advance; black keys pass through.

## Melody ("harmonic guide" piano roll)

- Model: `cb::Melody` (Melody.h) = notes with MIDI pitch 36..96, start / length in beats from the start of the
  progression (1/48-beat resolution), velocity. Always stored in canonical form (`sanitiseMelody`). Notes are tied to
  time, not to slots, but structural progression edits move them with their chords (`melodyFollowingSlots`).
- Processor: `getMelody / setMelody / clearMelody`; melody edits share the progression's undo stack (each undo entry is
  progression + melody). The melody is part of the plugin state (`<Melody><Note .../></Melody>`, absent = empty).
  Rendered on the message thread with `cb::renderMelody` into the same EngineData as the chords (channel 3, only notes
  that start inside the loop, clipped at its end) while the `melodyOn` parameter is on. `midiOutPart` gained
  "Melody only" (3) and "Chords + Bass" (4); 0 is now "All parts". Melody auditions (`startPreviewMelodyNote`) play on
  channel 3 and count as the melody part. `ExportPart::Melody` writes a one-track melody file.
- Ghost notes: every processor publishes its melody + loop length to a process-wide registry (src/plugin/GhostRegistry,
  mutex-protected, message thread); `getGhostMelodies()` returns the other instances' melodies for the roll.
- UI: the editor's "Chords | Melody" switch shows `MelodyRoll` in the palette's place. The roll's beat axis matches
  the lane's track exactly (same x range and beats per pixel), so chords and notes line up vertically.

## Layers (phase 2)

- Model (`core/Layers.h`): the roll edits three layers, `cb::Layer { Melody, Arp, Bass }` (tab order). Channels are
  fixed: chords 1, bass 2, melody 3, arp 4; `midiOutPart` 5 "Arp only" (masks `{ 0b1111, 0b0001, 0b0010, 0b0100,
  0b0011, 0b1000 }` over chords / bass / melody / arp). The melody is always user notes (`cb::Melody`). Arp and Bass are
  **auto** (rendered from the chords and their parameters: `cb::renderArp`, the bass of `cb::renderProgression`;
  nothing stored) until a note is edited, then **custom** (`cb::LayerState { custom, notes }`, stored in the plugin
  state as `<Layer id="arp|bass">` and in the undo stack) until Reset. Undo entries are `{ progression, melody, arp,
  bass }`; parameters are never undoable, and custom notes always win over the current parameter values.
- Follow: one function, `cb::layerNotesFollowing`, keeps custom notes on their chords after any progression edit (move
  with slots, remap by chord role on in-place chord changes, auto-fill new slots); the processor applies it in the same
  undo step as the edit.
- NoteLimits rule: arp and bass notes reuse `cb::Melody` with their own range and cap (`cb::layerLimits`: Melody
  36..96 / 512, Arp 28..108 / 2048, Bass 24..72 / 2048). Every arp / bass path (render, sanitise, snap, transpose,
  follow, state parse, paste) uses the `NoteLimits` overloads; the melody-range functions would clamp a bass to 36..96.
- User-pick rule: a real UI pick of a layer's pattern setting (popup / menu result, browser arrow; never `onChange`,
  a parameter listener, automation or a state load) first sets the parameter with a host gesture and then calls
  `processor.beginUserLayerPick (layer)`: a custom layer resets to auto (one undo step whose publish already renders
  the new pattern) and the layer is switched on. Host automation never resets a custom layer. The Controls Bass combo
  builds its own popup in a `showPopup` override (no flag consumed by a later `onChange`).
- Bass on / off: audible bass = `bassOn && bassMode != Off`. `renderProgressionEvents()` stays chords + the bass
  layer's content (independent of `bassOn`), so legacy exports keep their meaning.
- Groove: auto bass patterns and the arp warp time with `swingWarp (beat, grooveGridBeats (rs), swing)` on the
  absolute grid, so every auto layer swings exactly where the chords' swung steps are; the melody and custom notes play
  straight (a custom layer keeps the swing baked into it at 1/48). Bass patterns are fixed 16-step bars from beat 0,
  independent of Rate.
- Threading: unchanged rules. The message thread renders chords, bass, melody and arp into the one EngineData; it also
  keeps per-layer caches of the humanize-free auto display notes (`getLayerNotes`) and bumps an atomic layer serial
  (`getLayerSerial`, readable from any thread) on every re-render (commits, undo / redo, loads, parameter refreshes),
  so it also moves on parameter-only changes; the roll and the editor's 30 Hz timer poll it to refresh the layer UI.
  The state copy for off-thread `getStateInformation` includes the layer states (under the existing state mutex),
  taken in one lock together with the progression and melody.
- Generator: `cb::generateMelody` (pure, seeded, per-stage streams) behind `processor.generateMelody (settings, seed)`
  (one undo step, keeps parked notes). Seeds come from the UI (`juce::Random` only there).

## Visual design

- Default editor size **1100 x 690**, resizable 960x560 .. 1600x1000 (fits inside FL Studio's plugin wrapper on a
  1080p screen). Everything lays out proportionally; no fixed pixel positions that break when resized.
- Dark slate theme (see `Theme.h` colours) that sits next to FL Studio's blue-grey without clashing.
- Font: Inter (embedded). Hierarchy: chord names 15-18 px semibold, labels 11-12 px, section titles 11 px
  uppercase with letter spacing, dim colour.
- Soft rounded rectangles (radius ~8), 1 px outlines at low contrast, subtle vertical gradients, gentle glow for
  active/playing/suggested states. No harsh pure-white or pure-black.
- Function colours: tonic teal, subdominant amber, dominant coral, borrowed lavender, secondary sky, chromatic grey.
  Pads use the colour as a tint and an accent bar, not as a solid fill (except when pressed or playing).
- Interaction feedback everywhere: hover lift, pressed state, playing glow, drag insertion caret.
- Tooltips on everything non-obvious.

## Verification tools

- `build-x\CoreTests.exe [filter]`: core unit tests.
- `build-x\ChordbloomDev_artefacts\Release\ChordbloomDev.exe selftest`: processor integration tests.
- `build-x\ChordbloomDev_artefacts\Release\ChordbloomDev.exe snapshot out\snapshots`: editor PNGs (look at them!).
- `tools\pluginval\pluginval.exe --strictness-level 8 --validate-in-process --skip-gui-tests --validate <path to .vst3>`
- Standalone: `build-x\Chordbloom_artefacts\Release\Standalone\Chordbloom.exe`
