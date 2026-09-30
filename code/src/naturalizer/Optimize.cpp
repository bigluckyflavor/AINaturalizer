#include "Optimize.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>

namespace
{
    struct SampleSafety
    {
        bool finite = true;
        double peak = 0.0;
    };

    SampleSafety inspectSamples (const juce::AudioBuffer<float>& buffer)
    {
        SampleSafety s;
        for (int c = 0; c < buffer.getNumChannels(); ++c)
        {
            const float* d = buffer.getReadPointer (c);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                const double v = d[i];
                if (! std::isfinite (v))
                {
                    s.finite = false;
                    return s;
                }
                s.peak = std::max (s.peak, std::abs (v));
            }
        }
        return s;
    }

    bool hasFeatureRegression (const AnalysisOutcome& before,
                               const AnalysisOutcome& after,
                               double maxIncrease)
    {
        std::unordered_map<std::string, double> candidate;
        candidate.reserve (after.features.size());
        for (const auto& f : after.features)
            candidate[f.id] = f.suspicion;

        for (const auto& f : before.features)
        {
            const auto it = candidate.find (f.id);
            if (it == candidate.end())
                return true; // losing evidence is not a valid "improvement"
            if (it->second - f.suspicion > maxIncrease + 1.0e-9)
                return true;
        }
        return false;
    }

    bool targetReached (const AnalysisOutcome& outcome, double targetLikelihood)
    {
        return outcome.scorable
            && outcome.likelihood <= targetLikelihood
            && outcome.verdict == "Unlikely";
    }
}

Optimizer::Optimizer (Oracle& o, double sr, OptimizeConfig cfg)
    : oracle (o), sampleRate (sr), config (std::move (cfg)), ops (makeOperators (sr))
{
}

OptimizeResult Optimizer::run (juce::AudioBuffer<float>& buffer)
{
    OptimizeResult result;
    juce::AudioBuffer<float> original;
    original.makeCopyOf (buffer);

    auto currentOutcome = oracle.analyze (buffer, sampleRate);
    result.likelihoodBefore = currentOutcome.likelihood;

    if (! currentOutcome.scorable)
    {
        result.refused = true;
        result.refuseReason = "input is not scorable by the DAAT factory profile";
        result.likelihoodAfter = currentOutcome.likelihood;
        result.confidenceAfter = currentOutcome.confidence;
        return result;
    }

    double current = currentOutcome.likelihood;
    double budgetLeft = config.budget;

    const auto originalSafety = inspectSamples (buffer);
    const double allowedPeak = std::max (0.999, originalSafety.peak + 1.0e-6);

    std::printf ("iter  op                    strength   dose    likelihood\n");
    std::printf ("----  --------------------  --------   ----    ----------\n");

    for (int iter = 0; iter < config.maxIters; ++iter)
    {
        if (targetReached (currentOutcome, config.targetLikelihood)
        {
            result.reachedTarget = true;
            break;
        }

        auto candidateRng = [&] (const std::string& opId, double s)
        {
            std::size_t h = std::hash<std::string>{} (opId);
            h ^= std::hash<double>{} (s + 0.5) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<int>{} (iter) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return std::mt19937_64 (config.seed ^ h);
        };

        const PerturbOp* bestOp = nullptr;
        double bestStrength = 0.0, bestScore = current, bestDose = 0.0;
        AnalysisOutcome bestOutcome;

        for (const auto& op : ops)
        {
            for (double s : config.strengths)
            {
                const double dose = s * op.costPerUnit;
                if (dose > budgetLeft + 1e-9)
                    continue;

                juce::AudioBuffer<float> trial;
                trial.makeCopyOf (buffer);
                auto trialRng = candidateRng (op.id, s);
                op.apply (trial, s, trialRng);

                const auto safety = inspectSamples (trial);
                if (! safety.finite || safety.peak > allowedPeak)
                    continue;

                const auto trialOutcome = oracle.analyze (trial, sampleRate);
                if (! trialOutcome.scorable)
                    continue;

                if (hasFeatureRegression (currentOutcome, trialOutcome,
                                          config.maxFeatureRegression))
                    continue;

                if (trialOutcome.likelihood < bestScore)
                {
                    bestScore = trialOutcome.likelihood;
                    bestOp = &op;
                    bestStrength = s;
                    bestDose = dose;
                    bestOutcome = trialOutcome;
                }
            }
        }

        const double gain = current - bestScore;
        if (bestOp == nullptr || gain < config.minGain)
        {
            result.refused = true;
            result.refuseReason = (bestOp == nullptr)
                ? "no safe candidate fit the remaining budget and interaction guard"
                : "no candidate improved likelihood beyond the acceptance floor";
            break;
        }

        auto finalRng = candidateRng (bestOp->id, bestStrength);
        bestOp->apply (buffer, bestStrength, finalRng);
        budgetLeft -= bestDose;
        result.budgetUsed += bestDose;

        StepRecord step;
        step.opId = bestOp->id;
        step.opName = bestOp->displayName;
        step.strength = bestStrength;
        step.dose = bestDose;
        step.likelihoodBefore = current;
        step.likelihoodAfter = bestScore;
        result.steps.push_back (std::move (step));

        current = bestScore;
        currentOutcome = std::move (bestOutcome);

        std::printf ("%4d  %-20s  %8.2f   %4.2f    %.4f\n",
                     iter + 1, bestOp->id.c_str(), bestStrength, bestDose, current);
        std::fflush (stdout);
    }

    if (targetReached (currentOutcome, config.targetLikelihood)
        result.reachedTarget = true;
    else if (! result.refused)
    {
        result.refused = true;
        result.refuseReason = "iteration limit reached without hitting target";
    }

    result.likelihoodAfter = current;
    result.confidenceAfter = currentOutcome.confidence;

    double peakDelta = 0.0, sumSq = 0.0;
    long n = 0;
    const int chans = std::min (buffer.getNumChannels(), original.getNumChannels());
    const int len = std::min (buffer.getNumSamples(), original.getNumSamples());
    for (int c = 0; c < chans; ++c)
    {
        const float* a = original.getReadPointer (c);
        const float* b = buffer.getReadPointer (c);
        for (int i = 0; i < len; ++i)
        {
            const double d = (double) a[i] - (double) b[i];
            peakDelta = std::max (peakDelta, std::abs (d));
            sumSq += d * d;
            ++n;
        }
    }
    result.peakDeltaDbFS = 20.0 * std::log10 (std::max (peakDelta, 1e-12));
    result.rmsDeltaDbFS  = 20.0 * std::log10 (std::sqrt (sumSq / std::max (1L, n)) + 1e-12);

    return result;
}
