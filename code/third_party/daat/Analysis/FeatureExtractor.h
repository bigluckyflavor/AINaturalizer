#pragma once

#include <juce_dsp/juce_dsp.h>
#include "AnalysisResult.h"

#include <memory>
#include <utility>
#include <vector>

//==============================================================================
/**
    Windowing + FFT engine. Given a segment of the (already resampled) analysis
    buffer, it produces raw spectral and time-domain measurements for one
    window. The FFT plan and Hann table are cached in prepare() and reused
    across all windows (spec §22).

    This is the "FFT processing" deliverable for Phase 3. It intentionally
    produces raw values only - normalizing them into per-feature suspicion
    scores is the job of the Phase 4/5 feature classes.

    Worker-thread only. Not thread-safe by design.
*/
class FeatureExtractor
{
public:
    FeatureExtractor() = default;

    /** Profile settings that change the raw measurements themselves. */
    struct Settings
    {
        double hfCutoffHz       = 12000.0; // high-frequency energy ratio boundary
        double repetitionMinLagMs = 20.0;  // envelope autocorrelation lag range
        double repetitionMaxLagMs = 2000.0;
    };

    /** (Re)builds the cached FFT for the given size. fftSize must be a power of
        two; hopSize is clamped to [1, fftSize]. Returns false if fftSize is
        invalid. */
    bool prepare (int fftSize, int hopSize, double analysisSampleRate,
                  const Settings& settings); // NOTE (naturalizer port): the
                  // original `= {}` default argument is not valid C++ for a
                  // nested struct with default member initializers (NSDMI not
                  // complete until end of enclosing class; GCC/Clang reject).
                  // All in-tree callers pass Settings explicitly.

    bool isPrepared() const noexcept { return fft != nullptr; }

    /** Computes raw features for buffer[startSample, startSample+numSamples).
        numChannels 1 => mono (stereo fields left neutral). Safe for silence,
        NaN, and short/partial windows. */
    WindowRawFeatures analyzeWindow (const juce::AudioBuffer<float>& buffer,
                                     int startSample,
                                     int numSamples,
                                     int numChannels);

    /** Splits totalSamples into (offset, length) windows for a given window
        length in seconds and overlap fraction. The final partial window is
        included if it is at least half the target length. */
    static std::vector<std::pair<int, int>> planWindows (int totalSamples,
                                                          double sampleRate,
                                                          double windowSeconds,
                                                          double overlap);

private:
    void computeFrameMagnitudes (const juce::AudioBuffer<float>& buffer,
                                 int frameStart, int numChannels);

    std::unique_ptr<juce::dsp::FFT> fft;
    std::unique_ptr<juce::dsp::WindowingFunction<float>> window;

    int    fftSize   = 0;
    int    hopSize   = 0;
    int    numBins   = 0;
    double sampleRate = 48000.0;
    Settings settings;

    std::vector<float> fftScratch;   // 2 * fftSize
    std::vector<float> magnitude;    // numBins
    std::vector<float> avgMagnitude; // numBins
    std::vector<float> prevMagnitude;// numBins
    std::vector<double> frameFluxes; // per-frame spectral flux (onset envelope)
    std::vector<float> envelope;     // short-time RMS envelope for the window

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FeatureExtractor)
};
