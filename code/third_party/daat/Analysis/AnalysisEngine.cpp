#include "AnalysisEngine.h"
#include "AnalysisWorker.h"
#include "FeatureRegistry.h"
#include "../Features/FeatureEvaluation.h"

#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <map>

namespace
{
    const char* const kLimitation =
        "This result is a statistical screening estimate. Audio processing, synthesis, "
        "editing, compression, mastering, and source separation may produce similar "
        "characteristics. The result does not prove how the recording was created.";

    juce::String formatSeconds (double s)
    {
        return juce::String (s, 1) + " s";
    }
}

//==============================================================================
AnalysisEngine::AnalysisEngine()
{
    formatManager.registerBasicFormats();
    profile = DetectionProfile::getFactoryDefault();

    updateSnapshot ([] (EngineSnapshot& s)
    {
        s = EngineSnapshot{};
        s.message = "Idle. Capture or load a file.";
    });

    worker = std::make_unique<AnalysisWorker> (*this);
    worker->startThread();
}

AnalysisEngine::~AnalysisEngine()
{
    // Ordered shutdown (spec §21): signal, wake, join, then release.
    captureActive.store (false, std::memory_order_relaxed);

    if (worker != nullptr)
    {
        worker->signalThreadShouldExit();
        wakeEvent.signal();
        worker->stopThread (2000);
        worker.reset();
    }
}

//==============================================================================
void AnalysisEngine::prepare (double sampleRate, int /*blockSize*/, int numInputChannels)
{
    const juce::ScopedLock sl (bufferLock);

    const double newRate     = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int    newChannels = juce::jlimit (1, 2, numInputChannels);

    // Hosts call prepareToPlay repeatedly with unchanged settings (transport
    // start, offline render, bypass toggles). With the same configuration,
    // keep everything - including a capture the user armed before pressing
    // play. The host guarantees processBlock is not running during this call.
    const bool configChanged = newRate != preparedSampleRate
                            || newChannels != captureChannelsPrepared
                            || analysisBuffer.getNumSamples() == 0;
    if (! configChanged)
        return;

    // Configuration changed: any capture in progress is at the old rate or
    // channel count and cannot continue.
    captureActive.store (false, std::memory_order_relaxed);

    preparedSampleRate      = newRate;
    captureChannelsPrepared = newChannels;

    const int fifoCapacity = (int) std::ceil (4.0 * preparedSampleRate); // ~4 s of headroom
    fifo.prepare (captureChannelsPrepared, fifoCapacity);

    const int capacity = (int) std::ceil (maxSeconds * preparedSampleRate);
    analysisBuffer.setSize (2, capacity, false, true, false);
    analysisBuffer.clear();
    writePos          = 0;
    truncated         = false;
    captureSampleRate = preparedSampleRate;
    analysisChannels  = captureChannelsPrepared;
    ++captureGeneration;

    {
        const juce::ScopedLock rl (requestLock);
        pendingRange = {};
        excludedRanges.clear();
    }
    {
        const juce::ScopedLock fl (fileLock);
        sourceFile = juce::File();
        sourceFileHash.clear();
    }
    clearResult();

    updateSnapshot ([this] (EngineSnapshot& s)
    {
        s = EngineSnapshot{};
        s.sampleRate = preparedSampleRate;
        s.message    = "Idle. Capture or load a file.";
    });
}

void AnalysisEngine::release()
{
    captureActive.store (false, std::memory_order_relaxed);
    command.store ((int) Command::stop, std::memory_order_relaxed);
    wakeEvent.signal();
}

//==============================================================================
void AnalysisEngine::pushAudioBlock (const juce::AudioBuffer<float>& buffer) noexcept
{
    if (! captureActive.load (std::memory_order_relaxed))
        return;

    // Wait-free write. Surplus (if the worker briefly falls behind) is dropped.
    fifo.write (buffer, buffer.getNumChannels(), buffer.getNumSamples());
}

//==============================================================================
void AnalysisEngine::startCapture (bool live)
{
    cancelFlag.store (false, std::memory_order_relaxed);
    command.store ((int) (live ? Command::startLive : Command::startCapture),
                   std::memory_order_relaxed);
    wakeEvent.signal();
}

void AnalysisEngine::stop()
{
    cancelFlag.store (true, std::memory_order_relaxed);
    command.store ((int) Command::stop, std::memory_order_relaxed);
    wakeEvent.signal();
}

void AnalysisEngine::clear()
{
    cancelFlag.store (true, std::memory_order_relaxed);
    command.store ((int) Command::clear, std::memory_order_relaxed);
    wakeEvent.signal();
}

void AnalysisEngine::loadFile (const juce::File& file)
{
    {
        const juce::ScopedLock sl (fileLock);
        pendingFile = file;
    }
    cancelFlag.store (false, std::memory_order_relaxed);
    pendingFileFlag.store (true, std::memory_order_relaxed);
    wakeEvent.signal();
}

void AnalysisEngine::requestAnalyze()
{
    {
        const juce::ScopedLock sl (requestLock);
        pendingRange = {};
    }
    analyzeRequested.store (true, std::memory_order_relaxed);
    wakeEvent.signal();
}

void AnalysisEngine::requestAnalyzeRange (double startSeconds, double endSeconds)
{
    {
        const juce::ScopedLock sl (requestLock);
        pendingRange = juce::Range<double> (juce::jmax (0.0, juce::jmin (startSeconds, endSeconds)),
                                            juce::jmax (startSeconds, endSeconds));
    }
    analyzeRequested.store (true, std::memory_order_relaxed);
    wakeEvent.signal();
}

void AnalysisEngine::requestRescore()
{
    rescoreRequested.store (true, std::memory_order_relaxed);
    wakeEvent.signal();
}

bool AnalysisEngine::applyProfile (const DetectionProfile& newProfile)
{
    const auto old = getProfile();
    setProfile (newProfile);

    const auto last = getResult();
    if (last == nullptr)
        return false; // nothing analysed yet; the new profile applies next time

    if (newProfile.requiresReanalysisComparedTo (old))
    {
        if (last->isRangeAnalysis)
            requestAnalyzeRange (last->rangeStartSeconds, last->rangeEndSeconds);
        else
            requestAnalyze();
        return true;
    }

    requestRescore();
    return false;
}

void AnalysisEngine::setExcludedRanges (std::vector<juce::Range<double>> ranges)
{
    ranges.erase (std::remove_if (ranges.begin(), ranges.end(),
                                  [] (const juce::Range<double>& r) { return r.getLength() <= 0.0; }),
                  ranges.end());
    std::sort (ranges.begin(), ranges.end(),
               [] (const juce::Range<double>& a, const juce::Range<double>& b) { return a.getStart() < b.getStart(); });

    std::vector<juce::Range<double>> merged;
    for (const auto& r : ranges)
    {
        if (! merged.empty() && r.getStart() <= merged.back().getEnd())
            merged.back() = merged.back().getUnionWith (r);
        else
            merged.push_back (r);
    }

    const juce::ScopedLock sl (requestLock);
    excludedRanges = std::move (merged);
}

std::vector<juce::Range<double>> AnalysisEngine::getExcludedRanges() const
{
    const juce::ScopedLock sl (requestLock);
    return excludedRanges;
}

//==============================================================================
EngineSnapshot AnalysisEngine::getSnapshot() const
{
    const juce::ScopedLock sl (snapshotLock);
    return snapshot;
}

void AnalysisEngine::updateSnapshot (const std::function<void (EngineSnapshot&)>& fn)
{
    const juce::ScopedLock sl (snapshotLock);
    fn (snapshot);
}

std::shared_ptr<const AnalysisResult> AnalysisEngine::getResult() const
{
    const juce::ScopedLock sl (resultLock);
    return lastResult;
}

void AnalysisEngine::publishResult (std::shared_ptr<AnalysisResult> result)
{
    if (result != nullptr)
        result->generation = ++resultGeneration;

    const juce::ScopedLock sl (resultLock);
    lastResult = std::move (result);
}

void AnalysisEngine::clearResult()
{
    ++resultGeneration;
    const juce::ScopedLock sl (resultLock);
    lastResult = nullptr;
}

juce::File AnalysisEngine::getSourceFile() const
{
    const juce::ScopedLock sl (fileLock);
    return sourceFile;
}

DetectionProfile AnalysisEngine::getProfile() const
{
    const juce::ScopedLock sl (profileLock);
    return profile;
}

void AnalysisEngine::setProfile (const DetectionProfile& newProfile)
{
    const juce::ScopedLock sl (profileLock);
    profile = newProfile;
}

void AnalysisEngine::setRuntimeControls (const daat::detect::RuntimeControls& controls)
{
    const juce::ScopedLock sl (controlsLock);
    runtimeControls = controls;
}

daat::detect::RuntimeControls AnalysisEngine::getRuntimeControls() const
{
    const juce::ScopedLock sl (controlsLock);
    return runtimeControls;
}

//==============================================================================
void AnalysisEngine::serviceCommands()
{
    const auto c = (Command) command.exchange ((int) Command::none, std::memory_order_relaxed);

    switch (c)
    {
        case Command::startCapture: beginCapture (false); break;
        case Command::startLive:    beginCapture (true);  break;
        case Command::stop:         endCapture();         break;
        case Command::clear:        doClear();            break;
        case Command::analyze:      analyzeRequested.store (true, std::memory_order_relaxed); break;
        case Command::none:
        default: break;
    }
}

void AnalysisEngine::beginCapture (bool live)
{
    const juce::ScopedLock sl (bufferLock);

    fifo.reset();
    writePos          = 0;
    truncated         = false;
    captureSampleRate = preparedSampleRate;
    analysisChannels  = captureChannelsPrepared;
    ++captureGeneration;

    {
        const juce::ScopedLock rl (requestLock);
        pendingRange = {};
        excludedRanges.clear();
    }
    {
        const juce::ScopedLock fl (fileLock);
        sourceFile = juce::File();
        sourceFileHash.clear();
    }
    clearResult();

    cancelFlag.store (false, std::memory_order_relaxed);
    captureActive.store (true, std::memory_order_relaxed);

    updateSnapshot ([this, live] (EngineSnapshot& s)
    {
        s = EngineSnapshot{};
        s.state          = live ? EngineState::Live : EngineState::Capturing;
        s.sampleRate     = captureSampleRate;
        s.channels       = analysisChannels;
        s.capturedSeconds = 0.0;
        s.sourceName     = live ? "Live input" : "Captured input";
        s.message        = "Capturing...";
    });
}

void AnalysisEngine::drainCaptureFifo()
{
    if (! captureActive.load (std::memory_order_relaxed))
        return;

    const juce::ScopedLock sl (bufferLock);

    if (! captureActive.load (std::memory_order_relaxed))
        return;

    const int capacity = analysisBuffer.getNumSamples();
    int space = capacity - writePos;

    if (space > 0)
    {
        const int toRead = juce::jmin (fifo.getNumReady(), space);
        if (toRead > 0)
        {
            fifo.read (analysisBuffer, writePos, toRead);
            writePos += toRead;
            space    -= toRead;
        }
    }

    const double seconds = captureSampleRate > 0.0 ? (double) writePos / captureSampleRate : 0.0;
    const bool hitCap = space <= 0;

    if (hitCap)
    {
        truncated = true;
        captureActive.store (false, std::memory_order_relaxed);
    }

    updateSnapshot ([this, seconds, hitCap] (EngineSnapshot& s)
    {
        s.capturedSeconds = seconds;
        s.progress        = (float) juce::jlimit (0.0, 1.0, seconds / maxSeconds);
        s.truncated       = truncated;
        if (hitCap)
        {
            s.state   = EngineState::Idle;
            s.message = "Capture reached the " + juce::String (maxSeconds, 0)
                          + " s limit. Analyzing...";
        }
    });

    if (hitCap)
        requestAnalyze();
}

void AnalysisEngine::endCapture()
{
    // Stop with no capture running (e.g. to cancel a file analysis) must not
    // start a fresh analysis of whatever is already in the buffer.
    if (! captureActive.load (std::memory_order_relaxed))
    {
        cancelFlag.store (false, std::memory_order_relaxed);
        return;
    }

    // One final drain to collect whatever is still in the FIFO.
    {
        const juce::ScopedLock sl (bufferLock);
        const int capacity = analysisBuffer.getNumSamples();
        const int space    = capacity - writePos;
        if (space > 0)
        {
            const int toRead = juce::jmin (fifo.getNumReady(), space);
            if (toRead > 0)
            {
                fifo.read (analysisBuffer, writePos, toRead);
                writePos += toRead;
            }
        }
    }

    captureActive.store (false, std::memory_order_relaxed);
    cancelFlag.store (false, std::memory_order_relaxed);

    const double seconds = captureSampleRate > 0.0 ? (double) writePos / captureSampleRate : 0.0;

    updateSnapshot ([this, seconds] (EngineSnapshot& s)
    {
        s.state           = EngineState::Idle;
        s.capturedSeconds = seconds;
        s.channels        = analysisChannels;
        s.sampleRate      = captureSampleRate;
        s.message         = seconds > 0.0 ? "Captured " + juce::String (seconds, 1) + " s. Analyzing..."
                                          : "Nothing captured.";
    });

    if (seconds > 0.0)
        requestAnalyze(); // auto-analyze on stop
}

void AnalysisEngine::doClear()
{
    const juce::ScopedLock sl (bufferLock);

    captureActive.store (false, std::memory_order_relaxed);
    analyzeRequested.store (false, std::memory_order_relaxed);
    rescoreRequested.store (false, std::memory_order_relaxed);
    pendingFileFlag.store (false, std::memory_order_relaxed);
    fifo.reset();
    writePos  = 0;
    truncated = false;
    ++captureGeneration;

    {
        const juce::ScopedLock rl (requestLock);
        pendingRange = {};
        excludedRanges.clear();
    }
    {
        const juce::ScopedLock fl (fileLock);
        sourceFile = juce::File();
        sourceFileHash.clear();
    }
    clearResult();

    updateSnapshot ([this] (EngineSnapshot& s)
    {
        s = EngineSnapshot{};
        s.sampleRate = preparedSampleRate;
        s.message    = "Cleared.";
    });
}

//==============================================================================
void AnalysisEngine::processPendingFileLoad (const std::function<bool()>& shouldAbort)
{
    if (! pendingFileFlag.exchange (false, std::memory_order_relaxed))
        return;

    juce::File file;
    {
        const juce::ScopedLock sl (fileLock);
        file = pendingFile;
    }

    if (! file.existsAsFile())
    {
        setFailure ("File not found: " + file.getFullPathName());
        return;
    }

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
    if (reader == nullptr)
    {
        setFailure ("Unsupported or unreadable audio file: " + file.getFileName());
        return;
    }

    captureActive.store (false, std::memory_order_relaxed);

    const juce::ScopedLock sl (bufferLock);

    captureSampleRate = reader->sampleRate > 0.0 ? reader->sampleRate : preparedSampleRate;
    analysisChannels  = juce::jlimit (1, 2, (int) reader->numChannels);

    // Cap in *seconds of this file*, not in buffer samples: the buffer is sized
    // at the host rate, so a low-rate file would otherwise exceed the cap and a
    // high-rate file would stop early. The buffer capacity is the hard ceiling.
    const int   capacity = analysisBuffer.getNumSamples();
    const juce::int64 total = reader->lengthInSamples;
    const auto  secondsCap = (juce::int64) std::floor (maxSeconds * captureSampleRate);
    const auto  limit = juce::jmin ((juce::int64) capacity, secondsCap);
    const bool  willTruncate = total > limit;
    const int   toLoad = (int) juce::jmin (limit, total);
    const double limitSeconds = (double) limit / captureSampleRate;

    analysisBuffer.clear();
    writePos = 0;
    ++captureGeneration;

    {
        const juce::ScopedLock rl (requestLock);
        pendingRange = {};
        excludedRanges.clear();
    }
    clearResult();

    updateSnapshot ([this, &file] (EngineSnapshot& s)
    {
        s = EngineSnapshot{};
        s.state      = EngineState::LoadingFile;
        s.sampleRate = captureSampleRate;
        s.channels   = analysisChannels;
        s.sourceName = file.getFileName();
        s.progress   = 0.0f;
        s.message    = "Loading " + file.getFileName() + "...";
    });

    const int chunk = 1 << 15; // 32768 samples
    int pos = 0;
    const bool readRight = reader->numChannels > 1;

    while (pos < toLoad)
    {
        if (shouldAbort() || cancelFlag.load (std::memory_order_relaxed))
        {
            writePos  = pos;
            truncated = willTruncate;
            updateSnapshot ([] (EngineSnapshot& s)
            {
                s.state   = EngineState::Idle;
                s.message = "File load cancelled.";
            });
            return;
        }

        const int n = juce::jmin (chunk, toLoad - pos);
        reader->read (&analysisBuffer, pos, n, pos, true, readRight);
        pos += n;

        updateSnapshot ([pos, toLoad] (EngineSnapshot& s)
        {
            s.progress = toLoad > 0 ? (float) pos / (float) toLoad : 1.0f;
        });
    }

    writePos  = toLoad;
    truncated = willTruncate;
    ++captureGeneration;

    // Provenance hash for reports / training rows (worker thread, not the UI).
    const auto hash = juce::SHA256 (file).toHexString();
    {
        const juce::ScopedLock fl (fileLock);
        sourceFile = file;
        sourceFileHash = hash;
    }

    const double seconds = captureSampleRate > 0.0 ? (double) writePos / captureSampleRate : 0.0;

    updateSnapshot ([this, seconds, limitSeconds] (EngineSnapshot& s)
    {
        s.capturedSeconds = seconds;
        s.truncated       = truncated;
        s.progress        = 1.0f;
        s.message         = truncated
            ? "Loaded first " + juce::String (seconds, 1) + " s (file exceeds the "
                + juce::String (limitSeconds, 0) + " s limit at "
                + juce::String (captureSampleRate / 1000.0, 1) + " kHz). Analyzing..."
            : "Loaded " + juce::String (seconds, 1) + " s. Analyzing...";
    });

    requestAnalyze(); // auto-analyze after load
}

//==============================================================================
void AnalysisEngine::processPendingAnalysis (const std::function<bool()>& shouldAbort)
{
    if (analyzeRequested.exchange (false, std::memory_order_relaxed))
    {
        // A full analysis already scores with the current settings.
        rescoreRequested.store (false, std::memory_order_relaxed);

        // Stop / Clear cancel a running analysis (spec §22), not just a file load.
        // Paths that start new work (capture, load, end of capture) reset the
        // flag, so a cancel issued just before this point is never lost.
        const std::function<bool()> abortCheck = [this, &shouldAbort]
        {
            return shouldAbort() || cancelFlag.load (std::memory_order_relaxed);
        };
        runAnalysis (abortCheck);
        return;
    }

    if (rescoreRequested.exchange (false, std::memory_order_relaxed))
        runRescore();
}

void AnalysisEngine::analyzeScale (const juce::AudioBuffer<float>& buffer,
                                   int regionStart, int regionLength, int channels,
                                   double analysisRate, AnalysisScale scale,
                                   double windowSeconds, double overlap,
                                   const DetectionProfile& prof,
                                   const daat::detect::RuntimeControls& controls,
                                   std::vector<WindowResult>& out,
                                   const std::function<bool()>& shouldAbort,
                                   int windowsPlanned, int& windowsDone)
{
    const auto plan = FeatureExtractor::planWindows (regionLength, analysisRate, windowSeconds, overlap);
    out.reserve (plan.size());
    const auto mask = daat::features::scaleBit (scale);

    for (const auto& [offset, length] : plan)
    {
        if (shouldAbort())
            return;

        const int start = regionStart + offset;

        WindowResult wr;
        wr.scale        = scale;
        wr.startSeconds = (double) start / analysisRate;           // timeline seconds
        wr.endSeconds   = (double) (start + length) / analysisRate;
        wr.raw          = extractor.analyzeWindow (buffer, start, length, channels);
        wr.features     = daat::features::evaluateWindow (wr.raw, prof, channels, mask);
        daat::detect::scoreWindow (wr, prof, controls);
        out.push_back (std::move (wr));

        ++windowsDone;
        if (windowsPlanned > 0)
            updateSnapshot ([windowsDone, windowsPlanned] (EngineSnapshot& s)
            {
                s.progress = juce::jlimit (0.0f, 1.0f, (float) windowsDone / (float) windowsPlanned);
            });
    }
}

//==============================================================================
void AnalysisEngine::runAnalysis (const std::function<bool()>& shouldAbort)
{
    const DetectionProfile prof = getProfile();
    const auto controls = getRuntimeControls();
    const double outRate = prof.analysis.sampleRate;

    if (outRate <= 0.0)
    {
        setFailure ("Invalid analysis sample rate in profile.");
        return;
    }

    juce::Range<double> range;
    std::vector<juce::Range<double>> exclusions;
    {
        const juce::ScopedLock sl (requestLock);
        range      = pendingRange;
        exclusions = excludedRanges;
    }

    // --- 1. Resample the capture to the analysis rate (cached per capture) ---
    int    ch = 0;
    double inRate = 0.0;
    bool   wasTruncated = false;

    {
        const juce::ScopedLock sl (bufferLock);
        const int n  = writePos;
        ch           = juce::jlimit (1, analysisBuffer.getNumChannels(), analysisChannels);
        inRate       = captureSampleRate > 0.0 ? captureSampleRate : outRate;
        wasTruncated = truncated;

        if (n <= 0)
        {
            setFailure ("No audio to analyze - capture or load first.");
            return;
        }

        updateSnapshot ([] (EngineSnapshot& s) { s.state = EngineState::Analyzing; s.progress = 0.0f; });

        const bool cacheValid = resampledGeneration == captureGeneration
                             && resampledSourceLen == n
                             && resampledRate == outRate
                             && resampledBuffer.getNumChannels() == ch;
        if (! cacheValid)
        {
            const double ratio  = inRate / outRate; // input consumed per output sample
            const int    outLen = juce::jmax (1, (int) std::floor ((double) n / ratio));

            resampledBuffer.setSize (ch, outLen, false, false, true);
            resampledBuffer.clear();

            for (int c = 0; c < ch; ++c)
            {
                juce::LagrangeInterpolator interp;
                interp.reset();
                interp.process (ratio, analysisBuffer.getReadPointer (c),
                                resampledBuffer.getWritePointer (c), outLen);
            }

            resampledGeneration = captureGeneration;
            resampledSourceLen  = n;
            resampledRate       = outRate;
        }
    }

    const int outLen = resampledBuffer.getNumSamples();

    // --- 2. Resolve the analysed region (whole capture, or a selected range) ---
    int regionStart = 0, regionEnd = outLen;
    const bool isRange = ! range.isEmpty();
    if (isRange)
    {
        regionStart = juce::jlimit (0, outLen, (int) std::round (range.getStart() * outRate));
        regionEnd   = juce::jlimit (regionStart, outLen, (int) std::round (range.getEnd() * outRate));
    }
    const int regionLen = regionEnd - regionStart;

    if (regionLen <= 0)
    {
        setFailure ("The selected range contains no audio.");
        return;
    }

    // Level summary over the analysed region (drives caveats and confidence).
    float peak = 0.0f;
    double sumSq = 0.0;
    juce::int64 clippedSamples = 0;
    for (int c = 0; c < ch; ++c)
    {
        const float* d = resampledBuffer.getReadPointer (c);
        for (int i = regionStart; i < regionEnd; ++i)
        {
            const float a = std::abs (d[i]);
            peak = juce::jmax (peak, a);
            sumSq += (double) d[i] * (double) d[i];
            if (a >= 0.999f) ++clippedSamples;
        }
    }
    const double rms = std::sqrt (sumSq / ((double) regionLen * (double) ch));

    auto result = std::make_shared<AnalysisResult>();
    result->analysisSampleRate = outRate;
    result->sourceSampleRate   = inRate;
    result->channels           = ch;
    result->timelineSeconds    = (double) outLen / outRate;
    result->rangeStartSeconds  = (double) regionStart / outRate;
    result->rangeEndSeconds    = (double) regionEnd / outRate;
    result->isRangeAnalysis    = isRange;
    result->analyzedSeconds    = (double) regionLen / outRate;
    result->effectiveSeconds   = result->analyzedSeconds;
    result->peakLevel          = peak;
    result->rmsDb              = (float) juce::Decibels::gainToDecibels (rms, -100.0);
    result->clipped            = clippedSamples > (juce::int64) ((double) regionLen * (double) ch * 0.0001);
    result->truncated          = wasTruncated;
    result->excludedRanges     = exclusions;
    result->limitations.add (kLimitation);
    {
        const juce::ScopedLock fl (fileLock);
        result->sourceName     = sourceFile.existsAsFile() ? sourceFile.getFileName() : juce::String ("DAW capture");
        result->sourcePath     = sourceFile.getFullPathName();
        result->sourceFileHash = sourceFileHash;
    }

    // --- 3. Minimum-duration gate ---
    if (result->analyzedSeconds < prof.analysis.minimumAnalyzedSeconds)
    {
        result->verdict = Verdict::InsufficientAudio;
        result->summary = "Insufficient audio: " + formatSeconds (result->analyzedSeconds)
                        + (isRange ? " selected" : " analyzed") + ", minimum is "
                        + formatSeconds (prof.analysis.minimumAnalyzedSeconds) + ".";
        publishResult (result);
        publishSnapshotFor (*result, result->summary);
        return;
    }

    // --- 4. Windowed FFT feature extraction across all three scales ---
    FeatureExtractor::Settings xs;
    xs.hfCutoffHz         = prof.hfCutoffHz();
    xs.repetitionMinLagMs = prof.repetitionMinLagMs();
    xs.repetitionMaxLagMs = prof.repetitionMaxLagMs();

    if (! extractor.prepare (prof.analysis.fftSize, prof.analysis.hopSize, outRate, xs))
    {
        setFailure ("Invalid FFT settings in profile (size must be a power of two).");
        return;
    }

    const auto countPlan = [regionLen, outRate] (double secs, double ov)
    {
        return (int) FeatureExtractor::planWindows (regionLen, outRate, secs, ov).size();
    };
    const int windowsPlanned = countPlan (prof.analysis.shortWindowSeconds,  prof.analysis.shortOverlap)
                             + countPlan (prof.analysis.mediumWindowSeconds, prof.analysis.mediumOverlap)
                             + countPlan (prof.analysis.longWindowSeconds,   prof.analysis.longOverlap);
    int windowsDone = 0;

    analyzeScale (resampledBuffer, regionStart, regionLen, ch, outRate, AnalysisScale::shortScale,
                  prof.analysis.shortWindowSeconds, prof.analysis.shortOverlap, prof, controls,
                  result->shortWindows, shouldAbort, windowsPlanned, windowsDone);
    analyzeScale (resampledBuffer, regionStart, regionLen, ch, outRate, AnalysisScale::mediumScale,
                  prof.analysis.mediumWindowSeconds, prof.analysis.mediumOverlap, prof, controls,
                  result->mediumWindows, shouldAbort, windowsPlanned, windowsDone);
    analyzeScale (resampledBuffer, regionStart, regionLen, ch, outRate, AnalysisScale::longScale,
                  prof.analysis.longWindowSeconds, prof.analysis.longOverlap, prof, controls,
                  result->longWindows, shouldAbort, windowsPlanned, windowsDone);

    if (shouldAbort())
    {
        updateSnapshot ([] (EngineSnapshot& s) { s.state = EngineState::Idle; s.message = "Analysis cancelled."; });
        return;
    }

    // --- 5. Score ---
    finalizeResult (*result, prof, controls);
    publishResult (result);
    publishSnapshotFor (*result, "Screening estimate ready - " + toString (result->verdict)
                                   + (isRange ? " (selected range)." : "."));
}

//==============================================================================
void AnalysisEngine::runRescore()
{
    const auto last = getResult();
    if (last == nullptr)
        return;

    const DetectionProfile prof = getProfile();
    const auto controls = getRuntimeControls();

    auto result = std::make_shared<AnalysisResult> (*last);
    {
        const juce::ScopedLock sl (requestLock);
        result->excludedRanges = excludedRanges;
    }

    if (result->totalWindows() == 0)
    {
        // Nothing was extracted (e.g. insufficient audio): exclusions are
        // recorded, but there is nothing to re-score.
        publishResult (result);
        publishSnapshotFor (*result, result->summary);
        return;
    }

    updateSnapshot ([] (EngineSnapshot& s) { s.state = EngineState::Analyzing; s.progress = 0.5f; });

    // Re-evaluate every window from its stored raw measurements, so threshold,
    // weight, window-assignment, and control changes all take effect.
    for (auto* windows : { &result->shortWindows, &result->mediumWindows, &result->longWindows })
    {
        for (auto& w : *windows)
        {
            w.features = daat::features::evaluateWindow (w.raw, prof, result->channels,
                                                         daat::features::scaleBit (w.scale));
            daat::detect::scoreWindow (w, prof, controls);
        }
    }

    finalizeResult (*result, prof, controls);
    publishResult (result);
    publishSnapshotFor (*result, "Rescored - " + toString (result->verdict) + ".");
}

//==============================================================================
void AnalysisEngine::finalizeResult (AnalysisResult& r,
                                     const DetectionProfile& prof,
                                     const daat::detect::RuntimeControls& controls) const
{
    r.featureAverages.clear();
    r.groupScores.clear();
    r.strongestEvidence.clear();
    r.contradictoryEvidence.clear();
    r.caveats.clear();
    if (r.limitations.isEmpty())
        r.limitations.add (kLimitation);

    // Excluded time within the analysed region.
    const juce::Range<double> region (r.rangeStartSeconds, r.rangeEndSeconds);
    double excludedWithin = 0.0;
    for (const auto& e : r.excludedRanges)
        excludedWithin += e.getIntersectionWith (region).getLength();
    r.effectiveSeconds = juce::jmax (0.0, r.analyzedSeconds - excludedWithin);

    // --- Per-feature averages over non-excluded windows ---
    struct Agg { double raw = 0.0, susp = 0.0, conf = 0.0; float weight = 0.0f; int n = 0; };
    std::map<juce::String, Agg> agg;
    int used = 0;

    for (const auto* windows : { &r.shortWindows, &r.mediumWindows, &r.longWindows })
    {
        for (const auto& w : *windows)
        {
            if (r.isExcluded (w.startSeconds, w.endSeconds))
                continue;
            ++used;

            for (const auto& f : w.features)
            {
                if (! f.valid)
                    continue;
                auto& a = agg[f.featureId];
                a.raw   += f.rawValue;
                a.susp  += f.suspicionScore;
                a.conf  += f.confidence;
                a.weight = f.effectiveWeight;
                ++a.n;
            }
        }
    }
    r.windowsUsed = used;

    for (const auto& desc : getFeatureRegistry()) // registry order for stable display
    {
        const auto it = agg.find (desc.id);
        if (it == agg.end() || it->second.n == 0)
            continue;

        const auto& a = it->second;
        FeatureResult fr;
        fr.featureId       = desc.id;
        fr.valid           = true;
        fr.rawValue        = (float) (a.raw  / a.n);
        fr.suspicionScore  = (float) (a.susp / a.n);
        fr.normalizedValue = fr.suspicionScore;
        fr.confidence      = (float) (a.conf / a.n);
        fr.effectiveWeight = a.weight;
        const double meanRaw = a.raw / a.n;
        const int decimals   = std::abs (meanRaw) >= 100.0 ? 0 : (std::abs (meanRaw) >= 10.0 ? 1 : 3);
        fr.explanation     = "mean " + juce::String (meanRaw, decimals) + " over " + juce::String (a.n) + " windows";
        r.featureAverages.push_back (fr);
    }
    r.numValidFeatures = (int) r.featureAverages.size();

    daat::detect::ConfidenceContext ctx;
    ctx.analyzedSeconds  = r.effectiveSeconds;
    ctx.minimumSeconds   = prof.analysis.minimumAnalyzedSeconds;
    ctx.numWindows       = used;
    ctx.channels         = r.channels;
    ctx.clipped          = r.clipped;
    ctx.rmsDb            = r.rmsDb;
    ctx.sourceSampleRate = r.sourceSampleRate;

    r.caveats = daat::detect::detectCaveats (r.featureAverages, ctx, r.truncated);

    if (! r.excludedRanges.empty())
        r.caveats.insert (0, juce::String ((int) r.excludedRanges.size()) + " section(s), "
                               + formatSeconds (excludedWithin)
                               + ", excluded by the user. The estimate reflects only the remaining "
                               + formatSeconds (r.effectiveSeconds) + ".");

    if (r.isRangeAnalysis)
        r.caveats.insert (0, "Only " + formatSeconds (r.rangeStartSeconds) + "-"
                               + formatSeconds (r.rangeEndSeconds)
                               + " was analysed; the rest of the recording is not reflected.");

    // Too much excluded to say anything.
    if (r.effectiveSeconds < prof.analysis.minimumAnalyzedSeconds)
    {
        r.verdict           = Verdict::InsufficientAudio;
        r.overallLikelihood = 0.0f;
        r.confidence        = 0.0f;
        r.numAgreeingGroups = 0;
        r.numActiveGroups   = 0;
        r.summary = "Insufficient audio after exclusions: " + formatSeconds (r.effectiveSeconds)
                  + " remain, minimum is " + formatSeconds (prof.analysis.minimumAnalyzedSeconds) + ".";
        return;
    }

    r.groupScores      = daat::detect::computeGroupScores (r.featureAverages, prof, controls);
    const auto overall = daat::detect::combineGroups (r.groupScores, prof, controls);
    const float conf   = daat::detect::computeConfidence (overall, r.groupScores, prof, ctx);

    r.overallLikelihood = overall.likelihood;
    r.confidence        = conf;
    r.numAgreeingGroups = overall.numAgreeingGroups;
    r.numActiveGroups   = overall.numActiveGroups;
    r.verdict           = daat::detect::decideVerdict (overall, conf, prof, controls);

    daat::detect::extractEvidence (r.featureAverages, r.strongestEvidence, r.contradictoryEvidence);

    r.summary = daat::detect::describeVerdict (r.verdict, r.overallLikelihood, r.confidence);
}

//==============================================================================
void AnalysisEngine::publishSnapshotFor (const AnalysisResult& r, const juce::String& message)
{
    juce::String topIndicator;
    float topSusp = -1.0f;
    for (const auto& f : r.featureAverages)
    {
        if (f.suspicionScore > topSusp)
        {
            topSusp = f.suspicionScore;
            if (const auto* d = findFeatureDescriptor (f.featureId))
                topIndicator = d->displayName + " " + juce::String (f.suspicionScore, 2);
        }
    }

    const bool scored = r.totalWindows() > 0
                     && (r.verdict == Verdict::Likely || r.verdict == Verdict::Unlikely
                         || r.verdict == Verdict::Inconclusive);

    updateSnapshot ([&] (EngineSnapshot& s)
    {
        s.state            = EngineState::Complete;
        s.hasSummary       = true;
        s.progress         = 1.0f;
        s.peakLevel        = r.peakLevel;
        s.rmsDb            = r.rmsDb;
        s.clipped          = r.clipped;
        s.truncated        = r.truncated;
        s.analyzedSeconds  = r.effectiveSeconds;
        s.capturedSeconds  = r.timelineSeconds;
        s.channels         = r.channels;
        s.sampleRate       = r.sourceSampleRate;
        s.shortWindows     = (int) r.shortWindows.size();
        s.mediumWindows    = (int) r.mediumWindows.size();
        s.longWindows      = (int) r.longWindows.size();
        s.validFeatures    = r.numValidFeatures;
        s.agreeingGroups   = r.numAgreeingGroups;
        s.activeGroups     = r.numActiveGroups;
        s.likelihood       = r.overallLikelihood;
        s.scoreConfidence  = r.confidence;
        s.hasScore         = scored;
        s.resultGeneration = r.generation;
        s.verdictText      = toString (r.verdict);
        s.verdictDetail    = r.summary;
        s.topIndicator     = topIndicator;
        s.message          = message;
    });
}

//==============================================================================
void AnalysisEngine::setFailure (const juce::String& msg)
{
    updateSnapshot ([&msg] (EngineSnapshot& s)
    {
        s.state      = EngineState::Failed;
        s.progress   = 0.0f;
        s.hasSummary = false;
        s.message    = msg;
    });
}
