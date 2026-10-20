#include "TestFramework.h"

#include "core/Pattern.h"

#include <cmath>
#include <string>

// Bass pattern modes and the shared groove (owner: WP2; final-spec 4.2, 5.1, 9 WP2). WP0 seeded this file with checks
// of the frozen pattern data; WP2 adds the rendering tests.

using namespace cb;

TEST_CASE ("bass patterns: WP0 data matches the spec grids")
{
    CHECK_EQ (numBassModes, 15);
    CHECK_EQ (std::string (bassModeName (BassMode::Pulse)), std::string ("Pulse"));
    CHECK_EQ (std::string (bassModeName (BassMode::DottedQuarter)), std::string ("Dotted quarters"));

    CHECK_EQ (bassPatternGrid (BassMode::Offbeat8), std::string ("--x---x---x---x-"));
    CHECK_EQ (bassPatternGrid (BassMode::Rolling16), std::string ("xxxxxxxxxxxxxxxx"));
    CHECK_EQ (bassPatternGrid (BassMode::RollingKickGap), std::string ("-xox-xox-xox-xox"));
    CHECK_EQ (bassPatternGrid (BassMode::OffbeatPairs), std::string ("--xx--xx--xx--xx"));
    CHECK_EQ (bassPatternGrid (BassMode::Gallop), std::string ("x-xx-xx-x-xx-xx-"));
    CHECK_EQ (bassPatternGrid (BassMode::OctaveBounce8), std::string ("x-o-x-o-x-o-x-o-"));
    CHECK_EQ (bassPatternGrid (BassMode::Driving8), std::string ("x-x-x-x-x-x-x-x-"));
    CHECK_EQ (bassPatternGrid (BassMode::OffKick), std::string ("-x-x--x--x-x--x-"));
    CHECK_EQ (bassPatternGrid (BassMode::Tresillo), std::string ("x--x--x-x--x--x-"));
    CHECK_EQ (bassPatternGrid (BassMode::DottedQuarter), std::string ("x-----x-----x---"));

    for (int m = 0; m < numBassModes; ++m)
    {
        const auto mode = (BassMode) m;
        CHECK_EQ (isBassPattern (mode), m >= 5);
        CHECK_EQ (bassPatternSteps (mode).empty(), m < 5);
        if (bassPatternAvoidsKick (mode))
            for (const auto& s : bassPatternSteps (mode))
                CHECK (s.step % 4 != 0);
    }

    CHECK_EQ (bassModeBrowseOrder().size(), (size_t) 14);
    CHECK_EQ (std::string (bassModeFamily (BassMode::Off)), std::string (""));
    CHECK_EQ (std::string (bassModeFamily (BassMode::Gallop)), std::string ("Trance"));
    CHECK_EQ (std::string (bassModeFamily (BassMode::Tresillo)), std::string ("Groove"));
}

TEST_CASE ("groove: WP0 swingWarp and grooveGridBeats basics")
{
    RenderSettings rs;
    rs.rate = RateDivision::Sixteenth;
    CHECK_EQ (grooveGridBeats (rs), 0.25);
    rs.rate = RateDivision::EighthTriplet;
    CHECK_EQ (grooveGridBeats (rs), 0.0);

    CHECK_EQ (swingWarp (1.25, 0.25, 0.0), 1.25);
    CHECK_EQ (swingWarp (1.0, 0.25, 1.0), 1.0);
    CHECK (std::abs (swingWarp (0.5, 0.5, 0.5) - (0.5 + 0.5 * 0.5 / 3.0)) < 1.0e-12);
}
