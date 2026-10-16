# Chordbloom

A chord-progression plugin for producers, inspired by Klimper. Pick a key and you get a palette of chords that fit it:
plain, seventh, colour, borrowed and secondary-dominant chords. Chordbloom glows on the chords that sound good next,
helps you build a progression with rhythm, bass and voice leading, plays it with a built-in sound, and lets you drag the
result into FL Studio as MIDI.

VST3 + CLAP + Standalone, Windows, built with JUCE 8.

![Chordbloom](out/snapshots-final/03b_f_minor_extended.png)

## Install

1. Build it, or use the build that's already in `build\` (see *Building* below).
2. Copy it into the system plugin folders. Run this from the `Klimper-Like` folder in PowerShell; it asks for admin once:

   ```
   powershell -ExecutionPolicy Bypass -File scripts\install-plugin.ps1
   ```

   This copies `Chordbloom.vst3` to `C:\Program Files\Common Files\VST3\` and `Chordbloom.clap` to
   `C:\Program Files\Common Files\CLAP\`.
   No admin? Copies already sit in `%LOCALAPPDATA%\Programs\Common\VST3` and `...\CLAP`. Add those folders in FL's
   *Options > Manage plugins > Plugin search paths*.
3. In FL Studio: *Options > Manage plugins > Find installed plugins*, then add **Chordbloom** from the Channel Rack `+`
   menu. Use the VST3 by default. If MIDI out to other synths doesn't work, try the CLAP version (see below).

## Using it in FL Studio

The **?** button in the top-right corner shows the same instructions inside the plugin.

1. **Hear it right away.** Click a pad to hear it, double-click it (or drag it) into the progression, and press Play.
   The speaker icon, tone menu and volume control the built-in sound (Soft Keys, Warm Pad, Glass Pluck).
2. **Play it on your own synth (MIDI out).** In Chordbloom's FL wrapper, open *Settings (gear) > MIDI* and set *Output
   port* to a number, e.g. 1. In your synth's wrapper, set *MIDI > Input port* to the same number. Then click
   Chordbloom's speaker icon to turn its own sound off.
   *MIDI out part* (next to the MIDI out switch) sends Chords + Bass, Chords only or Bass only (channels 1 and 2). To put
   the bass on a separate synth, clone the Chordbloom channel and set the clone to *Bass*.
   I couldn't test whether FL forwards VST3 MIDI output on your machine. If nothing arrives, load the **CLAP** version,
   or use option 3.
3. **Drag it into FL.** Drag the **Chords / Bass / Both** chips (bottom right) onto the playlist, a channel or the piano
   roll. You can also drag a single pad or a single progression block straight out of the plugin.
   *Both* is a two-track file (chords and bass), so it is best dropped on the playlist.

**Play with FL** (top bar) loops the progression in time with FL's transport. Turn it off once the MIDI is in your
project, otherwise you'll hear the chords twice. Chordbloom shows a reminder after a drag.

## What's inside

- **Palette**: 5 rows (Triads, Sevenths, Color, Borrowed, Secondary) for 12 keys x 9 scales (major, natural, harmonic and
  melodic minor, and the modes). Colours show harmonic function: teal = tonic, amber = subdominant, coral = dominant,
  lavender = borrowed, blue = secondary dominant. Hover a pad for a short explanation.
  Extended colours: add9, 6, 6/9, maj9, m9, m11, maj7#11, 13, 7b9, sus.
- **Suggestions**: pads glow when they make a good next chord after the selected or last chord. The dots mark the top
  picks. With an empty progression, the glow shows good opening chords. The weights blend functional harmony with
  statistics from 349 modern producer MIDI files (minor-key aeolian motion, heavy use of colour chords, borrowed
  bVI/bVII in major).
- **Spark and Presets** (progression header): *Spark* writes a new progression in a chosen mood (Uplifting, Emotional,
  Dark, Dreamy, Epic, Lo-fi), and every click gives a different one. *Presets* lists well-known progressions resolved in
  your key. Both are undoable.
- **Progression lane**:
  - Drag pads in. Drop a pad on the middle of a block to replace that chord, or near an edge to insert.
  - Drag blocks to reorder them, scroll on a block to change its length, or drag its right edge.
  - Right-click a block for *Change to*, *Suggested here*, length, inversion (slash chords), octave, transpose,
    duplicate and delete.
  - When the chords fit another key better, *Move chords to <key>* appears.
- **Rhythm and voicing**:
  - Patterns: Block, Strum, Pulse, Offbeat, three arps and Broken, with rate, swing, gate, velocity and humanize.
  - Voicings: Close, Open (drop-2) and Spread, with automatic voice leading.
  - Bass line: Root, Root + 5th, Octaves or Pulse. It is voice-led and respects slash chords.
- **Live MIDI input**: *Scale lock* snaps your notes to the key. *Chord trigger* turns the white keys C–B into the key's
  chords I–VII (or 7ths).
- **Keyboard guide**: highlights scale tones, the current chord's notes (spelled correctly) and whatever is sounding.

## Building

You need Visual Studio 2022 with *Desktop development with C++*, plus CMake and Ninja (`pip install cmake ninja`).
JUCE 8.0.16 goes in `third_party/JUCE`:

```
git clone --depth 1 --branch 8.0.16-security https://github.com/juce-framework/JUCE.git third_party/JUCE
scripts\fetch_clap.bat
scripts\build.bat build Chordbloom_VST3 Chordbloom_CLAP Chordbloom_Standalone CoreTests ChordbloomDev
```

`fetch_clap.bat` adds the optional CLAP build. Then run the checks:

- `build\CoreTests.exe`: music-theory, voicing and generator unit tests
- `build\ChordbloomDev_artefacts\Release\ChordbloomDev.exe selftest`: processor and editor integration tests
- `build\ChordbloomDev_artefacts\Release\ChordbloomDev.exe snapshot out\snaps`: renders the UI to PNGs
- `tools\pluginval\pluginval.exe --strictness-level 10 --validate build\Chordbloom_artefacts\Release\VST3\Chordbloom.vst3`

See `ARCHITECTURE.md` for how the code is organised.

## Licences

- JUCE 8: AGPLv3 or the JUCE licence. Fine for personal use; check the JUCE terms before you distribute builds.
- Inter font: SIL Open Font License 1.1.
- clap-juce-extensions: MIT.
- The logo was generated for this project.
