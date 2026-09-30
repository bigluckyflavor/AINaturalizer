#pragma once

#include <juce_core/juce_core.h>

class AnalysisEngine;

//==============================================================================
/**
    Background thread that drives the AnalysisEngine. It owns no data of its
    own - it only sequences the engine's worker-thread entry points and honours
    the shutdown/cancel signals. Owned by the engine for the engine's lifetime.
*/
class AnalysisWorker : public juce::Thread
{
public:
    explicit AnalysisWorker (AnalysisEngine& engineToDrive);
    ~AnalysisWorker() override;

    void run() override;

private:
    AnalysisEngine& engine;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AnalysisWorker)
};
