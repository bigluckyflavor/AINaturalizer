#include "SpectralFeatures.h"
#include "Normalization.h"
#include "FeatureEvalHelpers.h"
#include "../Utility/JsonUtilities.h"

namespace daat::features
{
    void evaluateSpectral (const WindowRawFeatures& raw,
                           const DetectionProfile& profile,
                           int /*channels*/,
                           std::vector<FeatureResult>& out)
    {
        const float signalConf = norm::signalConfidence (raw.rms);

        // --- Spectral centroid (outside a typical range is mildly suspicious) ---
        if (const auto* fs = enabledSetting (profile, "spectralCentroid"))
        {
            const double low  = daat::json::getDouble (fs->settings, "suspiciousLowHz", 400.0);
            const double high = daat::json::getDouble (fs->settings, "suspiciousHighHz", 7000.0);
            const float susp  = norm::outsideRange (raw.spectralCentroidHz, low, high, 0.5);
            addResult (out, "spectralCentroid", raw.spectralCentroidHz, true, susp, signalConf,
                       weightOf (*fs),
                       "Centroid " + juce::String (raw.spectralCentroidHz, 0) + " Hz (typical "
                           + juce::String (low, 0) + "-" + juce::String (high, 0) + " Hz)");
        }

        // --- Spectral flatness (distance from a human-music distribution) ---
        if (const auto* fs = enabledSetting (profile, "spectralFlatness"))
        {
            const double mean = daat::json::getDouble (fs->settings, "expectedHumanMean", 0.21);
            const double sd   = daat::json::getDouble (fs->settings, "expectedHumanStdDev", 0.08);
            const double low  = daat::json::getDouble (fs->settings, "suspiciousLow", 0.02);
            const double high = daat::json::getDouble (fs->settings, "suspiciousHigh", 0.65);
            const float susp  = norm::distanceFromReference (raw.spectralFlatness, mean, sd, low, high);
            addResult (out, "spectralFlatness", raw.spectralFlatness, true, susp, signalConf,
                       weightOf (*fs),
                       "Flatness " + juce::String (raw.spectralFlatness, 3)
                           + " (human ~" + juce::String (mean, 2) + ")");
        }

        // --- Spectral flux (unusually low, uniform flux is suspicious) ---
        if (const auto* fs = enabledSetting (profile, "spectralFlux"))
        {
            const double thr = daat::json::getDouble (fs->settings, "suspiciousThreshold", 0.05);
            const float susp = norm::ramp (thr - raw.spectralFlux, 0.0, thr);
            addResult (out, "spectralFlux", raw.spectralFlux, true, susp, signalConf,
                       weightOf (*fs),
                       "Flux " + juce::String (raw.spectralFlux, 4)
                           + (susp > 0.5f ? " (low / uniform)" : ""));
        }

        // --- High-frequency energy ratio (outside range) ---
        if (const auto* fs = enabledSetting (profile, "highFrequencyRatio"))
        {
            const double low  = daat::json::getDouble (fs->settings, "suspiciousLow", 0.002);
            const double high = daat::json::getDouble (fs->settings, "suspiciousHigh", 0.45);
            const float susp  = norm::outsideRange (raw.highFrequencyRatio, low, high, 0.5);
            addResult (out, "highFrequencyRatio", raw.highFrequencyRatio, true, susp, signalConf,
                       weightOf (*fs),
                       "HF ratio " + juce::String (raw.highFrequencyRatio, 4));
        }
    }
}
