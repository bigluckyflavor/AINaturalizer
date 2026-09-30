#include "StereoFeatures.h"
#include "Normalization.h"
#include "FeatureEvalHelpers.h"
#include "../Utility/JsonUtilities.h"

namespace daat::features
{
    void evaluateStereo (const WindowRawFeatures& raw,
                         const DetectionProfile& profile,
                         int channels,
                         std::vector<FeatureResult>& out)
    {
        const bool stereo = channels >= 2;
        const float signalConf = norm::signalConfidence (raw.rms);

        // --- Left/right correlation (unnaturally high, stationary = suspicious) ---
        if (const auto* fs = enabledSetting (profile, "stereoCorrelation"))
        {
            if (! stereo)
            {
                addResult (out, "stereoCorrelation", raw.leftRightCorrelation, false, 0.0f, 0.0f,
                           weightOf (*fs), "n/a (mono input)");
            }
            else
            {
                const double thr = daat::json::getDouble (fs->settings, "suspiciousThreshold", 0.985);
                const float susp = norm::ramp (raw.leftRightCorrelation, thr, 1.0);
                addResult (out, "stereoCorrelation", raw.leftRightCorrelation, true, susp, signalConf,
                           weightOf (*fs),
                           "L/R correlation " + juce::String (raw.leftRightCorrelation, 3));
            }
        }

        // --- Mid/side energy ratio (extreme = suspicious) ---
        if (const auto* fs = enabledSetting (profile, "midSideRatio"))
        {
            if (! stereo)
            {
                addResult (out, "midSideRatio", raw.midSideRatio, false, 0.0f, 0.0f,
                           weightOf (*fs), "n/a (mono input)");
            }
            else
            {
                const double low  = daat::json::getDouble (fs->settings, "suspiciousLow", 0.02);
                const double high = daat::json::getDouble (fs->settings, "suspiciousHigh", 1.4);
                const float susp  = norm::outsideRange (raw.midSideRatio, low, high, 0.5);
                addResult (out, "midSideRatio", raw.midSideRatio, true, susp, signalConf,
                           weightOf (*fs),
                           "Mid/side ratio " + juce::String (raw.midSideRatio, 3));
            }
        }
    }
}
