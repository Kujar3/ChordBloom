#include <juce_core/juce_core.h>

#include <iostream>

// ChordbloomDev demos <outDir> (owner: WP6; final-spec 8.3): 3 built-in progressions x 4 melody styles x seeds 1..3 as
// type-1 MIDI files at 138 BPM (chords, bass, arp, generated melody) plus report.txt (analyseMelody per file and per
// style). Built-in generic progressions only, never the corpus; byte-identical output on every run.
//
// WP0 STUB: writes nothing yet.
int runDemos (const juce::File& outDir)
{
    std::cout << "demos: not implemented yet (WP6); would write to " << outDir.getFullPathName() << "\n";
    return 1;
}
