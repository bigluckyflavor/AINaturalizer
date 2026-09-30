#include "FeatureRegistry.h"

//==============================================================================
juce::String toString (FeatureGroup group)
{
    switch (group)
    {
        case FeatureGroup::spectral:    return "spectral";
        case FeatureGroup::temporal:    return "temporal";
        case FeatureGroup::dynamics:    return "dynamics";
        case FeatureGroup::stereo:      return "stereo";
        case FeatureGroup::noise:       return "noise";
        case FeatureGroup::repetition:  return "repetition";
        case FeatureGroup::vocal:       return "vocal";
        case FeatureGroup::fingerprint: return "fingerprint";
        case FeatureGroup::model:       return "model";
    }
    return "spectral";
}

FeatureGroup featureGroupFromString (const juce::String& s, bool& ok)
{
    ok = true;
    if (s == "spectral")    return FeatureGroup::spectral;
    if (s == "temporal")    return FeatureGroup::temporal;
    if (s == "dynamics")    return FeatureGroup::dynamics;
    if (s == "stereo")      return FeatureGroup::stereo;
    if (s == "noise")       return FeatureGroup::noise;
    if (s == "repetition")  return FeatureGroup::repetition;
    if (s == "vocal")       return FeatureGroup::vocal;
    if (s == "fingerprint") return FeatureGroup::fingerprint;
    if (s == "model")       return FeatureGroup::model;
    ok = false;
    return FeatureGroup::spectral;
}

juce::String toString (NormalizationMode mode)
{
    switch (mode)
    {
        case NormalizationMode::higherIsMoreSuspicious:            return "higherIsMoreSuspicious";
        case NormalizationMode::lowerIsMoreSuspicious:             return "lowerIsMoreSuspicious";
        case NormalizationMode::outsideRangeIsSuspicious:          return "outsideRangeIsSuspicious";
        case NormalizationMode::distanceFromReferenceDistribution: return "distanceFromReferenceDistribution";
        case NormalizationMode::customCurve:                       return "customCurve";
    }
    return "higherIsMoreSuspicious";
}

NormalizationMode normalizationFromString (const juce::String& s)
{
    if (s == "lowerIsMoreSuspicious")             return NormalizationMode::lowerIsMoreSuspicious;
    if (s == "outsideRangeIsSuspicious")          return NormalizationMode::outsideRangeIsSuspicious;
    if (s == "distanceFromReferenceDistribution") return NormalizationMode::distanceFromReferenceDistribution;
    if (s == "customCurve")                       return NormalizationMode::customCurve;
    return NormalizationMode::higherIsMoreSuspicious;
}

//==============================================================================
juce::String scaleMaskToString (juce::uint32 mask)
{
    juce::StringArray parts;
    if (mask & ScaleMask::shortWindows)  parts.add ("short");
    if (mask & ScaleMask::mediumWindows) parts.add ("medium");
    if (mask & ScaleMask::longWindows)   parts.add ("long");
    return parts.joinIntoString (",");
}

juce::uint32 scaleMaskFromString (const juce::String& s, bool& ok)
{
    ok = true;
    juce::uint32 mask = 0;
    for (auto token : juce::StringArray::fromTokens (s, ",", ""))
    {
        token = token.trim();
        if (token.isEmpty())                continue;
        if      (token == "short")          mask |= ScaleMask::shortWindows;
        else if (token == "medium")         mask |= ScaleMask::mediumWindows;
        else if (token == "long")           mask |= ScaleMask::longWindows;
        else                                ok = false;
    }
    return mask;
}

//==============================================================================
namespace
{
    // Small helper for building a settings var object inline.
    juce::var makeSettings (std::initializer_list<std::pair<const char*, juce::var>> entries)
    {
        auto* obj = new juce::DynamicObject();
        for (const auto& e : entries)
            obj->setProperty (juce::Identifier (e.first), e.second);
        return juce::var (obj);
    }

    FeatureDescriptor makeDescriptor (const char* id,
                                      const char* name,
                                      const char* description,
                                      FeatureGroup group,
                                      double weight,
                                      NormalizationMode norm,
                                      juce::var settings)
    {
        FeatureDescriptor d;
        d.id             = id;
        d.displayName    = name;
        d.description    = description;
        d.group          = group;
        d.defaultEnabled = true;
        d.defaultWeight  = weight;
        d.normalization  = norm;
        d.defaultSettings = std::move (settings);

        // Spec §6 window assignment: short windows for spectral texture,
        // transients and noise; medium for dynamics and stereo consistency;
        // long for structural repetition. Most features also run one scale up
        // so each lane has enough groups to score.
        using namespace ScaleMask;
        switch (group)
        {
            case FeatureGroup::spectral:
            case FeatureGroup::temporal:
            case FeatureGroup::noise:      d.defaultScales = shortWindows | mediumWindows; break;
            case FeatureGroup::dynamics:
            case FeatureGroup::stereo:
            case FeatureGroup::repetition: d.defaultScales = mediumWindows | longWindows;  break;
            case FeatureGroup::vocal:
            case FeatureGroup::fingerprint:
            case FeatureGroup::model:      d.defaultScales = all;                         break;
        }
        return d;
    }
}

//==============================================================================
const std::vector<FeatureDescriptor>& getFeatureRegistry()
{
    // Built once, then const. These are the Phase 4 "initial feature set".
    // Thresholds/weights are UNVALIDATED research defaults - see the profile
    // notes. Direction/thresholds live in defaultSettings so the profile
    // editor can expose them without code changes.
    static const std::vector<FeatureDescriptor> registry =
    {
        makeDescriptor (
            "spectralCentroid", "Spectral Centroid",
            "Average 'brightness' of the spectrum. Extreme or unusually stable "
            "values can accompany synthetic material, but also bright or dark mixes.",
            FeatureGroup::spectral, 0.08, NormalizationMode::outsideRangeIsSuspicious,
            makeSettings ({ { "direction", "outsideRangeIsSuspicious" },
                            { "suspiciousLowHz", 400.0 },
                            { "suspiciousHighHz", 7000.0 } })),

        makeDescriptor (
            "spectralFlatness", "Spectral Flatness",
            "Tonal vs. noise-like balance. Distance from a typical human-music "
            "distribution is treated as mildly suspicious.",
            FeatureGroup::spectral, 0.08, NormalizationMode::distanceFromReferenceDistribution,
            makeSettings ({ { "direction", "distanceFromHumanDistribution" },
                            { "expectedHumanMean", 0.21 },
                            { "expectedHumanStdDev", 0.08 },
                            { "suspiciousLow", 0.02 },
                            { "suspiciousHigh", 0.65 } })),

        makeDescriptor (
            "spectralFlux", "Spectral Flux",
            "Frame-to-frame spectral change. Unusually low, uniform flux can "
            "indicate over-smoothed or looped content.",
            FeatureGroup::spectral, 0.07, NormalizationMode::lowerIsMoreSuspicious,
            makeSettings ({ { "direction", "lowerIsMoreSuspicious" },
                            { "suspiciousThreshold", 0.05 } })),

        makeDescriptor (
            "highFrequencyRatio", "High-Frequency Energy Ratio",
            "Share of energy above a cutoff. Extreme values may reflect codec "
            "damage, brick-wall limiting, or synthesis - not AI specifically.",
            FeatureGroup::spectral, 0.08, NormalizationMode::outsideRangeIsSuspicious,
            makeSettings ({ { "direction", "outsideRangeIsSuspicious" },
                            { "cutoffHz", 12000.0 },
                            { "suspiciousLow", 0.002 },
                            { "suspiciousHigh", 0.45 } })),

        makeDescriptor (
            "crestFactor", "Crest Factor",
            "Peak-to-RMS ratio. Very low crest factor indicates heavy limiting, "
            "which is common in both AI and human mastering.",
            FeatureGroup::dynamics, 0.10, NormalizationMode::lowerIsMoreSuspicious,
            makeSettings ({ { "direction", "lowerIsMoreSuspicious" },
                            { "suspiciousThresholdDb", 6.0 } })),

        makeDescriptor (
            "transientVariance", "Transient Amplitude Variance",
            "Variation across detected transients. Excessively uniform transients "
            "can accompany programmed or generated rhythms.",
            FeatureGroup::temporal, 0.10, NormalizationMode::lowerIsMoreSuspicious,
            makeSettings ({ { "direction", "lowerIsMoreSuspicious" },
                            { "suspiciousThreshold", 0.35 } })),

        makeDescriptor (
            "stereoCorrelation", "Left/Right Correlation",
            "Inter-channel correlation. Unnaturally stationary, highly correlated "
            "stereo can accompany synthetic ambience. Skipped for mono input.",
            FeatureGroup::stereo, 0.09, NormalizationMode::higherIsMoreSuspicious,
            makeSettings ({ { "direction", "higherIsMoreSuspicious" },
                            { "suspiciousThreshold", 0.985 } })),

        makeDescriptor (
            "midSideRatio", "Mid/Side Energy Ratio",
            "Balance of centre vs. side energy. Extreme values may indicate "
            "aggressive widening or near-mono content. Skipped for mono input.",
            FeatureGroup::stereo, 0.06, NormalizationMode::outsideRangeIsSuspicious,
            makeSettings ({ { "direction", "outsideRangeIsSuspicious" },
                            { "suspiciousLow", 0.02 },
                            { "suspiciousHigh", 1.4 } })),

        makeDescriptor (
            "noiseFloorStationarity", "Noise-Floor Stationarity",
            "How unchanging the low-level noise floor is over time. Very high "
            "stationarity can accompany synthetic or denoised material.",
            FeatureGroup::noise, 0.10, NormalizationMode::higherIsMoreSuspicious,
            makeSettings ({ { "direction", "higherIsMoreSuspicious" },
                            { "suspiciousThreshold", 0.82 } })),

        makeDescriptor (
            "microRepetition", "Short-Time Repetition",
            "Short-lag self-similarity. High values flag looped or near-duplicated "
            "material - common in loop-based human production too.",
            FeatureGroup::repetition, 0.10, NormalizationMode::higherIsMoreSuspicious,
            makeSettings ({ { "direction", "higherIsMoreSuspicious" },
                            { "minimumLagMs", 20.0 },
                            { "maximumLagMs", 2000.0 },
                            { "suspiciousThreshold", 0.76 } }))
    };

    return registry;
}

const FeatureDescriptor* findFeatureDescriptor (const juce::String& id)
{
    for (const auto& d : getFeatureRegistry())
        if (d.id == id)
            return &d;
    return nullptr;
}
