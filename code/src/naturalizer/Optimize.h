#pragma once

#include "Oracle.h"
#include "Perturb.h"

#include <cstdint>
#include <string>
#include <vector>

//==============================================================================
/**
    Greedy coordinate-descent optimizer.

    Each iteration tries every (operator, strength) pair on a scratch copy,
    measures the DAAT likelihood of each candidate, and applies the single
    best move — provided it clears `minGain` and fits the remaining
    perceptual budget. Stops at the target likelihood, at maxIters, or when
    no safe move improves the score (the refusal rule: better to under-deliver
    than to audibly damage the music).
*/
struct OptimizeConfig
{
    double targetLikelihood = 0.35;
    int    maxIters         = 12;
    double budget           = 6.0;   // total perceptual dose (see PerturbOp)
    double minGain          = 0.005; // minimum likelihood improvement to accept
    uint64_t seed           = 1234;
    std::vector<double> strengths { 0.4, 0.7, 1.0 };
};

struct StepRecord
{
    std::string opId;
    std::string opName;
    double strength = 0.0;
    double dose     = 0.0;
    double likelihoodBefore = 0.0;
    double likelihoodAfter  = 0.0;
};

struct OptimizeResult
{
    bool reachedTarget = false;
    bool refused       = false;
    std::string refuseReason;

    double likelihoodBefore = 0.0;
    double likelihoodAfter  = 0.0;
    double confidenceAfter  = 0.0;
    double budgetUsed       = 0.0;

    // How much the audio changed (vs the original input).
    double peakDeltaDbFS = -200.0; // 20*log10(max|x - y|)
    double rmsDeltaDbFS  = -200.0; // 20*log10(rms(x - y))

    std::vector<StepRecord> steps;
};

class Optimizer
{
public:
    Optimizer (Oracle& oracle, double sampleRate, OptimizeConfig config = {});

    /** Runs the loop in place on `buffer`. Returns the full trace. */
    OptimizeResult run (juce::AudioBuffer<float>& buffer);

private:
    Oracle& oracle;
    double sampleRate;
    OptimizeConfig config;
    std::vector<PerturbOp> ops;
};
