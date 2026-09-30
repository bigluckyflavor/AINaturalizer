#include "Optimize.h"

#include <cmath>
#include <cstdio>

Optimizer::Optimizer (Oracle& o, double sr, OptimizeConfig cfg)
    : oracle (o), sampleRate (sr), config (std::move (cfg)), ops (makeOperators (sr))
{
}

OptimizeResult Optimizer::run (juce::AudioBuffer<float>& buffer)
{
    OptimizeResult result;
    juce::AudioBuffer<float> original;
    original.makeCopyOf (buffer);

    result.likelihoodBefore = oracle.score (buffer, sampleRate);
    double current = result.likelihoodBefore;
    double budgetLeft = config.budget;

    std::printf ("iter  op                    strength   dose    likelihood\n");
    std::printf ("----  --------------------  --------   ----    ----------\n");

    for (int iter = 0; iter < config.maxIters; ++iter)
    {
        if (current <= config.targetLikelihood)
        {
            result.reachedTarget = true;
            break;
        }

        // Sweep every (operator, strength) on a scratch copy.
        // Each candidate gets a deterministic RNG stream derived from
        // (seed, iteration, op, strength), so the winning move can be
        // re-applied to the real buffer bit-identically afterward.
        auto candidateRng = [&] (const std::string& opId, double s)
        {
            std::size_t h = std::hash<std::string>{} (opId);
            h ^= std::hash<double>{} (s + 0.5) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<int>{} (iter) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return std::mt19937_64 (config.seed ^ h);
        };

        const PerturbOp* bestOp = nullptr;
        double bestStrength = 0.0, bestScore = current, bestDose = 0.0;

        for (const auto& op : ops)
        {
            for (double s : config.strengths)
            {
                const double dose = s * op.costPerUnit;
                if (dose > budgetLeft + 1e-9)
                    continue; // cannot afford this move

                juce::AudioBuffer<float> trial;
                trial.makeCopyOf (buffer);
                auto trialRng = candidateRng (op.id, s);
                op.apply (trial, s, trialRng);
                const double l = oracle.score (trial, sampleRate);

                if (l < bestScore)
                {
                    bestScore = l;
                    bestOp = &op;
                    bestStrength = s;
                    bestDose = dose;
                }
            }
        }

        const double gain = current - bestScore;
        if (bestOp == nullptr || gain < config.minGain)
        {
            result.refused = true;
            result.refuseReason = (bestOp == nullptr)
                ? "no candidate fit the remaining perceptual budget"
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
        std::printf ("%4d  %-20s  %8.2f   %4.2f    %.4f\n",
                     iter + 1, bestOp->id.c_str(), bestStrength, bestDose, current);
        std::fflush (stdout);
    }

    if (current <= config.targetLikelihood)
        result.reachedTarget = true;
    else if (! result.refused)
    {
        result.refused = true;
        result.refuseReason = "iteration limit reached without hitting target";
    }

    result.likelihoodAfter = current;
    result.confidenceAfter = oracle.analyze (buffer, sampleRate).confidence;

    // Delta metrics vs the original.
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
