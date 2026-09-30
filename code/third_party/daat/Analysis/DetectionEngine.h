#pragma once

#include "AnalysisResult.h"
#include "DetectionProfile.h"

#include <vector>

//==============================================================================
/**
    Score combination, confidence, and verdict logic (spec §11-§13).

    Pure functions over already-extracted FeatureResults - no audio, no threads,
    no JUCE components - so the decision logic is unit-testable in isolation.

    Nothing here claims to prove AI origin. "Likelihood" is the weighted
    agreement of heuristic indicators, not a probability of AI authorship.
*/
namespace daat::detect
{
    /** Level-A (APVTS) controls that override profile values at runtime. */
    struct RuntimeControls
    {
        juce::uint32 groupEnableMask = 0xFFFFFFFFu; // bit per FeatureGroup
        float sensitivity        = 0.5f;  // 0.5 = neutral; >0.5 boosts, <0.5 suppresses
        float decisionThreshold  = 0.72f; // overrides profile likelyThreshold
        float minimumConfidence  = 0.55f; // overrides profile minimumConfidence
        bool  applyOverrides     = true;

        bool isGroupEnabled (FeatureGroup g) const noexcept
        {
            return (groupEnableMask & (1u << (juce::uint32) g)) != 0u;
        }
    };

    inline juce::uint32 groupBit (FeatureGroup g) noexcept
    {
        return 1u << (juce::uint32) g;
    }

    //==========================================================================
    /** Context describing the material analysed, used for confidence (§12). */
    struct ConfidenceContext
    {
        double analyzedSeconds  = 0.0;
        double minimumSeconds   = 12.0;
        int    numWindows       = 0;
        int    channels         = 2;
        bool   clipped          = false;
        float  rmsDb            = -100.0f;
        double sourceSampleRate = 48000.0;
    };

    //==========================================================================
    /** Combines features into per-group scores. Groups disabled in the profile or
        masked off by RuntimeControls are omitted entirely. */
    std::vector<GroupScore> computeGroupScores (const std::vector<FeatureResult>& features,
                                                const DetectionProfile& profile,
                                                const RuntimeControls& controls);

    struct OverallScore
    {
        float likelihood       = 0.0f;
        float rawLikelihood    = 0.0f; // before the sensitivity curve
        int   numAgreeingGroups = 0;   // groups independently at/above the likely threshold
        int   numActiveGroups   = 0;
        double activeFeatureWeight = 0.0;
        bool  scorable = false;        // false => nothing valid to combine
    };

    /** Weighted combination of group scores (§11). Each group contributes in
        proportion to groupWeight x groupConfidence, so a group with many
        features cannot dominate on count alone. */
    OverallScore combineGroups (const std::vector<GroupScore>& groups,
                                const DetectionProfile& profile,
                                const RuntimeControls& controls);

    /** Confidence from evidence quality, not just the score (§12). */
    float computeConfidence (const OverallScore& overall,
                             const std::vector<GroupScore>& groups,
                             const DetectionProfile& profile,
                             const ConfidenceContext& context);

    /** Three-state verdict, gated by confidence and group agreement (§11). */
    Verdict decideVerdict (const OverallScore& overall,
                           float confidence,
                           const DetectionProfile& profile,
                           const RuntimeControls& controls);

    /** Ranks features into supporting / contradicting evidence (§11). */
    void extractEvidence (const std::vector<FeatureResult>& features,
                          std::vector<EvidenceItem>& strongest,
                          std::vector<EvidenceItem>& contradictory,
                          int maxItems = 4);

    /** Conditions that can imitate AI-associated artifacts (§13). These are
        caveats, never evidence of AI generation. */
    juce::StringArray detectCaveats (const std::vector<FeatureResult>& features,
                                     const ConfidenceContext& context,
                                     bool truncated);

    /** Scores a single window in place (drives the Phase 6 timeline heat map). */
    void scoreWindow (WindowResult& window,
                      const DetectionProfile& profile,
                      const RuntimeControls& controls);

    /** Plain-language sentence describing what the estimate does and does not
        mean. Always paired with the verdict in the UI and reports. */
    juce::String describeVerdict (Verdict verdict, float likelihood, float confidence);
}
