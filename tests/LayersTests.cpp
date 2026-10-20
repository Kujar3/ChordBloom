#include "TestFramework.h"

#include "core/Layers.h"

#include <string>

// Layers, follow and note limits (owner: WP3; final-spec 4.1, 4.4, 9 WP3). WP0 seeded this file with checks of the
// frozen identity / limit functions; WP3 adds the rest.

using namespace cb;

TEST_CASE ("layers: WP0 identity, channels and limits")
{
    CHECK_EQ (numLayers, 3);
    CHECK_EQ (std::string (layerKey (Layer::Arp)), std::string ("arp"));
    CHECK_EQ (std::string (layerName (Layer::Bass)), std::string ("Bass"));
    CHECK (layerFromKey ("BASS") == Layer::Bass);
    CHECK (! layerFromKey ("chords").has_value());
    CHECK_EQ (layerMidiChannel (Layer::Melody), 3);
    CHECK_EQ (layerMidiChannel (Layer::Arp), 4);
    CHECK_EQ (layerMidiChannel (Layer::Bass), 2);
    CHECK_EQ (layerLimits (Layer::Bass).lowest, 24);
    CHECK_EQ (layerLimits (Layer::Bass).highest, 72);
    CHECK_EQ (layerLimits (Layer::Arp).maxNotes, kMaxLayerNotes);
    CHECK_EQ (layerLimits (Layer::Melody).maxNotes, kMaxMelodyNotes);
}
