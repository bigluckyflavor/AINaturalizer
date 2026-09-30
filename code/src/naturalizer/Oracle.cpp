#include "Oracle.h"

#include "Analysis/AnalysisResult.h"
#include "Analysis/DetectionEngine.h"
#include "Analysis/DetectionProfile.h"
#include "Analysis/FeatureExtractor.h"
#include "Analysis/FeatureRegistry.h"
#include "Features/FeatureEvaluation.h"

#include <cmath>
#include <map>

namespace
{
    juce::AudioBuffer<float> resampleLikeDaat (const juce::AudioBuffer<float>& in,
                                               double sourceRate,
                                               double analysisRate)
    {
        juce::AudioBuffer<float> copy;
        if (in.getNumSamples() <= 0 || in.getNumChannels() <= 0)
            return copy;

        if (sourceRate <= 0.0 || analysisRate <= 0.0
            || std::abs (sourceRate - analysisRate) < 1.0e-9)
        {
            copy.makeCopyOf (in);
            return copy;
        }

        const double ratio = sourceRate / analysisRate; // DAAT: input consumed per output sample
        const int outLen = juce::jmax (1, (int) std::floor ((double) in.getNumSamples() / ratio));
        copy.setSize (in.getNumChannels(), outLen, false, false, true);
        copy.clear();

        for (int c = 0; c < in.getNumChannels(); ++c)
        {
            juce::LagrangeInterpolator interp;
            interp.reset();
            interp.process (ratio, in.getReadPointer (c), copy.getWritePointer (c), outLen);
        }
        return copy;
    }

    const char* verdictName (Verdict v)
    {
        switch (v)
        {
            case Verdict::Likely:            return "Likely";
            case Verdict::Unlikely:          return "Unlikely";
            case Verdict::Inconclusive:      return "Inconclusive";
            case Verdict::InsufficientAudio: return "InsufficientAudio";
        }
        return "Inconclusive";
    }
} // namespace

class Oracle::Impl
{
public:
    Impl() : profile (DetectionProfile::getFactoryDefault()) {}

    AnalysisOutcome analyze (const juce::AudioBuffer<float>& buffer, double sourceRate)
    {
        AnalysisOutcome out;
        const int numSamples = buffer.getNumSamples();
        const int numChannels = buffer.getNumChannels();
        if (numSamples <= 0 || numChannels <= 0 || sourceRate <= 0.0)
            return out;

        const double analysisRate = profile.analysis.sampleRate;
        auto audio = resampleLikeDaat (buffer, sourceRate, analysisRate);
        if (audio.getNumSamples() <= 0)
            return out;

        const double analyzedSeconds = (double) audio.getNumSamples() / analysisRate;
        if (analyzedSeconds < profile.analysis.minimumAnalyzedSeconds)
        {
            out.verdict = "InsufficientAudio";
            return out;
        }

        FeatureExtractor extractor;
        FeatureExtractor::Settings xs;
        xs.hfCutoffHz         = profile.hfCutoffHz();
        xs.repetitionMinLagMs = profile.repetitionMinLagMs();
        xs.repetitionMaxLagMs = profile.repetitionMaxLagMs();

        if (! extractor.prepare (profile.analysis.fftSize,
                                 profile.analysis.hopSize,
                                 analysisRate,
                                 xs))
            return out;

        struct Acc
        {
            double raw = 0.0;
            double suspicion = 0.0;
            double confidence = 0.0;
            float weight = 0.0f;
            int n = 0;
        };
        std::map<juce::String, Acc> acc;

        auto analyzeScale = [&] (AnalysisScale scale, double windowSeconds, double overlap)
        {
            const auto windows = FeatureExtractor::planWindows (
                audio.getNumSamples(), analysisRate, windowSeconds, overlap);
            const auto mask = daat::features::scaleBit (scale);

            for (const auto& [start, length] : windows)
            {
                const auto raw = extractor.analyzeWindow (audio, start, length, numChannels);
                const auto feats = daat::features::evaluateWindow (
                    raw, profile, numChannels, mask);

                ++out.numWindows;
                for (const auto& f : feats)
                {
                    if (! f.valid)
                        continue;

                    auto& a = acc[f.featureId];
                    a.raw        += f.rawValue;
                    a.suspicion  += f.suspicionScore;
                    a.confidence += f.confidence;
                    a.weight      = f.effectiveWeight;
                    ++a.n;
                }
            }
        };

        analyzeScale (AnalysisScale::shortScale,
                      profile.analysis.shortWindowSeconds,
                      profile.analysis.shortOverlap);
        analyzeScale (AnalysisScale::mediumScale,
                      profile.analysis.mediumWindowSeconds,
                      profile.analysis.mediumOverlap);
        analyzeScale (AnalysisScale::longScale,
                      profile.analysis.longWindowSeconds,
                      profile.analysis.longOverlap);

        std::vector<FeatureResult> aggregate;
        aggregate.reserve (acc.size());

        // Match DAAT AnalysisEngine::finalizeResult: registry order, arithmetic
        // mean over applicable windows, and the effective weight from scoring.
        for (const auto& desc : getFeatureRegistry())
        {
            const auto it = acc.find (desc.id);
            if (it == acc.end() || it->second.n == 0)
                continue;

            const auto& a = it->second;
            FeatureResult r;
            r.featureId       = desc.id;
            r.valid           = true;
            r.rawValue        = (float) (a.raw / a.n);
            r.suspicionScore  = (float) (a.suspicion / a.n);
            r.normalizedValue = r.suspicionScore;
            r.confidence      = (float) (a.confidence / a.n);
            r.effectiveWeight = a.weight;
            aggregate.push_back (r);

            FeatureScore fs;
            fs.id         = desc.id.toStdString();
            fs.suspicion  = r.suspicionScore;
            fs.confidence = r.confidence;
            fs.weight     = r.effectiveWeight;
            fs.windows    = a.n;
            out.features.push_back (std::move (fs));
        }

        daat::detect::RuntimeControls controls;
        const auto groups = daat::detect::computeGroupScores (aggregate, profile, controls);
        const auto overall = daat::detect::combineGroups (groups, profile, controls);

        float peak = 0.0f;
        double sumSq = 0.0;
        juce::int64 clippedSamples = 0;
        for (int c = 0; c < numChannels; ++c)
        {
            const float* d = audio.getReadPointer (c);
            for (int i = 0; i < audio.getNumSamples(); ++i)
            {
                const float a = std::abs (d[i]);
                peak = juce::jmax (peak, a);
                sumSq += (double) d[i] * (double) d[i];
                if (a >= 0.999f)
                    ++clippedSamples;
            }
        }

        const double denom = (double) audio.getNumSamples() * (double) numChannels;
        const double rms = denom > 0.0 ? std::sqrt (sumSq / denom) : 0.0;

        daat::detect::ConfidenceContext ctx;
        ctx.analyzedSeconds  = analyzedSeconds;
        ctx.minimumSeconds   = profile.analysis.minimumAnalyzedSeconds;
        ctx.numWindows       = out.numWindows;
        ctx.channels         = numChannels;
        ctx.clipped          = clippedSamples > (juce::int64) (denom * 0.0001);
        ctx.rmsDb            = (float) juce::Decibels::gainToDecibels (rms, -100.0);
        ctx.sourceSampleRate = sourceRate;

        out.scorable = overall.scorable;
        out.likelihood = overall.scorable ? (double) overall.likelihood : 0.0;
        out.confidence = daat::detect::computeConfidence (overall, groups, profile, ctx);
        out.verdict = verdictName (
            daat::detect::decideVerdict (overall, (float) out.confidence, profile, controls));

        for (const auto& g : groups)
        {
            GroupScoreView v;
            v.name       = g.displayName.toStdString();
            v.score      = g.score;
            v.confidence = g.confidence;
            v.valid      = g.valid;
            out.groups.push_back (std::move (v));
        }
        return out;
    }

private:
    DetectionProfile profile;
};

Oracle::Oracle() : impl (std::make_unique<Impl>()) {}
Oracle::~Oracle() = default;

AnalysisOutcome Oracle::analyze (const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    return impl->analyze (buffer, sampleRate);
}
