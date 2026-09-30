#include "DynamicsFeatures.h"
#include "Normalization.h"
#include "FeatureEvalHelpers.h"
#include "../Utility/JsonUtilities.h"

namespace daat::features
{
    void evaluateDynamics (const WindowRawFeatures& raw,
                           const DetectionProfile& profile,
                           int /*channels*/,
                           std::vector<FeatureResult>& out)
    {
        const float signalConf = norm::signalConfidence (raw.rms);

        // --- Crest factor (very low crest = heavy limiting, mildly suspicious) ---
        if (const auto* fs = enabledSetting (profile, "crestFactor"))
        {
            const double thr = daat::json::getDouble (fs->settings, "suspiciousThresholdDb", 6.0);
            // Suspicion ramps from 0 at the threshold to 1 roughly one span below it.
            const double span = std::max (1.0, thr * 0.7);
            const float susp = norm::ramp (thr - raw.crestFactorDb, 0.0, span);
            addResult (out, "crestFactor", raw.crestFactorDb, true, susp, signalConf,
                       weightOf (*fs),
                       "Crest factor " + juce::String (raw.crestFactorDb, 1) + " dB"
                           + (susp > 0.5f ? " (heavily limited)" : ""));
        }
    }
}
