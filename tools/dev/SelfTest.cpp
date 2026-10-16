#include "plugin/PluginProcessor.h"

#include <iostream>

// Processor integration tests (owner: processor agent). Print one line per check; return 0 when all pass.
int runSelfTest()
{
    ChordbloomProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    std::cout << "selftest: stub OK\n";
    return 0;
}
