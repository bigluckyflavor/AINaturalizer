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
    constexpr double kAnalysisRate = 48000.0;
    constexpr double kWindowSeconds = 8.0;   // medium scale, like DAAT's default
    constexpr double kOverlap = 0.5;

    /** Linear-interpolation resample to the analysis rate. Bit-transparent
        when the source already matches. */
    juce::AudioBuffer<float> resampleToAnalysisRate (const juce::AudioBuffer<float>& in,
                                                     double sourceRate)
    {
        if (std::abs (sourceRate - kAnalysisRate) < 1.0 || in.getNumSamples() == 0)
        {
            juce::AudioBuffer<float> copy;
            copy.makeCopyOf (in);
            return copy;
        }

        const double ratio = kAnalysisRate / sourceRate;
        const int outLen = (int) std::ceil (in.getNumSamples() * ratio);
        const int ch = in.getNumChannels();

        juce::AudioBuffer<float> out (ch, outLen);
        for (int c = 0; c < ch; ++c)
        {
            const float* src = in.getReadPointer (c);
            float* dst = out.getWritePointer (c);
            const int inLen = in.getNumSamples();
            for (int n = 0; n < outLen; ++n)
            {
                const double pos = n / ratio;
                const int i0 = (int) pos;
                const int i1 = std::min (i0 + 1, inLen - 1);
                const double frac = pos - i0;
                dst[n] = (float) (src[i0] * (1.0 - frac) + src[i1] * frac);
            }
        }
        return out;
    }
} // namespace

class Oracle::Impl
{
public:
    Impl()
        : profile (DetectionProfile::getFactoryDefault())
    {
    }

    AnalysisOutcome analyze (const juce::AudioBuffer<float>& buffer, double sampleRate)
    {
        AnalysisOutcome out;
        const int numSamples = buffer.getNumSamples();
        const int numChannels = buffer.getNumChannels();
        if (numSamples <= 0 || numChannels <= 0)
            return out;

        juce::AudioBuffer<float> audio = resampleToAnalysisRate (buffer, sampleRate);

        FeatureExtractor extractor;
        FeatureExtractor::Settings xs;
        if (! extractor.prepare (profile.analysis.fftSize,
                                 profile.analysis.hopSize,
                                 kAnalysisRate, xs))
            return out;

        const auto windows = FeatureExtractor::planWindows (
            audio.getNumSamples(), kAnalysisRate, kWindowSeconds, kOverlap);

        struct Acc { double suspW = 0.0, conf = 0.0, weight = 0.0; int n = 0; };
        std::map<juce::String, Acc> acc;

        for (const auto& [start, length] : windows)
        {
            const WindowRawFeatures raw =
                extractor.analyzeWindow (audio, start, length, numChannels);

            const auto feats = daat::features::evaluateWindow (
                raw, profile, numChannels, ScaleMask::mediumWindows);

            for (const auto& f : feats)
            {
                if (! f.valid)
                    continue;
                Acc& a = acc[f.featureId];
                a.suspW  += (double) f.suspicionScore * (double) f.confidence;
                a.conf   += (double) f.confidence;
                a.weight += (double) f.effectiveWeight;
                a.n++;
            }
        }

        out.numWindows = (int) windows.size();

        std::vector<FeatureResult> aggregate;
        aggregate.reserve (acc.size());
        for (const auto& [id, a] : acc)
        {
            FeatureResult r;
            r.featureId       = id;
            r.valid           = a.n > 0;
            r.suspicionScore  = a.conf > 1e-9 ? (float) (a.suspW / a.conf) : 0.0f;
            r.normalizedValue = r.suspicionScore;
            r.confidence      = a.n > 0 ? (float) (a.conf / a.n) : 0.0f;
            r.effectiveWeight = a.n > 0 ? (float) (a.weight / a.n) : 0.0f;
            aggregate.push_back (r);

            FeatureScore fs;
            fs.id         = id.toStdString();
            fs.suspicion  = r.suspicionScore;
            fs.confidence = r.confidence;
            fs.weight     = r.effectiveWeight;
            fs.windows    = a.n;
            out.features.push_back (std::move (fs));
        }

        daat::detect::RuntimeControls controls; // factory-neutral: sensitivity 0.5
        const auto groups = daat::detect::computeGroupScores (aggregate, profile, controls);
        const auto overall = daat::detect::combineGroups (groups, profile, controls);

        daat::detect::ConfidenceContext ctx;
        ctx.analyzedSeconds  = audio.getNumSamples() / kAnalysisRate;
        ctx.numWindows       = out.numWindows;
        ctx.channels         = numChannels;
        ctx.sourceSampleRate = kAnalysisRate;
        out.confidence = daat::detect::computeConfidence (overall, groups, profile, ctx);

        out.likelihood = overall.scorable ? (double) overall.likelihood : 0.0;

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
