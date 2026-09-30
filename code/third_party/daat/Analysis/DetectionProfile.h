#pragma once

#include <juce_core/juce_core.h>
#include "FeatureRegistry.h"
#include <map>

//==============================================================================
/** Result of parsing/validating a profile. Never throws; collects human-
    readable problems for display in the settings editor. */
struct ProfileValidation
{
    bool ok = true;
    juce::StringArray errors;    // block load/use
    juce::StringArray warnings;  // allowed but flagged

    int  loadedVersion = 0;
    bool migrated = false;

    void addError   (const juce::String& m) { errors.add (m);   ok = false; }
    void addWarning (const juce::String& m) { warnings.add (m); }
};

//==============================================================================
struct AnalysisSettings
{
    double sampleRate             = 48000.0;
    int    fftSize                = 2048;
    int    hopSize                = 512;
    double shortWindowSeconds     = 2.0;
    double mediumWindowSeconds    = 8.0;
    double longWindowSeconds      = 30.0;
    double shortOverlap           = 0.75;
    double mediumOverlap          = 0.50;
    double longOverlap            = 0.50;
    double minimumAnalyzedSeconds = 12.0;
};

struct DecisionSettings
{
    double likelyThreshold             = 0.72;
    double unlikelyThreshold           = 0.28;
    double minimumConfidence           = 0.55;
    double minimumActiveFeatureWeight  = 0.40;
    bool   requireMultipleFeatureGroups = true;
    int    minimumAgreeingGroups       = 3;
};

/** Per-profile overrides for one feature. `settings` holds the remaining
    per-feature keys (thresholds, ranges, direction) as a JSON object var. */
struct FeatureSetting
{
    bool   enabled = true;
    double weight  = 0.0;
    juce::uint32 scales = ScaleMask::all;  // which window scales evaluate this feature
    juce::var settings;

    // juce::var holds DynamicObjects by reference, so a default copy would make
    // two profiles share (and co-mutate) the same settings object - e.g. an
    // editor's working copy editing the engine's live profile. Copies are deep.
    FeatureSetting() = default;
    FeatureSetting (const FeatureSetting& o)
        : enabled (o.enabled), weight (o.weight), scales (o.scales), settings (o.settings.clone()) {}
    FeatureSetting& operator= (const FeatureSetting& o)
    {
        if (this != &o)
        {
            enabled  = o.enabled;
            weight   = o.weight;
            scales   = o.scales;
            settings = o.settings.clone();
        }
        return *this;
    }
    FeatureSetting (FeatureSetting&&) noexcept = default;
    FeatureSetting& operator= (FeatureSetting&&) noexcept = default;
};

/** Group-level weight, so a group containing many features cannot overwhelm
    the others (spec §11). */
struct GroupSetting
{
    bool   enabled = true;
    double weight  = 1.0;
};

//==============================================================================
/**
    Editable detection profile (spec §7 Level B). Backed by JSON via juce::var
    - no third-party library. Supports load/save, validation, versioning, and
    a migration hook for older formats. The factory default is built from the
    FeatureRegistry so registry and profile never drift apart.
*/
class DetectionProfile
{
public:
    /** 2: group weights, per-feature window assignment, CV-based transient
           threshold (Phases 4-6). 1: Phase 3 schema. */
    static constexpr int currentVersion = 2;

    juce::String profileName = "Balanced Research";
    int profileVersion = currentVersion;

    AnalysisSettings analysis;
    DecisionSettings decision;
    std::map<juce::String, FeatureSetting> features; // keyed by feature id
    std::map<juce::String, GroupSetting> groups;     // keyed by group name (toString(FeatureGroup))

    /** Group setting for a group, or nullptr if absent/disabled. */
    const GroupSetting* enabledGroup (FeatureGroup group) const;

    /** A numeric per-feature setting, or fallback if missing / not numeric. */
    double featureParam (const juce::String& featureId, const char* key, double fallback) const;

    // Settings that change the raw measurement itself (not just its scoring),
    // so editing them requires re-extracting features from the audio.
    double hfCutoffHz() const          { return featureParam ("highFrequencyRatio", "cutoffHz", 12000.0); }
    double repetitionMinLagMs() const  { return featureParam ("microRepetition", "minimumLagMs", 20.0); }
    double repetitionMaxLagMs() const  { return featureParam ("microRepetition", "maximumLagMs", 2000.0); }

    /** True if moving from `other` to this profile changes raw measurements
        (analysis block or extractor settings) and so needs full reanalysis,
        rather than a cheap rescore of stored measurements. */
    bool requiresReanalysisComparedTo (const DetectionProfile& other) const;

    //==========================================================================
    /** The built-in, unvalidated research default. */
    static DetectionProfile getFactoryDefault();

    //==========================================================================
    // Serialization.
    juce::var    toVar() const;
    juce::String toJsonString() const;

    /** Parses from a var. Missing keys fall back to factory defaults; problems
        are reported in `result`. Applies migration if the source is an older
        (but supported) version. */
    static DetectionProfile fromVar (const juce::var& source, ProfileValidation& result);
    static DetectionProfile fromJsonString (const juce::String& json, ProfileValidation& result);

    //==========================================================================
    // Files.
    bool saveToFile (const juce::File& file, juce::String& errorMessage) const;
    static DetectionProfile loadFromFile (const juce::File& file, ProfileValidation& result);

    //==========================================================================
    /** Structural / range validation (spec §15). */
    ProfileValidation validate() const;

    /** Sum of weights over enabled features. */
    double totalActiveWeight() const;

private:
    /** In-place migration of a parsed var from an older version to current.
        Records what changed in `result`. */
    static void migrate (juce::var& source, int fromVersion, ProfileValidation& result);
};
