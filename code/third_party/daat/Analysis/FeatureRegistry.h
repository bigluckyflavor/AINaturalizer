#pragma once

#include <juce_core/juce_core.h>
#include <vector>

//==============================================================================
enum class FeatureGroup
{
    spectral,
    temporal,
    dynamics,
    stereo,
    noise,
    repetition,
    vocal,
    fingerprint,
    model
};

/** How a feature's raw measurement is turned into a 0..1 suspicion score.
    The scoring itself lands in Phase 5; the mode travels with the descriptor
    so profiles and the settings editor can present it now. */
enum class NormalizationMode
{
    higherIsMoreSuspicious,
    lowerIsMoreSuspicious,
    outsideRangeIsSuspicious,
    distanceFromReferenceDistribution,
    customCurve
};

//==============================================================================
/** Bitmask of analysis scales a feature is evaluated on (spec §6). Bit order
    matches AnalysisScale: short = bit 0, medium = bit 1, long = bit 2. */
namespace ScaleMask
{
    constexpr juce::uint32 shortWindows  = 1u << 0;
    constexpr juce::uint32 mediumWindows = 1u << 1;
    constexpr juce::uint32 longWindows   = 1u << 2;
    constexpr juce::uint32 all           = shortWindows | mediumWindows | longWindows;
}

juce::String  scaleMaskToString (juce::uint32 mask);   // e.g. "short,medium"
juce::uint32  scaleMaskFromString (const juce::String& s, bool& ok);

//==============================================================================
/** Static metadata for one detection feature. The mutable per-profile bits
    (enabled / weight / thresholds) live in DetectionProfile; this is the
    canonical description the profile is built against. */
struct FeatureDescriptor
{
    juce::String id;
    juce::String displayName;
    juce::String description;
    FeatureGroup group = FeatureGroup::spectral;

    bool defaultEnabled = true;
    double defaultWeight = 0.0;
    juce::uint32 defaultScales = ScaleMask::all;
    NormalizationMode normalization = NormalizationMode::higherIsMoreSuspicious;

    /** Default per-feature settings (thresholds, frequency ranges, direction)
        as a JSON object var. */
    juce::var defaultSettings;
};

//==============================================================================
// Enum <-> string helpers (used by JSON profiles and the UI).
juce::String       toString (FeatureGroup group);
FeatureGroup       featureGroupFromString (const juce::String& s, bool& ok);
juce::String       toString (NormalizationMode mode);
NormalizationMode  normalizationFromString (const juce::String& s);

//==============================================================================
/** The canonical list of features the plugin knows about. Order is stable.
    Phase 3 registers the Phase 4 initial feature set with full metadata;
    their evaluation functions are added in Phase 4. */
const std::vector<FeatureDescriptor>& getFeatureRegistry();

/** Returns the descriptor for id, or nullptr if unknown. */
const FeatureDescriptor* findFeatureDescriptor (const juce::String& id);
