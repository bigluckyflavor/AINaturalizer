#pragma once

#include "../Analysis/AnalysisResult.h"
#include "../Analysis/DetectionProfile.h"
#include <vector>

namespace daat::features
{
    /** Evaluates stereo features. For mono input (channels < 2) the features are
        appended as invalid (n/a) so ordinary mono audio is never flagged. */
    void evaluateStereo (const WindowRawFeatures& raw,
                         const DetectionProfile& profile,
                         int channels,
                         std::vector<FeatureResult>& out);
}
