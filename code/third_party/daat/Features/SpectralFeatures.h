#pragma once

#include "../Analysis/AnalysisResult.h"
#include "../Analysis/DetectionProfile.h"
#include <vector>

namespace daat::features
{
    /** Evaluates spectral features (centroid, flatness, flux, HF ratio) for one
        window, appending a FeatureResult for each enabled feature. */
    void evaluateSpectral (const WindowRawFeatures& raw,
                           const DetectionProfile& profile,
                           int channels,
                           std::vector<FeatureResult>& out);
}
