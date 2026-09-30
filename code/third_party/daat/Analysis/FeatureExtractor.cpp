#include "FeatureExtractor.h"
#include "../Utility/Statistics.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
    bool isPowerOfTwo (int x) noexcept { return x > 0 && (x & (x - 1)) == 0; }

    constexpr double kEpsilon = 1.0e-12;

    // Coefficient of variation of onset strengths picked from the flux envelope.
    // Returns <0 when there are too few onsets to judge (avoids false positives
    // on sustained material).
    double computeTransientCV (const std::vector<double>& flux)
    {
        if (flux.size() < 4)
            return -1.0;

        double mean = 0.0;
        for (auto f : flux) mean += f;
        mean /= (double) flux.size();

        double var = 0.0;
        for (auto f : flux) { const double d = f - mean; var += d * d; }
        var /= (double) (flux.size() - 1);
        const double sd = std::sqrt (var);
        const double thr = mean + 0.5 * sd;

        std::vector<double> onsets;
        for (size_t i = 1; i + 1 < flux.size(); ++i)
            if (flux[i] > thr && flux[i] >= flux[i - 1] && flux[i] >= flux[i + 1])
                onsets.push_back (flux[i]);

        if (onsets.size() < 3)
            return -1.0;

        double om = 0.0;
        for (auto o : onsets) om += o;
        om /= (double) onsets.size();
        if (om < kEpsilon)
            return -1.0;

        double ov = 0.0;
        for (auto o : onsets) { const double d = o - om; ov += d * d; }
        ov /= (double) (onsets.size() - 1);

        return juce::jlimit (0.0, 1.0, std::sqrt (ov) / om);
    }

    // 1 = perfectly stationary noise floor across the window; lower = more varied.
    double computeStationarity (const std::vector<float>& env)
    {
        const int K = 8;
        if ((int) env.size() < K)
            return 0.0;

        const int groupSize = (int) env.size() / K;
        std::vector<double> floorsDb;
        for (int g = 0; g < K; ++g)
        {
            float mn = std::numeric_limits<float>::max();
            for (int i = g * groupSize; i < (g + 1) * groupSize; ++i)
                mn = juce::jmin (mn, env[(size_t) i]);
            floorsDb.push_back (20.0 * std::log10 ((double) mn + 1.0e-9));
        }

        double m = 0.0;
        for (auto d : floorsDb) m += d;
        m /= (double) floorsDb.size();

        double v = 0.0;
        for (auto d : floorsDb) { const double dd = d - m; v += dd * dd; }
        v /= (double) (floorsDb.size() - 1);

        return juce::jlimit (0.0, 1.0, 1.0 - juce::jlimit (0.0, 1.0, std::sqrt (v) / 6.0));
    }

    // Peak normalized autocorrelation of the (mean-removed) envelope over a lag
    // range. Each lag is normalized by the energy of its own overlap region so a
    // periodic signal peaks near 1.0 regardless of lag. Lags are limited to n/2
    // to keep each estimate over a sufficient overlap. High => strong looping.
    double computeRepetition (const std::vector<float>& env, int minLag, int maxLag)
    {
        const int n = (int) env.size();
        if (n < 8 || minLag < 1 || minLag >= n)
            return 0.0;

        maxLag = juce::jmin (maxLag, n / 2);
        if (maxLag <= minLag)
            return 0.0;

        double mean = 0.0;
        for (auto e : env) mean += e;
        mean /= (double) n;

        std::vector<double> ec ((size_t) n);
        for (int i = 0; i < n; ++i)
            ec[(size_t) i] = (double) env[(size_t) i] - mean;

        double best = 0.0;
        for (int lag = minLag; lag <= maxLag; ++lag)
        {
            double s = 0.0, e1 = 0.0, e2 = 0.0;
            for (int t = 0; t + lag < n; ++t)
            {
                const double a = ec[(size_t) t];
                const double b = ec[(size_t) (t + lag)];
                s  += a * b;
                e1 += a * a;
                e2 += b * b;
            }
            const double denom = std::sqrt (e1 * e2);
            if (denom > kEpsilon)
                best = juce::jmax (best, s / denom);
        }

        return juce::jlimit (0.0, 1.0, best);
    }
}

//==============================================================================
bool FeatureExtractor::prepare (int newFftSize, int newHopSize, double newSampleRate,
                                const Settings& newSettings)
{
    if (! isPowerOfTwo (newFftSize))
        return false;

    settings   = newSettings;
    fftSize    = newFftSize;
    hopSize    = juce::jlimit (1, fftSize, newHopSize);
    numBins    = fftSize / 2;
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;

    const int order = (int) std::round (std::log2 ((double) fftSize));
    fft    = std::make_unique<juce::dsp::FFT> (order);
    window = std::make_unique<juce::dsp::WindowingFunction<float>> (
                 (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann, false);

    fftScratch.assign ((size_t) (fftSize * 2), 0.0f);
    magnitude.assign ((size_t) numBins, 0.0f);
    avgMagnitude.assign ((size_t) numBins, 0.0f);
    prevMagnitude.assign ((size_t) numBins, 0.0f);

    return true;
}

//==============================================================================
void FeatureExtractor::computeFrameMagnitudes (const juce::AudioBuffer<float>& buffer,
                                               int frameStart, int numChannels)
{
    const int totalSamples = buffer.getNumSamples();
    std::fill (fftScratch.begin(), fftScratch.end(), 0.0f);

    // Downmix to mono into the first fftSize slots, zero-padding past the end.
    for (int i = 0; i < fftSize; ++i)
    {
        const int idx = frameStart + i;
        if (idx >= totalSamples)
            break;

        float sum = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
            sum += buffer.getSample (ch, idx);

        float s = sum / (float) juce::jmax (1, numChannels);
        if (! std::isfinite (s))
            s = 0.0f;
        fftScratch[(size_t) i] = s;
    }

    window->multiplyWithWindowingTable (fftScratch.data(), (size_t) fftSize);
    fft->performFrequencyOnlyForwardTransform (fftScratch.data(), true);

    for (int k = 0; k < numBins; ++k)
        magnitude[(size_t) k] = fftScratch[(size_t) k];
}

//==============================================================================
WindowRawFeatures FeatureExtractor::analyzeWindow (const juce::AudioBuffer<float>& buffer,
                                                   int startSample,
                                                   int numSamples,
                                                   int numChannels)
{
    WindowRawFeatures out;

    if (fft == nullptr || numSamples <= 0 || buffer.getNumSamples() <= 0)
        return out;

    numChannels = juce::jlimit (1, buffer.getNumChannels(), numChannels);
    const int endSample = juce::jmin (startSample + numSamples, buffer.getNumSamples());
    const int usable    = endSample - startSample;
    if (usable <= 0)
        return out;

    //--- Time-domain: peak, RMS, crest, and stereo metrics ---
    double sumSq = 0.0, peak = 0.0;
    double sumL = 0.0, sumR = 0.0, sumLR = 0.0, sumL2 = 0.0, sumR2 = 0.0;
    double midEnergy = 0.0, sideEnergy = 0.0;
    const bool stereo = numChannels >= 2;

    const float* L = buffer.getReadPointer (0);
    const float* R = stereo ? buffer.getReadPointer (1) : nullptr;

    for (int i = startSample; i < endSample; ++i)
    {
        float l = L[i];
        if (! std::isfinite (l)) l = 0.0f;

        if (stereo)
        {
            float rr = R[i];
            if (! std::isfinite (rr)) rr = 0.0f;

            const double mono = 0.5 * ((double) l + (double) rr);
            const double side = 0.5 * ((double) l - (double) rr);
            sumSq += mono * mono;
            peak = juce::jmax (peak, (double) juce::jmax (std::abs (l), std::abs (rr)));

            sumL += l;   sumR += rr;
            sumLR += (double) l * (double) rr;
            sumL2 += (double) l * (double) l;
            sumR2 += (double) rr * (double) rr;
            midEnergy  += mono * mono;
            sideEnergy += side * side;
        }
        else
        {
            sumSq += (double) l * (double) l;
            peak = juce::jmax (peak, (double) std::abs (l));
        }
    }

    const double rms = std::sqrt (sumSq / (double) usable);
    out.rms  = daat::stats::sanitize (rms);
    out.peak = daat::stats::sanitize (peak);
    out.crestFactorDb = daat::stats::sanitize (
        juce::Decibels::gainToDecibels (out.peak, -120.0) - juce::Decibels::gainToDecibels (rms, -120.0), 0.0);

    if (stereo)
    {
        const double denom = std::sqrt ((sumL2 + kEpsilon) * (sumR2 + kEpsilon));
        out.leftRightCorrelation = daat::stats::sanitize (denom > 0.0 ? sumLR / denom : 1.0, 1.0);
        out.midSideRatio = daat::stats::sanitize (
            std::sqrt (sideEnergy + kEpsilon) / std::sqrt (midEnergy + kEpsilon), 0.0);
        juce::ignoreUnused (sumL, sumR);
    }
    else
    {
        out.leftRightCorrelation = 1.0; // neutral; stereo features skip mono
        out.midSideRatio = 0.0;
    }

    //--- Frequency-domain: average magnitude spectrum + spectral flux ---
    std::fill (avgMagnitude.begin(), avgMagnitude.end(), 0.0f);
    std::fill (prevMagnitude.begin(), prevMagnitude.end(), 0.0f);
    frameFluxes.clear();

    int numFrames = 0;
    double fluxAccum = 0.0;

    for (int frameStart = startSample;
         frameStart + fftSize <= endSample || (numFrames == 0 && frameStart < endSample);
         frameStart += hopSize)
    {
        computeFrameMagnitudes (buffer, frameStart, numChannels);

        double frameFlux = 0.0;
        for (int k = 0; k < numBins; ++k)
        {
            const float m = magnitude[(size_t) k];
            avgMagnitude[(size_t) k] += m;
            const double diff = (double) m - (double) prevMagnitude[(size_t) k];
            if (diff > 0.0)
                frameFlux += diff;
            prevMagnitude[(size_t) k] = m;
        }

        const double normFlux = frameFlux / (double) juce::jmax (1, numBins);
        if (numFrames > 0) // skip frame 0 (measured against a zero spectrum)
        {
            fluxAccum += normFlux;
            frameFluxes.push_back (normFlux);
        }
        ++numFrames;

        if (numFrames == 1 && frameStart + fftSize > endSample)
            break; // only a single zero-padded frame fits
    }

    if (numFrames == 0)
        return out;

    for (int k = 0; k < numBins; ++k)
        avgMagnitude[(size_t) k] /= (float) numFrames;

    // Aggregate spectral descriptors from the averaged magnitude spectrum.
    double totalMag = 0.0, weightedFreq = 0.0, logSum = 0.0;
    double hfMag = 0.0;
    const double binToHz = sampleRate / (double) fftSize;
    const double hfCutoffHz = settings.hfCutoffHz;

    for (int k = 0; k < numBins; ++k)
    {
        const double m = (double) avgMagnitude[(size_t) k];
        const double f = (double) k * binToHz;
        totalMag += m;
        weightedFreq += f * m;
        logSum += std::log (m + kEpsilon);
        if (f >= hfCutoffHz)
            hfMag += m;
    }

    if (totalMag > kEpsilon)
    {
        out.spectralCentroidHz = daat::stats::sanitize (weightedFreq / totalMag);
        out.highFrequencyRatio = daat::stats::sanitize (hfMag / totalMag);

        const double geoMean = std::exp (logSum / (double) numBins);
        const double ariMean = totalMag / (double) numBins;
        out.spectralFlatness = daat::stats::sanitize (ariMean > kEpsilon ? geoMean / ariMean : 0.0);

        // 85% spectral rolloff.
        const double rolloffTarget = 0.85 * totalMag;
        double cumulative = 0.0;
        int rolloffBin = numBins - 1;
        for (int k = 0; k < numBins; ++k)
        {
            cumulative += (double) avgMagnitude[(size_t) k];
            if (cumulative >= rolloffTarget)
            {
                rolloffBin = k;
                break;
            }
        }
        out.spectralRolloffHz = daat::stats::sanitize ((double) rolloffBin * binToHz);
    }

    // Normalize flux by the average magnitude so it is roughly scale-invariant.
    const int fluxFrames = juce::jmax (1, numFrames - 1);
    const double avgTotalMag = totalMag / (double) juce::jmax (1, numBins);
    out.spectralFlux = daat::stats::sanitize (
        avgTotalMag > kEpsilon ? (fluxAccum / (double) fluxFrames) / avgTotalMag : 0.0);

    //--- Time-domain features that need the sample envelope ---
    out.transientVariance = computeTransientCV (frameFluxes);

    const int envHop = juce::jmax (1, (int) std::round (0.010 * sampleRate)); // ~10 ms
    envelope.clear();
    for (int pos = startSample; pos < endSample; pos += envHop)
    {
        const int stop = juce::jmin (pos + envHop, endSample);
        double ss = 0.0;
        int cnt = 0;
        for (int i = pos; i < stop; ++i)
        {
            double mono = 0.0;
            for (int c = 0; c < numChannels; ++c)
                mono += buffer.getSample (c, i);
            mono /= (double) juce::jmax (1, numChannels);
            if (! std::isfinite (mono)) mono = 0.0;
            ss += mono * mono;
            ++cnt;
        }
        envelope.push_back ((float) std::sqrt (ss / (double) juce::jmax (1, cnt)));
    }

    out.noiseFloorStationarity = computeStationarity (envelope);

    const int minLagEnv = juce::jmax (1, (int) std::round (settings.repetitionMinLagMs * 0.001 * sampleRate / envHop));
    const int maxLagEnv = (int) std::round (settings.repetitionMaxLagMs * 0.001 * sampleRate / envHop);
    out.microRepetition = computeRepetition (envelope, minLagEnv, maxLagEnv);

    return out;
}

//==============================================================================
std::vector<std::pair<int, int>> FeatureExtractor::planWindows (int totalSamples,
                                                                double sampleRate,
                                                                double windowSeconds,
                                                                double overlap)
{
    std::vector<std::pair<int, int>> windows;

    if (totalSamples <= 0 || sampleRate <= 0.0 || windowSeconds <= 0.0)
        return windows;

    const int windowSamples = juce::jmax (1, (int) std::round (windowSeconds * sampleRate));
    overlap = juce::jlimit (0.0, 0.95, overlap);
    const int hop = juce::jmax (1, (int) std::round (windowSamples * (1.0 - overlap)));

    if (totalSamples <= windowSamples)
    {
        windows.emplace_back (0, totalSamples);
        return windows;
    }

    for (int start = 0; start < totalSamples; start += hop)
    {
        const int remaining = totalSamples - start;
        if (remaining < windowSamples)
        {
            // Include a trailing partial window only if it is at least half-length.
            if (remaining >= windowSamples / 2 && start > 0)
                windows.emplace_back (start, remaining);
            break;
        }
        windows.emplace_back (start, windowSamples);
    }

    return windows;
}
