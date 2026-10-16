#include "plugin/PluginProcessor.h"
#include "ui/PluginEditor.h"

#include <iostream>

// Renders the editor to PNGs (owner: ui-shell agent; extend with more states).
namespace
{
bool writePng (juce::Component& c, const juce::File& file)
{
    auto image = c.createComponentSnapshot (c.getLocalBounds(), true, 1.0f);
    file.deleteFile();
    juce::FileOutputStream out (file);
    juce::PNGImageFormat png;
    const bool ok = out.openedOk() && png.writeImageToStream (image, out);
    std::cout << (ok ? "wrote " : "FAILED ") << file.getFullPathName() << "\n";
    return ok;
}
}

int runSnapshot (const juce::File& outputDir)
{
    outputDir.createDirectory();
    bool ok = true;

    ChordbloomProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    editor->setSize (1100, 690);
    ok &= writePng (*editor, outputDir.getChildFile ("editor_default.png"));

    // C major: C - G - Am - F
    cb::Progression prog;
    for (auto chord : { cb::Chord { 0, cb::ChordQuality::Major }, cb::Chord { 7, cb::ChordQuality::Major },
                        cb::Chord { 9, cb::ChordQuality::Minor }, cb::Chord { 5, cb::ChordQuality::Major } })
    {
        cb::ProgressionSlot slot;
        slot.chord = chord;
        prog.slots.push_back (slot);
    }
    processor.setProgression (prog);
    processor.dispatchPendingMessages();   // deliver the ChangeBroadcaster callback synchronously (no message loop here)
    ok &= writePng (*editor, outputDir.getChildFile ("editor_filled.png"));

    editor = nullptr;
    return ok ? 0 : 1;
}
