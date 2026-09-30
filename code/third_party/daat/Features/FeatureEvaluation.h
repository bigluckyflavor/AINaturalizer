#pragma once

#include "../Analysis/AnalysisResult.h"
#include "../Analysis/DetectionProfile.h"
#include <vector>

namespace daat::features
{
    /** Bit for one analysis scale, matching ScaleMask. */
    inline juce::uint32 scaleBit (AnalysisScale scale) noexcept
    {
        return 1u << (juce::uint32) scale;
    }

    /** Runs every enabled feature evaluator over one window's raw measurements
        and returns the per-feature results. Only features assigned to one of the
        scales in `scaleMask` are returned (spec §6 window assignment). */
    std::vector<FeatureResult> evaluateWindow (const WindowRawFeatures& raw,
                                               const DetectionProfile& profile,
                                               int channels,
                                               juce::uint32 scaleMask = ScaleMask::all);
}
