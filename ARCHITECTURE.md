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
src/plugin/                 JUCE audio side
  PluginProcessor.h/.cpp    ChordbloomProcessor (public API = contract for the UI; private Impl is free)
  PreviewSynth.h/.cpp       built-in preview instrument
  MidiExport.h/.cpp         NoteEvents -> juce::MidiFile
src/ui/                     JUCE editor
  Theme.h/.cpp              colours, Inter fonts, LookAndFeel
  DragPayload.h             internal drag descriptions (done)
  PluginEditor.h/.cpp       layout + coordination
  HeaderBar, ChordPaletteView, ProgressionLane, ControlsPanel, KeyboardGuide, ExportPanel
tests/                      CoreTests (tiny framework in TestFramework.h)
tools/dev/                  ChordbloomDev: `selftest` (processor integration) and `snapshot <dir>` (editor PNGs)
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
  Chords on MIDI channel 1, bass on channel 2.
- Live MIDI input: passed through; with `scaleLock` notes snap to the nearest scale tone (remember the mapping so
  note-offs match); with `chordTrigger` white keys C..B (any octave) play the diatonic chord of degree 1..7
  (triads, or sevenths with `triggerSevenths`), voiced by the message thread in advance; black keys pass through.

## Visual design

- Default editor size **1100 x 690**, resizable 960x600 .. 1600x1000 (fits inside FL Studio's plugin wrapper on a
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
