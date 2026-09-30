#pragma once

#include <cmath>
#include <vector>

//==============================================================================
/**
    Small numeric helpers shared by the feature extractor and scoring code.
    All guard against empty input, NaN, and infinities so callers can stay
    terse. Header-only; no JUCE dependency so it is trivially unit-testable.
*/
namespace daat::stats
{
    inline bool isFinite (double x) noexcept
    {
        return std::isfinite (x);
    }

    /** Returns x if finite, otherwise fallback. */
    inline double sanitize (double x, double fallback = 0.0) noexcept
    {
        return std::isfinite (x) ? x : fallback;
    }

    inline double mean (const float* data, int n) noexcept
    {
        if (data == nullptr || n <= 0)
            return 0.0;

        double sum = 0.0;
        for (int i = 0; i < n; ++i)
            sum += (double) data[i];

        return sum / (double) n;
    }

    inline double variance (const float* data, int n) noexcept
    {
        if (data == nullptr || n <= 1)
            return 0.0;

        const double m = mean (data, n);
        double acc = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double d = (double) data[i] - m;
            acc += d * d;
        }
        return acc / (double) (n - 1);
    }

    inline double stdDev (const float* data, int n) noexcept
    {
        return std::sqrt (variance (data, n));
    }

    inline double mean (const std::vector<double>& v) noexcept
    {
        if (v.empty())
            return 0.0;

        double sum = 0.0;
        for (auto x : v)
            sum += x;

        return sum / (double) v.size();
    }

    inline double variance (const std::vector<double>& v) noexcept
    {
        if (v.size() < 2)
            return 0.0;

        const double m = mean (v);
        double acc = 0.0;
        for (auto x : v)
        {
            const double d = x - m;
            acc += d * d;
        }
        return acc / (double) (v.size() - 1);
    }

    inline double stdDev (const std::vector<double>& v) noexcept
    {
        return std::sqrt (variance (v));
    }

    /** Coefficient of variation (stdDev / |mean|), 0 when mean is ~0. */
    inline double coefficientOfVariation (const std::vector<double>& v) noexcept
    {
        const double m = mean (v);
        if (std::abs (m) < 1.0e-12)
            return 0.0;
        return stdDev (v) / std::abs (m);
    }
}
