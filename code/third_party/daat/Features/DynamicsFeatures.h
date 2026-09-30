#pragma once

#include "../Analysis/AnalysisResult.h"
#include "../Analysis/DetectionProfile.h"
#include <vector>

namespace daat::features
{
    void evaluateDynamics (const WindowRawFeatures& raw,
                           const DetectionProfile& profile,
                           int channels,
                           std::vector<FeatureResult>& out);
}
