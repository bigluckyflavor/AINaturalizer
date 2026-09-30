#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <string>
#include <vector>

//==============================================================================
/**
    Measurement oracle for the naturalizer.

    Mirrors DAAT's scoring path: the same factory profile, analysis sample rate,
    three window scales, per-feature scale masks, feature aggregation, group
    combination, confidence context, and verdict logic.

    The optimizer treats likelihood as the scalar loss, but callers can also
    inspect scorable/verdict so an unscorable input is never mistaken for a
    successful "human" result.
*/
struct FeatureScore
{
    std::string id;
    double suspicion  = 0.0; // arithmetic mean over applicable DAAT windows
    double confidence = 0.0; // mean window confidence
    double weight     = 0.0; // effective profile weight
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
    double likelihood = 0.0; // 0 = "looks human", 1 = "looks synthetic"
    double confidence = 0.0; // DAAT evidence-quality confidence
    bool scorable     = false;
    std::string verdict;
    std::vector<FeatureScore> features;
    std::vector<GroupScoreView> groups;
    int numWindows = 0;
};

class Oracle
{
public:
    Oracle();
    ~Oracle();

    AnalysisOutcome analyze (const juce::AudioBuffer<float>& buffer,
                             double sampleRate);

    double score (const juce::AudioBuffer<float>& buffer, double sampleRate)
    {
        const auto outcome = analyze (buffer, sampleRate);
        return outcome.scorable ? outcome.likelihood : 1.0;
    }

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};
