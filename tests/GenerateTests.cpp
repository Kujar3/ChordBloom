#include "TestFramework.h"

#include "core/Generate.h"
#include "core/Suggest.h"

// Tests for Generate (presets + spark) and Suggest calibration (owner: core-suggest agent)

TEST_CASE ("generate: placeholder")
{
    CHECK (cb::numMoods == 6);
}
