#pragma once

#include "../Analysis/AnalysisResult.h"
#include "../Analysis/DetectionProfile.h"

#include <algorithm>
#include <vector>

//==============================================================================
/** Shared plumbing for the per-domain feature evaluators. */
namespace daat::features
{
    /** Returns the profile setting for id, or nullptr if the feature is unknown
        or disabled (in which case the evaluator should skip it entirely). */
    inline const FeatureSetting* enabledSetting (const DetectionProfile& profile,
                                                 const juce::String& id)
    {
        const auto it = profile.features.find (id);
        if (it == profile.features.end() || ! it->second.enabled)
            return nullptr;
        return &it->second;
    }

    /** Appends a FeatureResult. When valid is false the score/confidence are
        forced to zero so downstream code can show the feature as "n/a". */
    inline void addResult (std::vector<FeatureResult>& out,
                           const juce::String& id,
                           double rawValue,
                           bool valid,
                           float suspicion,
                           float confidence,
                           float weight,
                           const juce::String& explanation)
    {
        FeatureResult r;
        r.featureId       = id;
        r.rawValue        = (float) rawValue;
        r.valid           = valid;
        r.suspicionScore  = valid ? suspicion : 0.0f;
        r.normalizedValue = r.suspicionScore;
        r.confidence      = valid ? confidence : 0.0f;
        r.effectiveWeight = weight;
        r.explanation     = explanation;
        out.push_back (std::move (r));
    }

    inline float weightOf (const FeatureSetting& fs) noexcept
    {
        return (float) std::max (0.0, fs.weight);
    }
}
