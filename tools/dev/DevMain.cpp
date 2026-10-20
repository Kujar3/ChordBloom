#include <juce_gui_basics/juce_gui_basics.h>

#include <iostream>

// ChordbloomDev: developer tool built from the same sources as the plugin.
//   ChordbloomDev selftest            -> runs processor-level integration tests, exit code 0 on success
//   ChordbloomDev snapshot <outDir>   -> renders the editor in several states to PNG files in outDir
//   ChordbloomDev demos <outDir>      -> writes the phase-2 listening demos (MIDI files + report.txt) to outDir

int runSelfTest();                               // tools/dev/SelfTest.cpp
int runSnapshot (const juce::File& outputDir);   // tools/dev/Snapshot.cpp
int runDemos (const juce::File& outDir);         // tools/dev/Demos.cpp

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const juce::String command = argc > 1 ? juce::String (argv[1]) : juce::String();

    if (command == "selftest")
        return runSelfTest();

    if (command == "snapshot")
    {
        const auto dir = argc > 2 ? juce::File::getCurrentWorkingDirectory().getChildFile (argv[2])
                                  : juce::File::getCurrentWorkingDirectory().getChildFile ("snapshots");
        return runSnapshot (dir);
    }

    if (command == "demos")
    {
        const auto dir = argc > 2 ? juce::File::getCurrentWorkingDirectory().getChildFile (argv[2])
                                  : juce::File::getCurrentWorkingDirectory().getChildFile ("demos");
        return runDemos (dir);
    }

    std::cout << "usage: ChordbloomDev selftest | snapshot <outDir> | demos <outDir>\n";
    return 2;
}
