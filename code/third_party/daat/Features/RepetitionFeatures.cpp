#include "RepetitionFeatures.h"
#include "Normalization.h"
#include "FeatureEvalHelpers.h"
#include "../Utility/JsonUtilities.h"

namespace daat::features
{
    void evaluateRepetition (const WindowRawFeatures& raw,
                             const DetectionProfile& profile,
                             int /*channels*/,
                             std::vector<FeatureResult>& out)
    {
        // --- Short-time repetition (high envelope autocorrelation = looped) ---
        if (const auto* fs = enabledSetting (profile, "microRepetition"))
        {
            const double thr = daat::json::getDouble (fs->settings, "suspiciousThreshold", 0.76);
            const float susp = norm::ramp (raw.microRepetition, thr, 1.0);
            const float conf = norm::signalConfidence (raw.rms);
            addResult (out, "microRepetition", raw.microRepetition, true, susp, conf,
                       weightOf (*fs),
                       "Repetition score " + juce::String (raw.microRepetition, 2)
                           + (susp > 0.5f ? " (looped / repeated)" : ""));
        }
    }
}
