#include "SelfTestUtil.h"

// Engine selftests (owner: WP4; final-spec 8.2 / 9 WP4): layers, arp, bass patterns, state, export, generator,
// PreviewSynth. Registered in runSelfTest (SelfTest.cpp) by WP0.

using namespace cbtest;

namespace
{
//==============================================================================
// WP0 (contract review): getLayerSerial is a real counter from WP0 on. It moves on every re-render: commits, undo /
// redo, state loads and parameter-only changes that the layer UI shows (arpOn here), so the UI never goes stale.
void testLayerSerialContract()
{
    ChordbloomProcessor p;
    pumpMessages (30);
    p.setProgression (makeProgression ({ C_, Am_, F_, G_ }));

    const auto s0 = p.getLayerSerial();
    setParam (p, cb::ParamIDs::arpOn, 1.0f);
    pumpMessages (80);
    const auto s1 = p.getLayerSerial();
    check (s1 != s0, "layer serial: changes after a parameter-only change (arpOn)");

    p.setMelody (makeMelody ({ melodyNote (72, 0.0, 1.0) }));
    const auto s2 = p.getLayerSerial();
    check (s2 != s1, "layer serial: changes synchronously on a melody commit");

    p.undo();
    check (p.getLayerSerial() != s2, "layer serial: changes on undo");

    const auto s3 = p.getLayerSerial();
    juce::MemoryBlock state;
    p.getStateInformation (state);
    p.setStateInformation (state.getData(), (int) state.getSize());
    check (p.getLayerSerial() != s3, "layer serial: changes on a state load");
}
} // namespace

void cbtest::runEngineSelfTests()
{
    testLayerSerialContract();
    // WP4 adds its tests here.
}
