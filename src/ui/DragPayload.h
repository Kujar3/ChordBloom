#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/Theory.h"

// Internal drag-and-drop descriptions (palette pad -> progression lane, slot reordering inside the lane).
// The editor is the juce::DragAndDropContainer.
namespace cb::ui::drag
{
inline juce::var makeChordPayload (const cb::Chord& chord)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("type", "chord");
    obj->setProperty ("root", chord.root);
    obj->setProperty ("quality", (int) chord.quality);
    return juce::var (obj);
}

inline juce::var makeSlotPayload (int slotIndex)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("type", "slot");
    obj->setProperty ("index", slotIndex);
    return juce::var (obj);
}

inline bool isChordPayload (const juce::var& v) { return v.getProperty ("type", {}).toString() == "chord"; }
inline bool isSlotPayload (const juce::var& v)  { return v.getProperty ("type", {}).toString() == "slot"; }

inline cb::Chord chordFromPayload (const juce::var& v)
{
    cb::Chord c;
    c.root = cb::pc ((int) v.getProperty ("root", 0));
    c.quality = (cb::ChordQuality) juce::jlimit (0, cb::numChordQualities - 1, (int) v.getProperty ("quality", 0));
    return c;
}

inline int slotFromPayload (const juce::var& v) { return (int) v.getProperty ("index", -1); }
}
