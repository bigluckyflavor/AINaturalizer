#include "DetectionEngine.h"
#include "../Utility/JsonUtilities.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace daat::detect
{
namespace
{
    constexpr double kEps = 1.0e-9;

    /** Keeps a confidence factor from collapsing the whole product to zero.
        A single weak dimension should lower confidence, not annihilate it. */
    float floorFactor (double x) noexcept
    {
        if (! std::isfinite (x)) return 0.1f;
        return (float) juce::jlimit (0.10, 1.0, x);
    }

    float effectiveLikelyThreshold (const DetectionProfile& p, const RuntimeControls& c)
    {
        const double t = c.applyOverrides ? (double) c.decisionThreshold
                                          : p.decision.likelyThreshold;
        return (float) juce::jlimit (0.05, 0.99, t);
    }

    float effectiveUnlikelyThreshold (const DetectionProfile& p, const RuntimeControls& c)
    {
        const float likely = effectiveLikelyThreshold (p, c);
        // Keep the inconclusive band non-empty even if a profile sets them badly.
        return (float) juce::jlimit (0.01, (double) likely - 0.02, p.decision.unlikelyThreshold);
    }

    float effectiveMinConfidence (const DetectionProfile& p, const RuntimeControls& c)
    {
        const double m = c.applyOverrides ? (double) c.minimumConfidence
                                          : p.decision.minimumConfidence;
        return (float) juce::jlimit (0.0, 1.0, m);
    }

    /** Sensitivity as a monotonic gamma curve on the likelihood.
        0.5 => unchanged, 1.0 => gamma 0.5 (raises scores), 0.0 => gamma 2. */
    float applySensitivity (float likelihood, float sensitivity)
    {
        const double s = juce::jlimit (0.0, 1.0, (double) sensitivity);
        const double gamma = std::pow (2.0, -(s - 0.5) * 2.0);
        const double l = juce::jlimit (0.0, 1.0, (double) likelihood);
        return (float) juce::jlimit (0.0, 1.0, std::pow (l, gamma));
    }

    const FeatureResult* findFeature (const std::vector<FeatureResult>& features,
                                      const juce::String& id)
    {
        for (const auto& f : features)
            if (f.featureId == id)
                return &f;
        return nullptr;
    }
}

//==============================================================================
std::vector<GroupScore> computeGroupScores (const std::vector<FeatureResult>& features,
                                            const DetectionProfile& profile,
                                            const RuntimeControls& controls)
{
    struct Acc { double num = 0.0, den = 0.0, confSum = 0.0; int n = 0; };
    std::map<int, Acc> byGroup;

    for (const auto& f : features)
    {
        if (! f.valid)
            continue;

        const auto* desc = findFeatureDescriptor (f.featureId);
        if (desc == nullptr)
            continue;

        // Level A (APVTS) mask and Level B (profile) group weight both gate here.
        if (! controls.isGroupEnabled (desc->group))
            continue;
        if (profile.enabledGroup (desc->group) == nullptr)
            continue;

        const double w = (double) juce::jmax (0.0f, f.effectiveWeight);
        const double c = (double) juce::jlimit (0.0f, 1.0f, f.confidence);
        const double contribution = c * w;

        auto& a = byGroup[(int) desc->group];
        a.num     += (double) juce::jlimit (0.0f, 1.0f, f.suspicionScore) * contribution;
        a.den     += contribution;
        a.confSum += c;
        ++a.n;
    }

    std::vector<GroupScore> out;
    for (const auto& [groupIndex, a] : byGroup)
    {
        const auto group = (FeatureGroup) groupIndex;
        const auto* gs = profile.enabledGroup (group);
        if (gs == nullptr)
            continue;

        GroupScore g;
        g.group            = group;
        g.displayName      = toString (group);
        g.weight           = (float) gs->weight;
        g.numValidFeatures = a.n;
        g.confidence       = a.n > 0 ? (float) (a.confSum / (double) a.n) : 0.0f;
        // A group with zero total weight/confidence carries no information.
        g.valid            = a.n > 0 && a.den > kEps;
        g.score            = g.valid ? (float) juce::jlimit (0.0, 1.0, a.num / a.den) : 0.0f;
        out.push_back (g);
    }

    std::sort (out.begin(), out.end(),
               [] (const GroupScore& a, const GroupScore& b) { return a.score > b.score; });
    return out;
}

//==============================================================================
OverallScore combineGroups (const std::vector<GroupScore>& groups,
                            const DetectionProfile& profile,
                            const RuntimeControls& controls)
{
    OverallScore out;
    const float likelyThreshold = effectiveLikelyThreshold (profile, controls);

    double num = 0.0, den = 0.0;

    for (const auto& g : groups)
    {
        if (! g.valid)
            continue;

        // Weight by group weight x group confidence so a many-feature group
        // cannot dominate purely on feature count.
        const double contribution = (double) juce::jmax (0.0f, g.weight)
                                  * (double) juce::jlimit (0.0f, 1.0f, g.confidence);
        if (contribution <= kEps)
            continue;

        num += (double) g.score * contribution;
        den += contribution;

        ++out.numActiveGroups;
        if (g.score >= likelyThreshold)
            ++out.numAgreeingGroups;
    }

    if (den > kEps)
    {
        out.rawLikelihood = (float) juce::jlimit (0.0, 1.0, num / den);
        out.likelihood    = controls.applyOverrides
                              ? applySensitivity (out.rawLikelihood, controls.sensitivity)
                              : out.rawLikelihood;
        out.scorable = true;
    }

    // Total active feature weight gates whether we score at all (§7 decision block).
    out.activeFeatureWeight = profile.totalActiveWeight();
    return out;
}

//==============================================================================
float computeConfidence (const OverallScore& overall,
                         const std::vector<GroupScore>& groups,
                         const DetectionProfile& profile,
                         const ConfidenceContext& context)
{
    if (! overall.scorable)
        return 0.0f;

    // --- Duration: a 12 s clip must not be as confident as a 3 minute track ---
    const double minSecs = juce::jmax (1.0, context.minimumSeconds);
    const float durationFactor = floorFactor (context.analyzedSeconds / (minSecs * 4.0));

    // --- Breadth: how many independent groups contributed ---
    const float groupFactor = floorFactor ((double) overall.numActiveGroups / 5.0);

    // --- Agreement: dispersion across group scores ---
    float agreementFactor = 1.0f;
    {
        std::vector<double> scores;
        for (const auto& g : groups)
            if (g.valid)
                scores.push_back ((double) g.score);

        if (scores.size() >= 2)
        {
            double m = 0.0;
            for (auto s : scores) m += s;
            m /= (double) scores.size();

            double v = 0.0;
            for (auto s : scores) { const double d = s - m; v += d * d; }
            v /= (double) (scores.size() - 1);

            agreementFactor = floorFactor (1.0 - juce::jlimit (0.0, 1.0, std::sqrt (v) / 0.35));
        }
        else
        {
            agreementFactor = 0.5f; // a single group cannot corroborate itself
        }
    }

    // --- Distance from the decision boundary: ~0.5 should be less confident ---
    float boundaryFactor;
    {
        const double likely   = (double) profile.decision.likelyThreshold;
        const double unlikely = (double) profile.decision.unlikelyThreshold;
        const double mid  = 0.5 * (likely + unlikely);
        const double span = juce::jmax (mid, 1.0 - mid);
        boundaryFactor = floorFactor (std::abs ((double) overall.likelihood - mid) / juce::jmax (kEps, span));
    }

    // --- Signal quality ---
    double quality = 1.0;
    if (context.clipped)                 quality *= 0.80;
    if (context.channels < 2)            quality *= 0.90; // stereo features unavailable
    if (context.rmsDb < -45.0f)          quality *= 0.60; // very quiet / near-silent
    if (context.sourceSampleRate < 32000.0) quality *= 0.85;
    if (context.numWindows < 4)          quality *= 0.70;
    const float qualityFactor = floorFactor (quality);

    // Weighted geometric mean: any weak dimension lowers confidence.
    const double confidence =
          std::pow ((double) durationFactor,  0.30)
        * std::pow ((double) groupFactor,     0.15)
        * std::pow ((double) agreementFactor, 0.20)
        * std::pow ((double) boundaryFactor,  0.15)
        * std::pow ((double) qualityFactor,   0.20);

    // Hard gate: too little active feature weight => untrustworthy.
    double gate = 1.0;
    if (overall.activeFeatureWeight < profile.decision.minimumActiveFeatureWeight)
        gate = 0.5;

    return (float) juce::jlimit (0.0, 1.0, confidence * gate);
}

//==============================================================================
Verdict decideVerdict (const OverallScore& overall,
                       float confidence,
                       const DetectionProfile& profile,
                       const RuntimeControls& controls)
{
    if (! overall.scorable)
        return Verdict::Inconclusive;

    // Not enough of the detector is switched on to trust either direction.
    if (overall.activeFeatureWeight < profile.decision.minimumActiveFeatureWeight)
        return Verdict::Inconclusive;

    if (confidence < effectiveMinConfidence (profile, controls))
        return Verdict::Inconclusive;

    const float likely   = effectiveLikelyThreshold (profile, controls);
    const float unlikely = effectiveUnlikelyThreshold (profile, controls);

    if (overall.likelihood >= likely)
    {
        // Require corroboration from independent groups before saying "Likely".
        if (profile.decision.requireMultipleFeatureGroups
            && overall.numAgreeingGroups < juce::jmax (1, profile.decision.minimumAgreeingGroups))
            return Verdict::Inconclusive;

        return Verdict::Likely;
    }

    if (overall.likelihood <= unlikely)
        return Verdict::Unlikely;

    return Verdict::Inconclusive;
}

//==============================================================================
void extractEvidence (const std::vector<FeatureResult>& features,
                      std::vector<EvidenceItem>& strongest,
                      std::vector<EvidenceItem>& contradictory,
                      int maxItems)
{
    strongest.clear();
    contradictory.clear();

    std::vector<EvidenceItem> supporting, opposing;

    for (const auto& f : features)
    {
        if (! f.valid)
            continue;

        const auto* desc = findFeatureDescriptor (f.featureId);
        const double w = (double) juce::jmax (0.0f, f.effectiveWeight);
        const double c = (double) juce::jlimit (0.0f, 1.0f, f.confidence);
        const double s = (double) juce::jlimit (0.0f, 1.0f, f.suspicionScore);

        EvidenceItem item;
        item.featureId   = f.featureId;
        item.displayName = desc != nullptr ? desc->displayName : f.featureId;
        item.detail      = f.explanation;
        item.suspicion   = (float) s;

        if (s > 0.05)
        {
            item.contribution = (float) (s * c * w);
            supporting.push_back (item);
        }
        else
        {
            // Low suspicion with real confidence/weight argues against the estimate.
            item.contribution = (float) ((1.0 - s) * c * w);
            opposing.push_back (item);
        }
    }

    auto byContribution = [] (const EvidenceItem& a, const EvidenceItem& b)
    {
        return a.contribution > b.contribution;
    };
    std::sort (supporting.begin(), supporting.end(), byContribution);
    std::sort (opposing.begin(), opposing.end(), byContribution);

    const int nSup = juce::jmin (maxItems, (int) supporting.size());
    const int nOpp = juce::jmin (maxItems, (int) opposing.size());
    strongest.assign (supporting.begin(), supporting.begin() + nSup);
    contradictory.assign (opposing.begin(), opposing.begin() + nOpp);
}

//==============================================================================
juce::StringArray detectCaveats (const std::vector<FeatureResult>& features,
                                 const ConfidenceContext& context,
                                 bool truncated)
{
    juce::StringArray caveats;

    if (context.clipped)
        caveats.add ("Clipping detected - clipping and brick-wall limiting can imitate "
                     "artifacts associated with synthetic audio.");

    if (context.channels < 2)
        caveats.add ("Mono material - stereo and phase indicators are unavailable, and mono "
                     "audio is not treated as suspicious for lacking stereo complexity.");

    if (context.sourceSampleRate < 32000.0)
        caveats.add ("Low source sample rate (" + juce::String (context.sourceSampleRate / 1000.0, 1)
                     + " kHz) - lossy codecs and band-limiting produce similar spectral traces.");

    if (context.rmsDb < -45.0f)
        caveats.add ("Very low signal level - measurements over near-silent audio are unreliable.");

    if (truncated)
        caveats.add ("Audio was truncated at the capture-length cap; only the analysed portion "
                     "informs this estimate.");

    if (context.analyzedSeconds < context.minimumSeconds * 2.0)
        caveats.add ("Short excerpt - a longer passage would materially improve confidence.");

    // Feature-driven caveats: each of these has ordinary production explanations.
    if (const auto* f = findFeature (features, "crestFactor"))
        if (f->valid && f->rawValue < 6.0f)
            caveats.add ("Very low crest factor (" + juce::String (f->rawValue, 1)
                         + " dB) indicates heavy limiting or loudness maximisation, which is "
                           "routine in commercial mastering.");

    if (const auto* f = findFeature (features, "highFrequencyRatio"))
        if (f->valid && f->rawValue < 0.005f)
            caveats.add ("Very little high-frequency energy - consistent with MP3/AAC "
                         "compression, low bitrate, or deliberate low-pass filtering.");

    if (const auto* f = findFeature (features, "microRepetition"))
        if (f->valid && f->suspicionScore > 0.5f)
            caveats.add ("Strong short-term repetition - loop-based and sample-based human "
                         "production produces this pattern as readily as generated audio.");

    if (const auto* f = findFeature (features, "noiseFloorStationarity"))
        if (f->valid && f->suspicionScore > 0.5f)
            caveats.add ("Highly stationary noise floor - noise reduction, gating, and purely "
                         "synthetic instrumentation all produce this.");

    if (const auto* f = findFeature (features, "stereoCorrelation"))
        if (f->valid && f->rawValue > 0.99f)
            caveats.add ("Near-mono stereo image - mono conversion or a centred mix, not "
                         "necessarily synthetic ambience.");

    // Non-musical material (test tones, sustained drones, single synth notes) trips
    // several indicators at once purely by lacking the texture of a real recording.
    // Flag it loudly rather than letting it inflate the estimate unexplained.
    {
        const auto* flux  = findFeature (features, "spectralFlux");
        const auto* trans = findFeature (features, "transientVariance");
        const auto* noise = findFeature (features, "noiseFloorStationarity");

        const bool noFlux       = flux  != nullptr && flux->valid  && flux->rawValue  < 0.01f;
        const bool noTransients = trans != nullptr && trans->valid && trans->rawValue <= 0.0f;
        const bool deadFloor    = noise != nullptr && noise->valid && noise->rawValue > 0.95f;

        if (noFlux && (noTransients || deadFloor))
            caveats.add ("This does not look like a normal music recording: near-zero spectral "
                         "change with no detected transients. Test tones, single sustained notes, "
                         "and drones trigger several indicators at once for reasons unrelated to "
                         "how the audio was created - treat any elevated score here as an "
                         "artifact of the material, not a finding.");
    }

    caveats.add ("Synthesizers, drum machines, sample libraries, stem separation, and AI "
                 "mastering can all produce these characteristics in human-made music.");

    return caveats;
}

//==============================================================================
void scoreWindow (WindowResult& window,
                  const DetectionProfile& profile,
                  const RuntimeControls& controls)
{
    const auto groups  = computeGroupScores (window.features, profile, controls);
    const auto overall = combineGroups (groups, profile, controls);

    window.likelihood = overall.likelihood;

    // Per-window confidence is intentionally simpler than the overall figure:
    // it reflects only evidence breadth and agreement within this window.
    if (! overall.scorable)
    {
        window.confidence = 0.0f;
        return;
    }

    double confSum = 0.0;
    int    valid   = 0;
    std::vector<double> scores;
    for (const auto& g : groups)
    {
        if (! g.valid) continue;
        confSum += (double) g.confidence;
        scores.push_back ((double) g.score);
        ++valid;
    }

    if (valid == 0)
    {
        window.confidence = 0.0f;
        return;
    }

    double agreement = 1.0;
    if (scores.size() >= 2)
    {
        double m = 0.0;
        for (auto s : scores) m += s;
        m /= (double) scores.size();
        double v = 0.0;
        for (auto s : scores) { const double d = s - m; v += d * d; }
        v /= (double) (scores.size() - 1);
        agreement = juce::jlimit (0.0, 1.0, 1.0 - std::sqrt (v) / 0.35);
    }

    const double breadth = juce::jlimit (0.0, 1.0, (double) valid / 5.0);
    const double meanConf = confSum / (double) valid;

    window.confidence = (float) juce::jlimit (0.0, 1.0,
        std::pow (meanConf, 0.5) * std::pow (juce::jmax (0.1, agreement), 0.3)
        * std::pow (juce::jmax (0.1, breadth), 0.2));
}

//==============================================================================
juce::String describeVerdict (Verdict verdict, float likelihood, float confidence)
{
    const auto pct = [] (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; };

    switch (verdict)
    {
        case Verdict::Likely:
            return "Several independent indicator groups agree (score " + pct (likelihood)
                 + ", confidence " + pct (confidence) + "). This is a screening flag for "
                   "further review, not evidence of AI generation.";

        case Verdict::Unlikely:
            return "Indicators do not show the measured patterns (score " + pct (likelihood)
                 + ", confidence " + pct (confidence) + "). This does not rule out AI "
                   "involvement - it means these heuristics found nothing notable.";

        case Verdict::Inconclusive:
            return "Evidence is mixed or insufficient (score " + pct (likelihood)
                 + ", confidence " + pct (confidence) + "). No determination either way.";

        case Verdict::InsufficientAudio:
            return "Not enough audio was analysed to form an estimate.";

        case Verdict::AnalysisFailed:
            return "Analysis failed before an estimate could be produced.";
    }

    return {};
}

} // namespace daat::detect
