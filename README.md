# Chordbloom

A chord-progression plugin for producers, inspired by Klimper. Pick a key and you get a palette of chords that fit it:
plain, seventh, colour, borrowed and secondary-dominant chords. Chordbloom glows on the chords that sound good next,
helps you build a progression with rhythm, bass and voice leading, adds bass patterns and an arp that follow the chords,
writes (or lets you draw) a melody over it in a piano roll that lights up the notes that fit, plays it all with a
built-in sound, and lets you drag the result into FL Studio as MIDI.

VST3 + CLAP + Standalone, Windows, built with JUCE 8.

![Chordbloom](docs/screenshot.png)

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
   *MIDI out part* (next to the MIDI out switch) sends *All* parts (chords on channel 1, bass on 2, melody on 3, arp on
   4), *Chords + Bass*, or the chords, the bass, the melody or the arp alone (*Arp only*). To put a part on a separate
   synth, clone the Chordbloom channel and set the clone's part (for example *Arp only* into a pluck, *Bass only* into
   your bass synth).
   I couldn't test whether FL forwards VST3 MIDI output on your machine. If nothing arrives, load the **CLAP** version,
   or use option 3.
3. **Drag it into FL.** Drag the **Chords / Bass / Melody / Arp / All** chips (bottom right) onto the playlist, a
   channel or the piano roll. Each single-part chip is one loop on its own channel (Chords 1, Bass 2, Melody 3, Arp 4);
   the Arp chip exports the arp even while it is muted in Chordbloom. **All** is one file with a named track per part
   that plays (Chords, then Bass, Melody, Arp), so it is best dropped on the playlist. **Save** (the pill above the
   chips) writes the same parts as .mid files, plus *Chords + bass* (the old two-track *Both* file). Right-click a chip
   to save just that part. You can also drag a single pad or a single progression block straight out of the plugin.
4. **Melody, arp and bass.** Click **Roll** in the switch at the top right of the chord palette. See *Roll* below.

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
  - Bass line (the *Bass* menu, or the Roll's Bass tab): *Classic* modes Off, Root, Root + 5th, Octaves and Pulse
    (Octaves and Pulse follow Rate), and ten *patterns* (fixed 16th-note grooves that ignore Rate): Offbeat 8ths, Rolling
    16ths, Rolling kick gap, Offbeat pairs and Gallop (trance), Octave bounce, Driving 8ths, Off-kick, Tresillo 3-3-2 and
    Dotted quarters. Every mode is voice-led, stays low (MIDI 24-55) and plays the root or the slash-chord bass note.
  - Arp (off by default; the Roll's Arp tab): 17 shapes in three families (ladders such as Up 3 / Up-down / 1-3-5-7,
    pedal and octave bounces, two-note alternations), rate 1/4 / 1/8 / 1/16 / 1/8T, rhythm masks (3 on 1 off, kick gap,
    breath, late start ...), gate (Pluck 45% to Legato 100%) and octave. It follows each chord's inversion, keeps clear
    of the bass and plays on MIDI channel 4.
  - One groove: the arp and the bass patterns swing on the chords' Rate grid, so they never flam against swung chord
    steps (Rate stays lit whenever the arp or a bass pattern is on, and Swing too except at 1/8T).
- **Live MIDI input**: *Scale lock* snaps your notes to the key. *Chord trigger* turns the white keys C–B into the key's
  chords I–VII (or 7ths).
- **Keyboard guide**: highlights scale tones, the current chord's notes (spelled correctly) and whatever is sounding.
- **Roll** (the "harmonic guide" piano roll, *Chords | Roll* switch at the top right of the palette; a dot after
  *Roll* says it holds your notes):
  - Three layers, one tab each: **Melody** (channel 3, your lead), **Arp** (channel 4) and **Bass** (channel 2). The dot
    on a tab plays or mutes that layer (lit = you hear it). The roll sits right above the progression and its beats line
    up with the chord blocks. Each row is a note: the key's notes are lighter, and over each chord its tones light up in
    the chord's colour (the root more strongly). The other layers show as faint outlines while they play (toggle:
    *Show other layers and instances* in the menu).
  - **Generate** (Melody tab) writes a melody over the chords: strong beats on chord tones, steps and passing notes in
    between, a motif that repeats and varies, phrase ends that ask and answer. Every click gives a new one; Undo brings
    the last one back. The menu next to it (it shows the style) sets *Style* (Uplifting lead, Lyrical hook, Driving riff,
    Sparse), *Density*, *Range*, *Contour*, *Repetition* and *End on the home note*. Changing a setting re-generates the
    same idea while the melody is untouched; once you edited it, settings only apply to the next Generate (or *Generate
    again (same idea)*). An empty Melody tab shows a *Generate a melody* card.
  - **Pattern browser** (Arp and Bass tabs): the arrows step through the patterns as you listen; the name opens the
    menu with every pattern by family (and the arp's rate, rhythm, gate and octave).
  - **Auto and edited layers**: the arp and the bass follow the chords and their pattern until you edit a note. Then the
    layer is *edited* (a pencil on the tab and next to the Controls' Bass caption, a dot on each edited chord): your notes
    play instead, and they still follow chord edits (moved with their chords, re-mapped by chord role when a chord
    changes, new chords filled from the pattern). **Reset** makes the layer follow the pattern again; picking another
    pattern (browser, menu or the Controls' Bass menu) resets it too, and Undo brings your notes back. Host automation
    of the pattern settings never touches edited notes.
  - Click to draw a note (new notes take the length and velocity of the last one you touched), drag it to move it, drag
    its right edge to resize it, right-click it to delete it (a right-drag deletes every note it crosses). **Ctrl**+drag
    draws a selection box (Ctrl+Shift adds to it), **Ctrl**+click adds or removes a note, **Shift**+drag duplicates,
    dragging a selected note moves the whole selection. Hold **Alt** for no grid. The mouse wheel scrolls up and down.
  - The *N selected* pill and the **...** menu: select all / none, copy, cut, paste (also between layers and
    instances), duplicate, delete, velocity presets, transpose by scale step / semitone / octave, quantize, legato,
    Snap, Grid, the velocity lane (drag a stalk to set a velocity) and the layer's play / reset items. Right-click a tab
    for that layer's items.
  - *Snap* keeps notes on the harmony: *Scale* (the key's notes, plus the chord's own tones, e.g. G# over E7 in A minor),
    *Chord* (only the tones of the chord under the note) or *Off*. *Grid* sets the step (1/4, 1/8, 1/16, 1/8T). Each
    layer remembers its own Snap, Grid and pitch range.
  - Everything is saved with the project, and Undo / Redo cover chords and notes together. Reordering, inserting,
    duplicating, deleting or resizing chords moves the notes with them; notes past the end of the progression are kept
    (shown dimmed) but don't play.
  - **Ghost notes**: with several Chordbloom instances in one project, each roll shows the others' melodies as faint
    outlines, so a lead and a counter-melody don't collide.

### Good to know (phase 2)

- **Timing of edited layers.** An edited arp or bass keeps the timing it had when you first edited it, rounded to 1/48
  of a beat and played straight: it keeps the swing it had then and no longer follows the Swing knob (Reset brings the
  live pattern back). Its swung notes can sit up to half a tick (about 4.5 ms at 138 BPM) off the chords' exact swung
  steps. The melody always plays exactly as drawn (never swung).
- **Old projects** sound the same. Two parameters gained choices at the end of their lists: *Bass* (10 patterns) and
  *MIDI out part* (*Arp only*). Saved values are safe, but FL automation clips or linked controllers of those two
  parameters move by their normalised value, so in old projects they can land on other choices: re-draw such clips.
  An older Chordbloom build opening a newer project plays a bass pattern as *Pulse* and ignores the arp.
- **Bass on / off**: the Bass tab's dot mutes the bass without changing its mode; the mode *Off* also silences it (old
  automation keeps working). The Bass chip exports the bass even while it is muted, unless the mode is *Off*.
- `ChordbloomDev demos <dir>` writes 36 multi-track listening demos (3 progressions x 4 styles x 3 seeds, 138 BPM) plus
  `report.txt` with melody statistics, for checking the generator by ear in FL.

## Building

You need Visual Studio 2022 with *Desktop development with C++*, plus CMake and Ninja (`pip install cmake ninja`).
JUCE 8.0.16 goes in `third_party/JUCE`:

```
git clone --depth 1 --branch 8.0.16-security https://github.com/juce-framework/JUCE.git third_party/JUCE
scripts\fetch_clap.bat
scripts\build.bat build Chordbloom_VST3 Chordbloom_CLAP Chordbloom_Standalone CoreTests ChordbloomDev
```

`fetch_clap.bat` adds the optional CLAP build. Then run the checks:

- `build\CoreTests.exe`: music-theory, voicing, generator and melody unit tests
- `build\ChordbloomDev_artefacts\Release\ChordbloomDev.exe selftest`: processor and editor integration tests
- `build\ChordbloomDev_artefacts\Release\ChordbloomDev.exe snapshot out\snaps`: renders the UI to PNGs
- `build\ChordbloomDev_artefacts\Release\ChordbloomDev.exe demos out\demos`: the melody generator's listening demos
- `tools\pluginval\pluginval.exe --strictness-level 10 --validate build\Chordbloom_artefacts\Release\VST3\Chordbloom.vst3`

See `ARCHITECTURE.md` for how the code is organised.

## Licences

- JUCE 8: AGPLv3 or the JUCE licence. Fine for personal use; check the JUCE terms before you distribute builds.
- Inter font: SIL Open Font License 1.1.
- clap-juce-extensions: MIT.
- The logo was generated for this project.
