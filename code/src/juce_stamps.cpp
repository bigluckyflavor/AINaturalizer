// Provides the two compilation-stamp symbols JUCE's own CMake normally
// generates. Needed because we compile the JUCE modules directly instead
// of through juce_add_module.
#include <juce_core/juce_core.h>

namespace juce
{
    const char* juce_compilationDate = __DATE__;
    const char* juce_compilationTime = __TIME__;
}
