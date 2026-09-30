#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "../Utility/LockFreeFifo.h"
#include "DetectionProfile.h"
#include "FeatureExtractor.h"
#include "AnalysisResult.h"
#include "DetectionEngine.h"

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

class AnalysisWorker;

//==============================================================================
enum class EngineState
{
    Idle,        // nothing captured, or captured-and-cleared
    Capturing,   // explicit Capture in progress
    Live,        // continuous capture while transport plays
    LoadingFile, // decoding an audio file on the worker thread
    Analyzing,   // extracting features / scoring
    Complete,    // analysis finished, result available
    Failed       // load or analysis failed; see message
};

/** Lightweight status handed to the UI. Copied under a lock; the UI never
    touches engine buffers or the worker thread directly. Detailed results
    live in the AnalysisResult returned by getResult(). */
struct EngineSnapshot
{
    EngineState state       = EngineState::Idle;
    double capturedSeconds  = 0.0;   // length of the whole capture / file
    double sampleRate       = 0.0;   // source sample rate
    int    channels         = 0;
    float  progress         = 0.0f;  // 0..1 for LoadingFile / Analyzing

    bool   hasSummary       = false;
    float  peakLevel        = 0.0f;
    float  rmsDb            = -100.0f;
    bool   clipped          = false;
    bool   truncated        = false;

    double analyzedSeconds  = 0.0;
    int    shortWindows     = 0;
    int    mediumWindows    = 0;
    int    longWindows      = 0;
    int    validFeatures    = 0;
    int    agreeingGroups   = 0;
    int    activeGroups     = 0;
    float  likelihood       = 0.0f;
    float  scoreConfidence  = 0.0f;
    bool   hasScore         = false;
    int    resultGeneration = 0;
    juce::String verdictText;
    juce::String verdictDetail;
    juce::String topIndicator; // strongest single feature

    juce::String message;
    juce::String sourceName;
};

//==============================================================================
/**
    Owns audio capture and the background worker. Lives on the processor, never
    on the editor, so analysis survives editor open/close.

    Threading:
      - Audio thread calls pushAudioBlock() only (wait-free FIFO write).
      - Message thread issues commands and reads getSnapshot() / getResult().
      - A single worker thread drains the FIFO, decodes files, extracts
        features, and scores. It owns all access to the audio buffers.

    Results are immutable shared_ptr<const AnalysisResult> snapshots. Rescoring
    (after a profile, control, or exclusion change) re-evaluates the stored
    per-window raw measurements and never needs the audio again.
*/
class AnalysisEngine
{
public:
    AnalysisEngine();
    ~AnalysisEngine();

    //==========================================================================
    /** Non-audio thread. Allocates the FIFO and accumulation buffer. Safe to
        call repeatedly; a changed rate/channel count reallocates and resets. */
    void prepare (double sampleRate, int blockSize, int numInputChannels);

    /** Non-audio thread. Stops any capture in progress. */
    void release();

    //==========================================================================
    /** Audio thread only. Wait-free; does nothing unless a capture is active. */
    void pushAudioBlock (const juce::AudioBuffer<float>& buffer) noexcept;

    //==========================================================================
    // Message-thread commands.
    void startCapture (bool live);
    void stop();
    void clear();
    void loadFile (const juce::File& file);

    /** Full analysis of the whole capture / file. */
    void requestAnalyze();

    /** Analysis of [startSeconds, endSeconds) of the capture only, in timeline
        seconds. Windows keep timeline times so they line up with the heat map. */
    void requestAnalyzeRange (double startSeconds, double endSeconds);

    /** Re-evaluate and re-score the last result from its stored raw
        measurements with the current profile, controls, and exclusions. */
    void requestRescore();

    /** Replace the profile. Triggers a full reanalysis if the change affects
        raw measurements (analysis block, HF cutoff, repetition lags), otherwise
        a cheap rescore. Returns true if a reanalysis was scheduled. */
    bool applyProfile (const DetectionProfile& newProfile);

    /** Timeline sections excluded from the overall score (spec §14). Ranges
        are sorted and merged. Call requestRescore() to apply. */
    void setExcludedRanges (std::vector<juce::Range<double>> ranges);
    std::vector<juce::Range<double>> getExcludedRanges() const;

    //==========================================================================
    EngineSnapshot getSnapshot() const;

    /** Most recent immutable analysis result, or nullptr if none. */
    std::shared_ptr<const AnalysisResult> getResult() const;
    int getResultGeneration() const noexcept { return resultGeneration.load(); }

    /** File the current audio came from (empty for DAW captures). */
    juce::File getSourceFile() const;

    /** Active detection profile (thread-safe copy / replace, no rescore). */
    DetectionProfile getProfile() const;
    void setProfile (const DetectionProfile& newProfile);

    /** Level-A (APVTS) controls: group enables, sensitivity, thresholds.
        Pushed by the processor; consumed by the worker when scoring. */
    void setRuntimeControls (const daat::detect::RuntimeControls& controls);
    daat::detect::RuntimeControls getRuntimeControls() const;

    //==========================================================================
    // Worker-thread entry points (public so AnalysisWorker can drive the loop).
    juce::WaitableEvent& getWakeEvent() noexcept { return wakeEvent; }
    bool isCaptureActive()  const noexcept { return captureActive.load (std::memory_order_relaxed); }

    void serviceCommands();
    void drainCaptureFifo();
    void processPendingFileLoad (const std::function<bool()>& shouldAbort);
    void processPendingAnalysis (const std::function<bool()>& shouldAbort);

private:
    //==========================================================================
    enum class Command { none, startCapture, startLive, stop, analyze, clear };

    void beginCapture (bool live);
    void endCapture();
    void doClear();
    void runAnalysis (const std::function<bool()>& shouldAbort);
    void runRescore();
    void analyzeScale (const juce::AudioBuffer<float>& buffer,
                       int regionStart, int regionLength, int channels,
                       double analysisRate, AnalysisScale scale,
                       double windowSeconds, double overlap,
                       const DetectionProfile& prof,
                       const daat::detect::RuntimeControls& controls,
                       std::vector<WindowResult>& out,
                       const std::function<bool()>& shouldAbort,
                       int windowsPlanned, int& windowsDone);

    /** Aggregation + group scoring + confidence + verdict + evidence + caveats.
        The single scoring path shared by analysis and rescoring. */
    void finalizeResult (AnalysisResult& result,
                         const DetectionProfile& prof,
                         const daat::detect::RuntimeControls& controls) const;

    void setFailure (const juce::String& msg);
    void updateSnapshot (const std::function<void (EngineSnapshot&)>& fn);
    void publishResult (std::shared_ptr<AnalysisResult> result);
    void publishSnapshotFor (const AnalysisResult& result, const juce::String& message);
    void clearResult();

    //==========================================================================
    AudioRingBuffer fifo;
    juce::AudioBuffer<float> analysisBuffer;     // preallocated to maxSeconds
    int    writePos                 = 0;         // valid samples in analysisBuffer
    double preparedSampleRate       = 48000.0;
    double captureSampleRate        = 48000.0;
    int    captureChannelsPrepared  = 2;
    int    analysisChannels         = 2;         // meaningful channels in buffer
    bool   truncated                = false;
    int    captureGeneration        = 0;         // bumps whenever the buffer contents change
    const double maxSeconds         = 300.0;     // hard cap on captured/loaded audio

    std::atomic<bool> captureActive    { false };
    std::atomic<bool> cancelFlag       { false };
    std::atomic<bool> analyzeRequested { false };
    std::atomic<bool> rescoreRequested { false };
    std::atomic<bool> pendingFileFlag  { false };
    std::atomic<int>  command          { (int) Command::none };
    std::atomic<int>  resultGeneration { 0 };

    juce::File pendingFile;
    juce::File sourceFile;
    juce::String sourceFileHash;                 // SHA-256 hex of sourceFile (worker-computed)
    mutable juce::CriticalSection fileLock;
    juce::CriticalSection bufferLock;            // guards analysisBuffer / writePos / fifo realloc
    mutable juce::CriticalSection snapshotLock;  // guards snapshot
    EngineSnapshot snapshot;

    // Requested analysis range (empty = whole capture) and exclusions.
    juce::Range<double> pendingRange;
    std::vector<juce::Range<double>> excludedRanges;
    mutable juce::CriticalSection requestLock;

    DetectionProfile profile;
    mutable juce::CriticalSection profileLock;
    daat::detect::RuntimeControls runtimeControls;
    mutable juce::CriticalSection controlsLock;

    // Worker-thread only.
    FeatureExtractor extractor;
    juce::AudioBuffer<float> resampledBuffer;
    int    resampledGeneration = -1;             // captureGeneration the cache belongs to
    int    resampledSourceLen  = -1;             // writePos the cache was built from
    double resampledRate       = 0.0;

    std::shared_ptr<const AnalysisResult> lastResult;
    mutable juce::CriticalSection resultLock;

    juce::AudioFormatManager formatManager;
    juce::WaitableEvent wakeEvent;
    std::unique_ptr<AnalysisWorker> worker;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AnalysisEngine)
};
