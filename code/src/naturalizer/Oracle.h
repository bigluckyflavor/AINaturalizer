#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <string>
#include <vector>

//==============================================================================
/**
    Measurement oracle for the naturalizer.

    Drives DAAT's real analysis pipeline — FeatureExtractor (windowed FFT
    measurements) -> feature evaluators (per-window suspicion scores) ->
    DetectionEngine (group combination) — over a whole audio file and returns
    the aggregate AI-likelihood plus the per-feature suspicion table.

    The optimizer treats `likelihood` as the loss to minimize. Because this is
    DAAT's own code, the loop provably converges against DAAT; transfer to
    independent detectors is a separate, explicitly measured question.
*/
struct FeatureScore
{
    std::string id;
    double suspicion  = 0.0; // confidence-weighted mean over windows
    double confidence = 0.0; // mean window confidence
    double weight     = 0.0; // mean effective weight
    int    windows    = 0;
};

struct GroupScoreView
{
    std::string name;
    double score      = 0.0;
    double confidence = 0.0;
    bool   valid      = false;
};

struct AnalysisOutcome
{
    double likelihood = 0.0; // the loss: 0 = "looks human", 1 = "looks synthetic"
    double confidence = 0.0; // DAAT's evidence-quality confidence
    std::vector<FeatureScore>  features;
    std::vector<GroupScoreView> groups;
    int numWindows = 0;
};

class Oracle
{
public:
    Oracle();
    ~Oracle();

    /** Full-file analysis at the buffer's native sample rate. */
    AnalysisOutcome analyze (const juce::AudioBuffer<float>& buffer,
                             double sampleRate);

    /** Convenience: just the loss. */
    double score (const juce::AudioBuffer<float>& buffer, double sampleRate)
    {
        return analyze (buffer, sampleRate).likelihood;
    }

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};
