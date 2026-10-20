#include "TestFramework.h"

#include "core/MelodyGen.h"

#include <string>

// Melody generator (owner: WP1; final-spec 4.6, 5.4, 9 WP1). WP0 seeded this file with checks of the frozen names and
// the mood table; WP1 adds the generator tests.

using namespace cb;

TEST_CASE ("melody gen: WP0 names, mood table and settings equality")
{
    CHECK_EQ (std::string (melodyStyleName (MelodyStyle::Uplifting)), std::string ("Uplifting lead"));
    CHECK_EQ (std::string (melodyStyleShortName (MelodyStyle::Driving)), std::string ("Driving"));
    CHECK_EQ (std::string (melodyContourName (MelodyContour::Wave)), std::string ("Wave"));
    CHECK (melodyStyleForMood (Mood::Dark) == MelodyStyle::Driving);
    CHECK (melodyStyleForMood (Mood::Dreamy) == MelodyStyle::Sparse);
    CHECK (melodyStyleForMood (Mood::LoFi) == MelodyStyle::Lyrical);

    MelodyGenSettings a, b;
    CHECK (a == b);
    b.contour = MelodyContour::Arch;
    CHECK (a != b);
}
