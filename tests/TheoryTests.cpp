#include "TestFramework.h"

#include "core/Palette.h"
#include "core/Suggest.h"
#include "core/Theory.h"

// Tests for Theory / Palette / Suggest (owner: core-theory agent)

TEST_CASE ("theory: placeholder")
{
    CHECK (cb::pc (-1) == 11);
}
