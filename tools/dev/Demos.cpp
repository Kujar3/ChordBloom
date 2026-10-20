#include "core/Arp.h"
#include "core/Melody.h"
#include "core/MelodyGen.h"
#include "core/Pattern.h"
#include "core/Voicing.h"
#include "plugin/MidiExport.h"

#include <juce_core/juce_core.h>

#include <array>
#include <iostream>

// ChordbloomDev demos <outDir> (owner: WP6; final-spec 8.3): 3 built-in progressions x 4 melody styles x seeds 1..3 as
// type-1 MIDI files at 138 BPM (tracks "Chordbloom Chords", "Chordbloom Bass", "Chordbloom Arp", "Chordbloom Melody")
// plus report.txt (one analyseMelody row per file, the per-style means and the listening checklist). Built-in generic
// progressions only, never the corpus; deterministic (no clock, no random), so a second run writes byte-identical files.

namespace
{
using Q = cb::ChordQuality;

struct Demo
{
    juce::String name;
    cb::Key key;
    cb::Progression progression;
    cb::BassMode bass;
};

cb::ProgressionSlot slot (int root, Q quality, int beats, int inversion = -1)
{
    cb::ProgressionSlot s;
    s.chord = { root, quality };
    s.lengthBeats = beats;
    s.inversion = inversion;
    return s;
}

cb::Progression twice (std::initializer_list<cb::ProgressionSlot> slots)
{
    cb::Progression p;

    for (int pass = 0; pass < 2; ++pass)
        for (const auto& s : slots)
            p.slots.push_back (s);

    return p;
}

std::vector<Demo> demoProgressions()
{
    return {
        // (a) A minor i-VI-III-VII at 4 beats, twice (8 bars)
        { "minor", { 9, cb::ScaleType::NaturalMinor },
          twice ({ slot (9, Q::Minor, 4), slot (5, Q::Major, 4), slot (0, Q::Major, 4), slot (7, Q::Major, 4) }),
          cb::BassMode::Offbeat8 },
        // (b) C major I-V-vi-IV at 4 beats, twice
        { "pop", { 0, cb::ScaleType::Major },
          twice ({ slot (0, Q::Major, 4), slot (7, Q::Major, 4), slot (9, Q::Minor, 4), slot (5, Q::Major, 4) }),
          cb::BassMode::Driving8 },
        // (c) G major G D/F# Em C Am7 D7sus4 G/B C at 2 beats, twice
        { "slash", { 7, cb::ScaleType::Major },
          twice ({ slot (7, Q::Major, 2), slot (2, Q::Major, 2, 1), slot (4, Q::Minor, 2), slot (0, Q::Major, 2),
                   slot (9, Q::Minor7, 2), slot (2, Q::Dominant7Sus4, 2), slot (7, Q::Major, 2, 1), slot (0, Q::Major, 2) }),
          cb::BassMode::OctaveBounce8 },
    };
}

std::vector<cb::NoteEvent> onChannel (const std::vector<cb::NoteEvent>& events, int channel)
{
    std::vector<cb::NoteEvent> out;

    for (const auto& e : events)
        if (e.channel == channel)
            out.push_back (e);

    return out;
}

juce::String styleKey (cb::MelodyStyle style) { return juce::String (cb::melodyStyleShortName (style)).toLowerCase(); }
juce::String fixed (double v, int decimals) { return juce::String (v, decimals); }
juce::String pct (double share) { return juce::String (juce::roundToInt (share * 100.0)) + "%"; }

juce::String padRight (const juce::String& s, int width)
{
    return s + juce::String::repeatedString (" ", juce::jmax (0, width - s.length()));
}

struct Column
{
    const char* title;
    int width;
};

constexpr Column kColumns[] = {
    { "file", 28 }, { "notes", 6 }, { "range", 7 }, { "n/bar", 6 }, { "ct", 5 }, { "downCT", 7 }, { "finalCT", 8 },
    { "repeat", 7 }, { "step", 5 }, { "leap", 5 }, { "revLeap", 8 }, { "odd16", 6 }, { "lag4", 5 }, { "clash", 6 },
    { "outCS", 6 }, { "seam", 4 }
};

juce::String row (const juce::StringArray& cells)
{
    juce::String line;

    for (int i = 0; i < cells.size() && i < (int) std::size (kColumns); ++i)
        line << padRight (cells[i], kColumns[i].width) << " ";

    return line.trimEnd();
}

juce::StringArray statCells (const juce::String& name, const cb::MelodyStats& s)
{
    return { name, juce::String (s.notes), juce::String (s.lowest) + "-" + juce::String (s.highest),
             fixed (s.notesPerActiveBar, 2), pct (s.chordToneShare), pct (s.downbeatChordToneShare),
             pct (s.phraseFinalChordToneShare), pct (s.repeatShare), pct (s.stepShareMoving), pct (s.leapShareMoving),
             pct (s.reverseAfterLeap), pct (s.odd16Share), pct (s.rhythmRepeatLag4), pct (s.strongBeatClashShare),
             pct (s.outOfChordScaleShare), juce::String (s.seamInterval) };
}

struct Sum
{
    int files = 0;
    double notes = 0, perBar = 0, ct = 0, downCt = 0, finalCt = 0, repeat = 0, step = 0, leap = 0, reverse = 0,
           odd16 = 0, lag4 = 0, clash = 0, outCs = 0, seam = 0;

    void add (const cb::MelodyStats& s)
    {
        ++files;
        notes += s.notes;
        perBar += s.notesPerActiveBar;
        ct += s.chordToneShare;
        downCt += s.downbeatChordToneShare;
        finalCt += s.phraseFinalChordToneShare;
        repeat += s.repeatShare;
        step += s.stepShareMoving;
        leap += s.leapShareMoving;
        reverse += s.reverseAfterLeap;
        odd16 += s.odd16Share;
        lag4 += s.rhythmRepeatLag4;
        clash += s.strongBeatClashShare;
        outCs += s.outOfChordScaleShare;
        seam += s.seamInterval;
    }
};
} // namespace

int runDemos (const juce::File& outDir)
{
    if (! outDir.createDirectory())
    {
        std::cout << "demos: cannot create " << outDir.getFullPathName() << "\n";
        return 1;
    }

    constexpr double bpm = 138.0;
    int written = 0, failed = 0;

    juce::String report;
    report << "Chordbloom melody generator demos (ChordbloomDev demos, final-spec 8.3)\n"
           << "Every file: type 1, 138 BPM, tracks Chordbloom Chords (Block, gate 90%, Close voicing around C4), Chordbloom "
              "Bass, Chordbloom Arp (Up 3, 1/16, gate 90%) and Chordbloom Melody (the style's default settings, context = "
              "chords + bass + arp).\n"
           << "minor = A minor Am F C G (4 beats each, twice, Offbeat 8ths bass); pop = C major C G Am F (4 beats, twice, "
              "Driving 8ths bass); slash = G major G D/F# Em C Am7 D7sus4 G/B C (2 beats, twice, Octave bounce bass).\n\n"
           << "Columns (analyseMelody): notes, range, notes per active bar, chord tones overall / on downbeats / at phrase "
              "ends, repeated-note intervals, steps and leaps among the moving intervals, reversal after a leap, odd-16th "
              "onsets, lag-4 rhythm repeats, strong-beat clashes, out-of-chord-scale notes, loop seam in semitones.\n\n";

    juce::StringArray header;

    for (const auto& c : kColumns)
        header.add (c.title);

    report << row (header) << "\n";
    std::array<Sum, (size_t) cb::numMelodyStyles> sums {};

    for (const auto& demo : demoProgressions())
    {
        const auto& prog = demo.progression;
        const double total = (double) prog.totalBeats();

        cb::VoicingSettings vs;
        vs.style = cb::VoicingStyle::Close;
        vs.centerNote = 60;
        vs.voiceLeading = true;
        const auto voicings = cb::voiceProgression (prog, vs);

        cb::RenderSettings rs;
        rs.pattern = cb::RhythmPattern::Block;
        rs.rate = cb::RateDivision::Sixteenth;
        rs.bass = demo.bass;
        rs.velocity = 0.75f;
        rs.gate = 0.9f;
        rs.swing = 0.0f;
        rs.humanize = 0.0f;

        const auto rendered = cb::renderProgression (prog, voicings, rs);
        const auto chords = onChannel (rendered, 1);
        const auto bass = onChannel (rendered, 2);

        cb::ArpSettings arpSettings;
        arpSettings.shape = cb::ArpShape::Up3;
        arpSettings.rate = cb::RateDivision::Sixteenth;
        arpSettings.rhythm = cb::ArpRhythm::EveryStep;
        arpSettings.centreNote = vs.centerNote + 5;   // as the processor: voicing centre + 5 + 12 * arpOctave
        arpSettings.gate = 0.9f;
        const auto arp = cb::renderArp (prog, voicings, arpSettings, rs, bass, 4);

        cb::MelodyGenContext context;
        context.voicings = voicings;
        context.accompaniment = chords;
        context.accompaniment.insert (context.accompaniment.end(), bass.begin(), bass.end());
        context.accompaniment.insert (context.accompaniment.end(), arp.begin(), arp.end());

        for (int st = 0; st < cb::numMelodyStyles; ++st)
        {
            cb::MelodyGenSettings settings;
            settings.style = (cb::MelodyStyle) st;

            for (std::uint32_t seed = 1; seed <= 3; ++seed)
            {
                const auto melody = cb::generateMelody (prog, demo.key, context, seed, settings);
                const auto stem = "demo_" + demo.name + "_" + styleKey (settings.style) + "_s" + juce::String ((int) seed);

                const std::vector<cb::midiexport::ExportTrack> tracks = {
                    { "Chordbloom Chords", chords },
                    { "Chordbloom Bass", bass },
                    { "Chordbloom Arp", arp },
                    { "Chordbloom Melody", cb::renderMelody (melody, total, 3) },
                };

                const auto file = outDir.getChildFile (stem + ".mid");
                const bool ok = cb::midiexport::writeMidiFile (cb::midiexport::buildTracksMidiFile (tracks, bpm, total), file);

                if (ok)
                {
                    ++written;
                }
                else
                {
                    ++failed;
                    std::cout << "demos: FAILED to write " << file.getFullPathName() << "\n";
                }

                const auto stats = cb::analyseMelody (melody, prog, demo.key, context.accompaniment);
                report << row (statCells (stem, stats)) << "\n";
                sums[(size_t) st].add (stats);
            }
        }
    }

    report << "\nPer-style means\n" << row (header) << "\n";

    for (int st = 0; st < cb::numMelodyStyles; ++st)
    {
        const auto& sum = sums[(size_t) st];
        const double n = juce::jmax (1, sum.files);
        report << row ({ juce::String (cb::melodyStyleShortName ((cb::MelodyStyle) st)) + " (" + juce::String (sum.files) + ")",
                         fixed (sum.notes / n, 1), "", fixed (sum.perBar / n, 2), pct (sum.ct / n), pct (sum.downCt / n),
                         pct (sum.finalCt / n), pct (sum.repeat / n), pct (sum.step / n), pct (sum.leap / n),
                         pct (sum.reverse / n), pct (sum.odd16 / n), pct (sum.lag4 / n), pct (sum.clash / n),
                         pct (sum.outCs / n), fixed (sum.seam / n, 1) })
               << "\n";
    }

    report << "\nListening checklist (in FL, every file): a recognisable motif? phrase ends as question and answer? sour "
              "notes against pad or arp? bass and arp locked? smooth loop seam?\n";

    const auto reportFile = outDir.getChildFile ("report.txt");
    const bool reportOk = reportFile.replaceWithText (report, false, false, "\n");

    std::cout << "demos: wrote " << written << " MIDI files" << (failed > 0 ? " (" + juce::String (failed) + " failed)" : juce::String())
              << " and " << (reportOk ? "report.txt" : "NO report.txt") << " to " << outDir.getFullPathName() << "\n";
    return failed == 0 && reportOk && written == 36 ? 0 : 1;
}
