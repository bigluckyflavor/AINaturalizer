#include "TemporalFeatures.h"
#include "Normalization.h"
#include "FeatureEvalHelpers.h"
#include "../Utility/JsonUtilities.h"

namespace daat::features
{
    void evaluateTemporal (const WindowRawFeatures& raw,
                           const DetectionProfile& profile,
                           int /*channels*/,
                           std::vector<FeatureResult>& out)
    {
        const float signalConf = norm::signalConfidence (raw.rms);

        // --- Transient amplitude variance (excessive uniformity is suspicious) ---
        if (const auto* fs = enabledSetting (profile, "transientVariance"))
        {
            if (raw.transientVariance < 0.0)
            {
                // Too few transients to judge - report but with low confidence so
                // sustained material (pads, drones) is not falsely flagged.
                addResult (out, "transientVariance", 0.0, true, 0.0f, signalConf * 0.2f,
                           weightOf (*fs), "Too few transients to assess variability");
            }
            else
            {
                const double thr = daat::json::getDouble (fs->settings, "suspiciousThreshold", 0.35);
                const float susp = norm::ramp (thr - raw.transientVariance, 0.0, thr);
                addResult (out, "transientVariance", raw.transientVariance, true, susp, signalConf,
                           weightOf (*fs),
                           "Transient variability " + juce::String (raw.transientVariance, 2)
                               + (susp > 0.5f ? " (very uniform)" : ""));
            }
        }
    }
}
