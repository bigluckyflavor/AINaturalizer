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
    best move only if it:
      - fits the remaining dose budget,
      - improves likelihood by at least minGain,
      - does not worsen any other feature suspicion by more than
        maxFeatureRegression, and
      - does not introduce non-finite samples or new clipping.

    The numeric dose budget is still an optimization heuristic, not a
    calibrated perceptual metric.
*/
struct OptimizeConfig
{
    double targetLikelihood      = 0.28;  // DAAT factory "Unlikely" boundary
    int    maxIters              = 12;
    double budget                = 6.0;   // heuristic total dose
    double minGain               = 0.005;
    double maxFeatureRegression  = 0.10;
    uint64_t seed                = 1234;
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

    double peakDeltaDbFS = -200.0;
    double rmsDeltaDbFS  = -200.0;

    std::vector<StepRecord> steps;
};

class Optimizer
{
public:
    Optimizer (Oracle& oracle, double sampleRate, OptimizeConfig config = {});

    OptimizeResult run (juce::AudioBuffer<float>& buffer);

private:
    Oracle& oracle;
    double sampleRate;
    OptimizeConfig config;
    std::vector<PerturbOp> ops;
};
