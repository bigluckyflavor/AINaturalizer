#include "NoiseFeatures.h"
#include "Normalization.h"
#include "FeatureEvalHelpers.h"
#include "../Utility/JsonUtilities.h"

namespace daat::features
{
    void evaluateNoise (const WindowRawFeatures& raw,
                        const DetectionProfile& profile,
                        int /*channels*/,
                        std::vector<FeatureResult>& out)
    {
        // --- Noise-floor stationarity (very unchanging floor = suspicious) ---
        if (const auto* fs = enabledSetting (profile, "noiseFloorStationarity"))
        {
            const double thr = daat::json::getDouble (fs->settings, "suspiciousThreshold", 0.82);
            const float susp = norm::ramp (raw.noiseFloorStationarity, thr, 1.0);
            // Confidence is reduced for near-silent windows, where a "stationary
            // floor" is just silence rather than a synthetic characteristic.
            const float conf = norm::signalConfidence (raw.rms);
            addResult (out, "noiseFloorStationarity", raw.noiseFloorStationarity, true, susp, conf,
                       weightOf (*fs),
                       "Noise-floor stationarity " + juce::String (raw.noiseFloorStationarity, 2));
        }
    }
}
