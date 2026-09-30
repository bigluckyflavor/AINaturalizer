#pragma once

#include <juce_core/juce_core.h>
#include "FeatureRegistry.h"
#include <vector>

//==============================================================================
enum class AnalysisScale { shortScale, mediumScale, longScale };

enum class Verdict
{
    Unlikely,
    Inconclusive,
    Likely,
    InsufficientAudio,
    AnalysisFailed
};

juce::String toString (AnalysisScale scale);
juce::String toString (Verdict verdict);

//==============================================================================
/** Per-feature outcome for one window, or averaged across windows. */
struct FeatureResult
{
    juce::String featureId;

    float rawValue        = 0.0f;
    float normalizedValue = 0.0f;
    float suspicionScore  = 0.0f;
    float confidence      = 0.0f;
    float effectiveWeight = 0.0f;

    bool valid = false;

    juce::String explanation;
};

//==============================================================================
/** Score for one feature group, combined from its member features. */
struct GroupScore
{
    FeatureGroup group = FeatureGroup::spectral;
    juce::String displayName;

    float score      = 0.0f;   // 0..1 group suspicion
    float confidence = 0.0f;   // 0..1 mean feature confidence
    float weight     = 0.0f;   // group weight from the profile

    int  numValidFeatures = 0;
    bool valid = false;        // false => no valid features, excluded from scoring
};

//==============================================================================
/** Raw, unscored measurements for one analysis window.
    Stereo fields are only meaningful when channels == 2. */
struct WindowRawFeatures
{
    double rms                 = 0.0;
    double peak                = 0.0;
    double crestFactorDb       = 0.0;
    double spectralCentroidHz  = 0.0;
    double spectralFlatness    = 0.0;
    double spectralRolloffHz   = 0.0;
    double highFrequencyRatio  = 0.0;
    double spectralFlux        = 0.0;
    double leftRightCorrelation = 1.0;
    double midSideRatio        = 0.0;

    double transientVariance     = -1.0;  // coeff. of variation of onsets; <0 = too few transients
    double noiseFloorStationarity = 0.0;  // 0..1, 1 = unchanging noise floor
    double microRepetition        = 0.0;  // 0..1, envelope autocorrelation peak
};

//==============================================================================
struct WindowResult
{
    double startSeconds = 0.0;
    double endSeconds   = 0.0;
    AnalysisScale scale = AnalysisScale::shortScale;

    float likelihood = 0.0f;
    float confidence = 0.0f;

    WindowRawFeatures raw;
    std::vector<FeatureResult> features;
};

//==============================================================================
/** One line of human-readable evidence for or against the estimate. */
struct EvidenceItem
{
    juce::String featureId;
    juce::String displayName;
    juce::String detail;
    float suspicion    = 0.0f;
    float contribution = 0.0f; // suspicion x confidence x weight
};

//==============================================================================
/** Immutable snapshot handed to the UI once analysis completes. The UI holds
    it via shared_ptr<const AnalysisResult> and never mutates it. */
struct AnalysisResult
{
    /** Increments every time the engine publishes a result (analysis or
        rescore), so observers can tell a new result from a stale one. */
    int generation = 0;

    Verdict verdict = Verdict::Inconclusive;

    float overallLikelihood = 0.0f;
    float confidence        = 0.0f;

    int numValidFeatures  = 0;
    int numAgreeingGroups = 0;
    int numActiveGroups   = 0;
    int windowsUsed       = 0;          // windows that fed the overall score (after exclusions)

    double analyzedSeconds     = 0.0;   // length of the analysed region
    double effectiveSeconds    = 0.0;   // analysed length minus excluded sections
    double timelineSeconds     = 0.0;   // length of the whole capture/file
    double rangeStartSeconds   = 0.0;   // analysed region, in timeline seconds
    double rangeEndSeconds     = 0.0;
    bool   isRangeAnalysis     = false; // true when only a selected range was analysed

    // Provenance (spec §17). Audio itself is never stored or exported.
    juce::String sourceName;            // file name, or "DAW capture"
    juce::String sourcePath;            // full path for files, empty for captures
    juce::String sourceFileHash;        // SHA-256 of the source file, empty for captures

    double analysisSampleRate  = 0.0;   // rate features were computed at (after resampling)
    double sourceSampleRate    = 0.0;   // native rate of the captured/loaded audio
    int    channels            = 0;

    // Quick level summary (also used for caveat detection and rescoring).
    float peakLevel = 0.0f;
    float rmsDb     = -100.0f;
    bool  clipped   = false;
    bool  truncated = false;

    /** Timeline sections the user excluded from the overall score. */
    std::vector<juce::Range<double>> excludedRanges;

    std::vector<WindowResult> shortWindows;
    std::vector<WindowResult> mediumWindows;
    std::vector<WindowResult> longWindows;

    /** Per-feature results averaged across all valid windows. */
    std::vector<FeatureResult> featureAverages;

    /** Per-group scores feeding the overall estimate. */
    std::vector<GroupScore> groupScores;

    std::vector<EvidenceItem> strongestEvidence;      // most suspicious contributors
    std::vector<EvidenceItem> contradictoryEvidence;  // least suspicious / counter-indicators

    juce::StringArray caveats;      // conditions that can imitate AI artifacts
    juce::StringArray limitations;  // standing scientific caveats
    juce::String summary;

    int totalWindows() const noexcept
    {
        return (int) (shortWindows.size() + mediumWindows.size() + longWindows.size());
    }

    const std::vector<WindowResult>& windowsFor (AnalysisScale scale) const noexcept
    {
        switch (scale)
        {
            case AnalysisScale::mediumScale: return mediumWindows;
            case AnalysisScale::longScale:   return longWindows;
            case AnalysisScale::shortScale:  break;
        }
        return shortWindows;
    }

    /** True if more than half of [start, end) lies inside an excluded range. */
    bool isExcluded (double start, double end) const noexcept
    {
        const double len = end - start;
        if (len <= 0.0 || excludedRanges.empty())
            return false;

        double covered = 0.0;
        for (const auto& r : excludedRanges)
            covered += r.getIntersectionWith ({ start, end }).getLength();
        return covered > 0.5 * len;
    }
};
