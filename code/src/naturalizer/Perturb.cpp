#include "Perturb.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    double bufferRms (const juce::AudioBuffer<float>& b)
    {
        double sum = 0.0;
        long n = 0;
        for (int c = 0; c < b.getNumChannels(); ++c)
        {
            const float* d = b.getReadPointer (c);
            for (int i = 0; i < b.getNumSamples(); ++i) sum += (double) d[i] * d[i];
            n += b.getNumSamples();
        }
        return n > 0 ? std::sqrt (sum / n) : 0.0;
    }

    double bufferPeak (const juce::AudioBuffer<float>& b)
    {
        float peak = 0.0f;
        for (int c = 0; c < b.getNumChannels(); ++c)
            peak = std::max (peak, b.getMagnitude (c, 0, b.getNumSamples()));
        return peak;
    }

    void renormalizePeak (juce::AudioBuffer<float>& b, double targetPeak)
    {
        const double peak = bufferPeak (b);
        if (peak < 1e-9 || targetPeak < 1e-9) return;
        const float g = (float) (targetPeak / peak);
        if (std::abs (g - 1.0f) < 1e-6f) return;
        for (int c = 0; c < b.getNumChannels(); ++c)
            b.applyGain (c, 0, b.getNumSamples(), g);
    }

    //--- light-weight measurement mirrors (approximate; the Oracle is the arbiter)
    struct SpectrumStats { double centroidHz = 0.0; double hfRatio = 0.0; };

    SpectrumStats measureSpectrum (const juce::AudioBuffer<float>& b, double sampleRate)
    {
        SpectrumStats st;
        const int order = 12, size = 1 << order;
        juce::dsp::FFT fft (order);
        juce::dsp::WindowingFunction<float> win ((size_t) size, juce::dsp::WindowingFunction<float>::hann);

        std::vector<float> scratch ((size_t) (2 * size));
        std::vector<double> accMag ((size_t) (size / 2), 0.0);
        int frames = 0;

        const int n = b.getNumSamples();
        const int step = std::max (size, n / 8);
        std::vector<float> mono ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            double s = 0.0;
            for (int c = 0; c < b.getNumChannels(); ++c) s += b.getReadPointer (c)[i];
            mono[(size_t) i] = (float) (s / std::max (1, b.getNumChannels()));
        }

        for (int start = 0; start + size <= n; start += step)
        {
            std::copy (mono.begin() + start, mono.begin() + start + size, scratch.begin());
            win.multiplyWithWindowingTable (scratch.data(), (size_t) size);
            std::fill (scratch.begin() + size, scratch.end(), 0.0f);
            fft.performFrequencyOnlyForwardTransform (scratch.data());

            for (int k = 0; k < size / 2; ++k) accMag[(size_t) k] += scratch[(size_t) k];
            ++frames;
        }
        if (frames == 0) return st;

        double num = 0.0, den = 0.0, hf = 0.0, tot = 0.0;
        for (int k = 1; k < size / 2; ++k)
        {
            const double mag = accMag[(size_t) k] / frames;
            const double f = k * sampleRate / size;
            const double e = mag * mag;
            num += f * mag; den += mag; tot += e;
            if (f > 12000.0) hf += e;
        }
        st.centroidHz = den > 1e-12 ? num / den : 0.0;
        st.hfRatio = tot > 1e-18 ? hf / tot : 0.0;
        return st;
    }

    double measureCorrelation (const juce::AudioBuffer<float>& b)
    {
        if (b.getNumChannels() < 2) return 0.0;
        const float* l = b.getReadPointer (0);
        const float* r = b.getReadPointer (1);
        const int n = b.getNumSamples();
        const int stride = std::max (1, n / 200000);
        double slr = 0.0, sll = 0.0, srr = 0.0;
        for (int i = 0; i < n; i += stride) { slr += l[i] * r[i]; sll += l[i] * l[i]; srr += r[i] * r[i]; }
        const double den = std::sqrt (sll * srr);
        return den > 1e-18 ? juce::jlimit (-1.0, 1.0, slr / den) : 0.0;
    }

    double measureCrestDb (const juce::AudioBuffer<float>& b)
    {
        const double rms = bufferRms (b), peak = bufferPeak (b);
        if (rms < 1e-9 || peak < 1e-9) return 0.0;
        return 20.0 * std::log10 (peak / rms);
    }

    // One-pole lowpass helper (in-place on a scratch vector).
    void onePoleLowpass (std::vector<float>& x, double cutoffHz, double sampleRate)
    {
        const double a = std::exp (-2.0 * kPi * cutoffHz / sampleRate);
        float y = 0.0f;
        for (auto& s : x) { y = (float) (s * (1.0 - a) + y * a); s = y; }
    }

    std::vector<float> whiteNoise (int n, std::mt19937_64& rng)
    {
        std::vector<float> x ((size_t) n);
        std::uniform_real_distribution<float> u (-1.0f, 1.0f);
        for (auto& s : x) s = u (rng);
        return x;
    }
} // namespace

std::vector<PerturbOp> makeOperators (double sampleRate)
{
    std::vector<PerturbOp> ops;

    //--- 1. Stereo widen / decorrelate -------------------------------------
    // Targets: stereoCorrelation (high L/R corr), midSideRatio (extreme).
    ops.push_back ({
        "stereoWiden", "Stereo Decorrelate", "stereoCorrelation, midSideRatio", 1.2,
        [sampleRate] (juce::AudioBuffer<float>& b, double s, std::mt19937_64& rng)
        {
            if (b.getNumChannels() < 2 || s <= 0.0) return;
            const int n = b.getNumSamples();
            const double peakBefore = bufferPeak (b);

            std::vector<float> mid ((size_t) n), side ((size_t) n);
            const float* l = b.getReadPointer (0);
            const float* r = b.getReadPointer (1);
            for (int i = 0; i < n; ++i)
            {
                mid[(size_t) i]  = 0.5f * (l[i] + r[i]);
                side[(size_t) i] = 0.5f * (l[i] - r[i]);
            }

            double sideRms = 0.0, midRms = 0.0;
            for (int i = 0; i < n; ++i)
            {
                sideRms += (double) side[(size_t) i] * side[(size_t) i];
                midRms  += (double) mid[(size_t) i]  * mid[(size_t) i];
            }
            sideRms = std::sqrt (sideRms / n); midRms = std::sqrt (midRms / n);

            auto noise = whiteNoise (n, rng);
            onePoleLowpass (noise, 8000.0, sampleRate); // decorrelated, band-limited

            if (sideRms < 1e-7)
            {
                // Dual-mono: synthesize a side channel from a micro-delayed
                // difference plus a whisper of decorrelated noise (Haas-style).
                const int d = 1 + (int) (14.0 * s);
                const float nz = (float) (midRms * 0.030 * s);
                for (int i = 0; i < n; ++i)
                {
                    const float delayed = (i >= d) ? mid[(size_t) (i - d)] : 0.0f;
                    side[(size_t) i] = 0.5f * (mid[(size_t) i] - delayed) * (float) s
                                     + noise[(size_t) i] * nz;
                }
            }
            else
            {
                const float widen = (float) (1.0 + 0.5 * s);
                const float nz = (float) (midRms * 0.030 * s);
                for (int i = 0; i < n; ++i)
                    side[(size_t) i] = side[(size_t) i] * widen + noise[(size_t) i] * nz;
            }

            float* lw = b.getWritePointer (0);
            float* rw = b.getWritePointer (1);
            for (int i = 0; i < n; ++i)
            {
                lw[i] = mid[(size_t) i] + side[(size_t) i];
                rw[i] = mid[(size_t) i] - side[(size_t) i];
            }
            renormalizePeak (b, peakBefore);
        }
    });

    //--- 2. Noise-floor breathing ------------------------------------------
    // Targets: noiseFloorStationarity. Adds time-varying shaped noise so the
    // floor wobbles instead of sitting at one digital value.
    ops.push_back ({
        "noiseBreathe", "Noise-Floor Breathing", "noiseFloorStationarity", 0.8,
        [sampleRate] (juce::AudioBuffer<float>& b, double s, std::mt19937_64& rng)
        {
            if (s <= 0.0) return;
            const int n = b.getNumSamples();
            auto noise = whiteNoise (n, rng);
            onePoleLowpass (noise, 6000.0, sampleRate);

            // Slow random-walk gain envelope, updated every 250 ms.
            const int seg = (int) (0.25 * sampleRate);
            std::uniform_real_distribution<double> step (-0.35, 0.35);
            double g = 1.0;
            std::vector<float> env ((size_t) n);
            for (int start = 0; start < n; start += seg)
            {
                g = juce::jlimit (0.35, 1.65, g + step (rng));
                const int end = std::min (start + seg, n);
                for (int i = start; i < end; ++i) env[(size_t) i] = (float) g;
            }
            // Smooth the envelope (50 ms ramp between segments).
            const int sm = std::max (1, (int) (0.05 * sampleRate));
            for (int i = sm; i < n; ++i)
                env[(size_t) i] = env[(size_t) (i - sm)] * 0.7f + env[(size_t) i] * 0.3f;

            const float level = (float) (2.5e-4 * (0.4 + 0.6 * s)); // ~-72..-64 dBFS
            for (int c = 0; c < b.getNumChannels(); ++c)
            {
                float* d = b.getWritePointer (c);
                for (int i = 0; i < n; ++i) d[i] += noise[(size_t) i] * env[(size_t) i] * level;
            }
        }
    });

    //--- 3. Transient humanize ----------------------------------------------
    // Targets: transientVariance (via temporal group). Randomizes transient
    // amplitudes so they are not suspiciously uniform.
    ops.push_back ({
        "transientHumanize", "Transient Variation", "transientVariance", 1.0,
        [] (juce::AudioBuffer<float>& b, double s, std::mt19937_64& rng)
        {
            if (s <= 0.0) return;
            const int n = b.getNumSamples();
            const int frame = 512, hop = 256;
            std::vector<double> flux;
            std::vector<double> energy;
            for (int start = 0; start + frame <= n; start += hop)
            {
                double e = 0.0;
                for (int c = 0; c < b.getNumChannels(); ++c)
                {
                    const float* d = b.getReadPointer (c);
                    for (int i = 0; i < frame; ++i) e += (double) d[start + i] * d[start + i];
                }
                energy.push_back (e);
                if (energy.size() > 1)
                    flux.push_back (std::max (0.0, e - energy[energy.size() - 2]));
            }
            if (flux.size() < 8) return;
            const double mean = std::accumulate (flux.begin(), flux.end(), 0.0) / flux.size();
            double var = 0.0;
            for (auto f : flux) var += (f - mean) * (f - mean);
            const double sd = std::sqrt (var / flux.size());
            const double thr = mean + 2.5 * sd;

            std::uniform_real_distribution<double> ugain (-1.0, 1.0);
            const int minGap = (int) (0.10 * 48000.0 / hop); // 100 ms between onsets
            int lastOnset = -minGap * 2, onsets = 0;

            for (size_t k = 0; k < flux.size() && onsets < 400; ++k)
            {
                if (flux[k] < thr || (int) k - lastOnset < minGap) continue;
                lastOnset = (int) k; ++onsets;

                const int center = (int) k * hop + frame / 2;
                const int half = (int) (0.040 * 48000.0); // ±40 ms region
                const double g = 1.0 + s * 0.30 * ugain (rng);
                const int ramp = (int) (0.010 * 48000.0);
                for (int i = -half; i <= half; ++i)
                {
                    const int idx = center + i;
                    if (idx < 0 || idx >= n) continue;
                    double w = 1.0;
                    const int ad = half - std::abs (i);
                    if (ad < ramp) w = 0.5 - 0.5 * std::cos (kPi * ad / ramp); // raised cosine
                    const float gg = (float) (1.0 + (g - 1.0) * w);
                    for (int c = 0; c < b.getNumChannels(); ++c)
                        b.getWritePointer (c)[idx] *= gg;
                }
            }
        }
    });

    //--- 4. Repetition breaker ----------------------------------------------
    // Targets: microRepetition. Time-varying micro-delay + slow amplitude
    // wobble destroy sample-exact loops; wow-like, nearly inaudible.
    ops.push_back ({
        "repetitionBreak", "Loop De-quantize", "microRepetition", 1.0,
        [sampleRate] (juce::AudioBuffer<float>& b, double s, std::mt19937_64& rng)
        {
            if (s <= 0.0) return;
            const int n = b.getNumSamples();
            std::uniform_real_distribution<double> uphase (0.0, 2.0 * kPi);
            const double p1 = uphase (rng), p2 = uphase (rng);

            // Slow random walk for the delay wander, updated every 500 ms.
            const int seg = (int) (0.5 * sampleRate);
            std::uniform_real_distribution<double> ustep (-0.5, 0.5);
            double walk = 0.0;
            std::vector<double> wander ((size_t) (n / seg + 1));
            for (auto& w : wander) { walk = juce::jlimit (-1.0, 1.0, walk + ustep (rng)); w = walk; }

            for (int c = 0; c < b.getNumChannels(); ++c)
            {
                std::vector<float> src ((size_t) n);
                std::copy (b.getReadPointer (c), b.getReadPointer (c) + n, src.begin());
                float* d = b.getWritePointer (c);
                for (int i = 0; i < n; ++i)
                {
                    const double t = i / sampleRate;
                    const double wseg = wander[(size_t) std::min ((size_t) (i / seg), wander.size() - 1)];
                    const double delay = s * (6.0 * std::sin (2.0 * kPi * 0.11 * t + p1) + 4.0 * wseg);
                    const double pos = i - delay;
                    const int i0 = (int) std::floor (pos);
                    const double frac = pos - i0;
                    const float s0 = (i0 >= 0) ? src[(size_t) i0] : 0.0f;
                    const float s1 = (i0 + 1 < n && i0 + 1 >= 0) ? src[(size_t) (i0 + 1)] : 0.0f;
                    const double wobble = 1.0 + 0.008 * s * std::sin (2.0 * kPi * 0.07 * t + p2);
                    d[i] = (float) ((s0 * (1.0 - frac) + s1 * frac) * wobble);
                }
            }
        }
    });

    //--- 5. Dynamic expansion -------------------------------------------------
    // Targets: crestFactor. Undoes brickwall-style limiting with a soft
    // upward expansion, then restores peak level.
    ops.push_back ({
        "dynamicExpand", "De-limit (Expand)", "crestFactor", 0.9,
        [] (juce::AudioBuffer<float>& b, double s, std::mt19937_64&)
        {
            if (s <= 0.0) return;
            if (measureCrestDb (b) >= 10.0) return; // nothing to fix
            const double peakBefore = bufferPeak (b);
            if (peakBefore < 1e-9) return;

            const double expo = 1.0 + 0.35 * s;
            for (int c = 0; c < b.getNumChannels(); ++c)
            {
                float* d = b.getWritePointer (c);
                const int n = b.getNumSamples();
                for (int i = 0; i < n; ++i)
                {
                    const double x = d[i] / peakBefore; // normalize to 1.0
                    d[i] = (float) (std::copysign (std::pow (std::abs (x), expo), x) * peakBefore);
                }
            }
            renormalizePeak (b, peakBefore);
        }
    });

    //--- 6. Spectral tilt ------------------------------------------------------
    // Targets: spectralCentroid (outsideRange), spectralFlatness (indirectly).
    // Shelves the spectrum back toward the middle of DAAT's "typical" band.
    ops.push_back ({
        "spectralTilt", "Spectral Rebalance", "spectralCentroid, spectralFlatness", 1.1,
        [sampleRate] (juce::AudioBuffer<float>& b, double s, std::mt19937_64&)
        {
            if (s <= 0.0) return;
            const auto stats = measureSpectrum (b, sampleRate);
            const double peakBefore = bufferPeak (b);

            auto shelf = juce::dsp::IIR::Coefficients<float>::makeHighShelf (
                sampleRate, 9000.0, 0.7f,
                (float) (stats.centroidHz > 6000.0 ? -(2.0 + 5.0 * s)
                         : stats.centroidHz < 800.0 ? (2.0 + 5.0 * s) : 0.0));
            if (std::abs (stats.centroidHz > 6000.0 ? -(2.0 + 5.0 * s)
                          : stats.centroidHz < 800.0 ? (2.0 + 5.0 * s) : 0.0) < 0.1)
                return; // already in band

            for (int c = 0; c < b.getNumChannels(); ++c)
            {
                juce::dsp::IIR::Filter<float> f (shelf);
                float* d = b.getWritePointer (c);
                for (int i = 0; i < b.getNumSamples(); ++i)
                    d[i] = f.processSample (d[i]);
            }
            renormalizePeak (b, peakBefore);
        }
    });

    //--- 7. Flux restore --------------------------------------------------------
    // Targets: spectralFlux (low/uniform). Slow random amplitude modulation
    // reintroduces frame-to-frame variation.
    ops.push_back ({
        "fluxRestore", "Flux Variation", "spectralFlux", 0.5,
        [sampleRate] (juce::AudioBuffer<float>& b, double s, std::mt19937_64& rng)
        {
            if (s <= 0.0) return;
            const int n = b.getNumSamples();
            const int seg = (int) (0.040 * sampleRate); // 40 ms steps
            std::uniform_real_distribution<double> u (-1.0, 1.0);
            double cur = 1.0;
            std::vector<float> env ((size_t) n);
            for (int start = 0; start < n; start += seg)
            {
                const double target = 1.0 + s * 0.05 * u (rng);
                const int end = std::min (start + seg, n);
                for (int i = start; i < end; ++i)
                {
                    cur += (target - cur) * 0.25; // smooth toward target
                    env[(size_t) i] = (float) cur;
                }
            }
            for (int c = 0; c < b.getNumChannels(); ++c)
            {
                float* d = b.getWritePointer (c);
                for (int i = 0; i < n; ++i) d[i] *= env[(size_t) i];
            }
        }
    });

    //--- 8. HF regulate ----------------------------------------------------------
    // Targets: highFrequencyRatio (outsideRange). Tames excess ultrasonic
    // energy or lifts a suspiciously band-limited top end.
    ops.push_back ({
        "hfRegulate", "HF Balance", "highFrequencyRatio", 0.7,
        [sampleRate] (juce::AudioBuffer<float>& b, double s, std::mt19937_64& rng)
        {
            if (s <= 0.0) return;
            const auto stats = measureSpectrum (b, sampleRate);
            const double peakBefore = bufferPeak (b);

            if (stats.hfRatio > 0.45)
            {
                auto coefs = juce::dsp::IIR::Coefficients<float>::makeLowPass (sampleRate, 15000.0, 0.5f);
                const float k = (float) (0.3 + 0.7 * s);
                for (int c = 0; c < b.getNumChannels(); ++c)
                {
                    std::vector<float> filtered ((size_t) b.getNumSamples());
                    std::copy (b.getReadPointer (c), b.getReadPointer (c) + b.getNumSamples(), filtered.begin());
                    juce::dsp::IIR::Filter<float> f (coefs);
                    for (int i = 0; i < b.getNumSamples(); ++i)
                        filtered[(size_t) i] = f.processSample (filtered[(size_t) i]);
                    float* d = b.getWritePointer (c);
                    for (int i = 0; i < b.getNumSamples(); ++i)
                        d[i] = d[i] * (1.0f - k) + filtered[(size_t) i] * k;
                }
            }
            else if (stats.hfRatio < 0.002)
            {
                // Suspiciously band-limited: whisper of highpassed noise.
                const int n = b.getNumSamples();
                auto noise = whiteNoise (n, rng);
                // crude highpass: subtract lowpassed version
                auto low = noise;
                onePoleLowpass (low, 12000.0, sampleRate);
                const float level = (float) (1.0e-4 * (0.3 + 0.7 * s)); // ~-80..-70 dBFS
                for (int c = 0; c < b.getNumChannels(); ++c)
                {
                    float* d = b.getWritePointer (c);
                    for (int i = 0; i < n; ++i) d[i] += (noise[(size_t) i] - low[(size_t) i]) * level;
                }
            }
            renormalizePeak (b, peakBefore);
        }
    });

    return ops;
}
