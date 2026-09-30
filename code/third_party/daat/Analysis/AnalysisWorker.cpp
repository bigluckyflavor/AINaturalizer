#include "AnalysisWorker.h"
#include "AnalysisEngine.h"

AnalysisWorker::AnalysisWorker (AnalysisEngine& engineToDrive)
    : juce::Thread ("DAAT Analysis Worker"),
      engine (engineToDrive)
{
}

AnalysisWorker::~AnalysisWorker()
{
    // The engine signals + joins us in its destructor; this is a backstop in
    // case a worker is ever destroyed directly.
    stopThread (2000);
}

void AnalysisWorker::run()
{
    auto& wake = engine.getWakeEvent();

    // Abort predicate for cancellable long operations (file load / analysis):
    // stop when the thread is asked to exit.
    const std::function<bool()> exitPredicate = [this] { return threadShouldExit(); };

    while (! threadShouldExit())
    {
        // Poll frequently while capturing so the FIFO stays well below full;
        // idle more slowly otherwise. Commands signal wakeEvent for low latency.
        wake.wait (engine.isCaptureActive() ? 20 : 100);

        if (threadShouldExit())
            break;

        engine.serviceCommands();
        engine.drainCaptureFifo();
        engine.processPendingFileLoad (exitPredicate);
        engine.processPendingAnalysis (exitPredicate);
    }
}
