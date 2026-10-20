#include "TestFramework.h"

#include "core/Arp.h"

#include <string>

// Arp layer (owner: WP2; final-spec 4.3, 9 WP2). WP0 seeded this file with checks of the frozen shape / rhythm data;
// WP2 adds the rendering tests.

using namespace cb;

TEST_CASE ("arp: WP0 shape and rhythm data")
{
    CHECK_EQ (numArpShapes, 17);
    CHECK_EQ (numArpRhythms, 7);
    CHECK_EQ (std::string (arpShapeName (ArpShape::Up3)), std::string ("Up 3"));
    CHECK_EQ (std::string (arpShapeName (ArpShape::Alternate15)), std::string ("Alternate 1-5"));
    CHECK_EQ (std::string (arpShapeFamily (ArpShape::Broken)), std::string ("Ladders"));
    CHECK_EQ (std::string (arpShapeFamily (ArpShape::RootPulse)), std::string ("Pedal & bounce"));
    CHECK_EQ (std::string (arpShapeFamily (ArpShape::Alternate13)), std::string ("Two-note"));
    CHECK_EQ (arpShapeTones (ArpShape::TwoOctaves).size(), (size_t) 6);
    CHECK (arpShapeTones (ArpShape::Down4).front() == (ArpTone { 0, 1 }));
    CHECK (arpShapeFreeRuns (ArpShape::Down3));
    CHECK (! arpShapeFreeRuns (ArpShape::Up4));

    CHECK (! arpStepSounds (ArpRhythm::ThreeOnOneOff, 3, 16));
    CHECK (arpStepSounds (ArpRhythm::ThreeOnOneOff, 4, 16));
    CHECK (! arpStepSounds (ArpRhythm::Breath, 15, 16));
    CHECK (! arpStepSounds (ArpRhythm::LateStart, 0, 8));
    CHECK_EQ (std::string (arpRhythmName (ArpRhythm::ThreeOnOneOff)), std::string ("3 on, 1 off"));
}
