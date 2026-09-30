#include "FeatureEvaluation.h"

#include "SpectralFeatures.h"
#include "DynamicsFeatures.h"
#include "TemporalFeatures.h"
#include "StereoFeatures.h"
#include "NoiseFeatures.h"
#include "RepetitionFeatures.h"

#include <algorithm>

namespace daat::features
{
    std::vector<FeatureResult> evaluateWindow (const WindowRawFeatures& raw,
                                               const DetectionProfile& profile,
                                               int channels,
                                               juce::uint32 scaleMask)
    {
        std::vector<FeatureResult> out;
        out.reserve (12);

        evaluateSpectral   (raw, profile, channels, out);
        evaluateDynamics   (raw, profile, channels, out);
        evaluateTemporal   (raw, profile, channels, out);
        evaluateStereo     (raw, profile, channels, out);
        evaluateNoise      (raw, profile, channels, out);
        evaluateRepetition (raw, profile, channels, out);

        // Drop features not assigned to this window's scale.
        out.erase (std::remove_if (out.begin(), out.end(),
                                   [&profile, scaleMask] (const FeatureResult& r)
                                   {
                                       const auto it = profile.features.find (r.featureId);
                                       return it == profile.features.end()
                                           || (it->second.scales & scaleMask) == 0;
                                   }),
                   out.end());

        return out;
    }
}
