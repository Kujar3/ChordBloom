#include "TestFramework.h"

#include "core/Pattern.h"
#include "core/Voicing.h"

// Tests for Voicing / Pattern / Progression (owner: core-voicing agent)

TEST_CASE ("voicing: placeholder")
{
    cb::Progression p;
    p.slots.push_back ({});
    CHECK_EQ (p.totalBeats(), 4);
}
