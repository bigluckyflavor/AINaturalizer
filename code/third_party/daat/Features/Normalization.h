#pragma once

#include <algorithm>
#include <cmath>

//==============================================================================
/**
    Shared helpers that map a raw feature measurement into a 0..1 suspicion
    score. Deliberately simple, soft transitions (spec §10) rather than hard
    booleans. No JUCE dependency so they are trivially unit-testable.

    "Suspicion" here means "suspicious according to THIS feature alone" - it is
    not an AI-likelihood. Combining features into a verdict is Phase 5.
*/
namespace daat::norm
{
    inline double clamp01 (double x) noexcept
    {
        if (! std::isfinite (x)) return 0.0;
        return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
    }

    /** Linear ramp: 0 at/below lo, 1 at/above hi. */
    inline float ramp (double v, double lo, double hi) noexcept
    {
        if (hi <= lo)
            return v >= hi ? 1.0f : 0.0f;
        return (float) clamp01 ((v - lo) / (hi - lo));
    }

    /** Suspicion for a value that should sit inside [low, high]. Outside the
        band, suspicion ramps up over a fraction of the boundary value. */
    inline float outsideRange (double v, double low, double high, double marginFrac) noexcept
    {
        const double belowSpan = std::max (1.0e-9, std::abs (low)  * marginFrac);
        const double aboveSpan = std::max (1.0e-9, std::abs (high) * marginFrac);

        if (v < low)  return ramp (low - v, 0.0, belowSpan);
        if (v > high) return ramp (v - high, 0.0, aboveSpan);
        return 0.0f;
    }

    /** Distance from a reference (human) distribution, in z-scores, with hard
        bounds. Within ~1 sd => 0; beyond ~3 sd or outside [low, high] => 1. */
    inline float distanceFromReference (double v, double mean, double sd,
                                        double low, double high) noexcept
    {
        const double z = std::abs (v - mean) / std::max (1.0e-9, sd);
        float base = ramp (z, 1.0, 3.0);

        float hard = 0.0f;
        if (v < low)  hard = ramp (low - v, 0.0, std::max (1.0e-6, std::abs (low)));
        else if (v > high) hard = ramp (v - high, 0.0, std::max (1.0e-6, 1.0 - high));

        return std::max (base, hard);
    }

    /** Confidence contribution from signal level: near-silent windows carry
        little information. Maps ~ -60 dB..-30 dB RMS to 0..1. */
    inline float signalConfidence (double rms) noexcept
    {
        const double db = 20.0 * std::log10 (std::max (rms, 1.0e-9));
        return (float) clamp01 ((db + 60.0) / 30.0);
    }
}
