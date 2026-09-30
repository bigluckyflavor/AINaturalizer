// naturalizer — offline AI-music naturalizer (prototype).
//
//   naturalizer analyze in.wav
//       Print DAAT's AI-likelihood and the per-feature suspicion table.
//
//   naturalizer in.wav out.wav [--target 0.35] [--max-iters 12]
//             [--budget 6.0] [--seed 1234]
//       Iteratively perturb the audio until DAAT's likelihood falls below
//       --target, then write the result. Refuses to exceed the perceptual
//       budget or to ship changes that don't actually help.

#include "naturalizer/Oracle.h"
#include "naturalizer/Optimize.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
    struct Args
    {
        bool analyzeOnly = false;
        std::string inPath, outPath;
        OptimizeConfig config;
    };

    bool parseArgs (int argc, char** argv, Args& a)
    {
        if (argc < 3) return false;
        if (std::strcmp (argv[1], "analyze") == 0)
        {
            if (argc != 3) return false;
            a.analyzeOnly = true;
            a.inPath = argv[2];
            return true;
        }
        a.inPath = argv[1];
        a.outPath = argv[2];
        for (int i = 3; i < argc; ++i)
        {
            std::string k = argv[i];
            auto needVal = [&] (double& dst, const char* name) -> bool
            {
                if (++i >= argc) { std::printf ("missing value for %s\n", name); return false; }
                dst = std::stod (argv[i]);
                return true;
            };
            if (k == "--target")         { double v; if (! needVal (v, "--target")) return false; a.config.targetLikelihood = v; }
            else if (k == "--max-iters") { double v; if (! needVal (v, "--max-iters")) return false; a.config.maxIters = (int) v; }
            else if (k == "--budget")    { double v; if (! needVal (v, "--budget")) return false; a.config.budget = v; }
            else if (k == "--seed")      { double v; if (! needVal (v, "--seed")) return false; a.config.seed = (uint64_t) v; }
            else { std::printf ("unknown option: %s\n", k.c_str()); return false; }
        }
        return true;
    }

    void usage()
    {
        std::printf ("usage:\n"
                     "  naturalizer analyze in.wav\n"
                     "  naturalizer in.wav out.wav [--target 0.35] [--max-iters 12]\n"
                     "              [--budget 6.0] [--seed 1234]\n");
    }

    bool loadWav (const std::string& path, juce::AudioBuffer<float>& buffer, double& sampleRate)
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (
            fm.createReaderFor (juce::File (juce::String (path))));
        if (! reader) { std::printf ("cannot read %s\n", path.c_str()); return false; }
        sampleRate = reader->sampleRate;
        buffer.setSize ((int) reader->numChannels, (int) reader->lengthInSamples);
        reader->read (&buffer, 0, (int) reader->lengthInSamples, 0, true, true);
        return true;
    }

    bool writeWav (const std::string& path, const juce::AudioBuffer<float>& buffer, double sampleRate)
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::FileOutputStream> out (
            juce::File (juce::String (path)).createOutputStream());
        if (! out) { std::printf ("cannot write %s\n", path.c_str()); return false; }
        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (
            out.get(), sampleRate, (unsigned int) buffer.getNumChannels(), 24, {}, 0));
        if (! writer) { std::printf ("cannot create writer for %s\n", path.c_str()); return false; }
        out.release(); // writer owns it now
        writer->writeFromAudioSampleBuffer (
            const_cast<juce::AudioBuffer<float>&> (buffer), 0, buffer.getNumSamples());
        return true;
    }

    void printAnalysis (const AnalysisOutcome& o)
    {
        std::printf ("\nlikelihood %.4f   confidence %.3f   (%d windows)\n",
                     o.likelihood, o.confidence, o.numWindows);
        std::printf ("\n%-22s %10s %10s %8s\n", "feature", "suspicion", "confidence", "weight");
        for (const auto& f : o.features)
            std::printf ("%-22s %10.4f %10.3f %8.3f\n",
                         f.id.c_str(), f.suspicion, f.confidence, f.weight);
        std::printf ("\n%-12s %8s %10s\n", "group", "score", "confidence");
        for (const auto& g : o.groups)
            std::printf ("%-12s %8.4f %10.3f %s\n",
                         g.name.c_str(), g.score, g.confidence, g.valid ? "" : "(n/a)");
        std::printf ("\n");
    }
} // namespace

int main (int argc, char** argv)
{
    Args args;
    if (! parseArgs (argc, argv, args)) { usage(); return 1; }

    juce::AudioBuffer<float> buffer;
    double sampleRate = 0.0;
    if (! loadWav (args.inPath, buffer, sampleRate)) return 1;
    std::printf ("loaded %s: %d ch, %d samples, %.0f Hz\n",
                 args.inPath.c_str(), buffer.getNumChannels(),
                 buffer.getNumSamples(), sampleRate);

    Oracle oracle;
    if (args.analyzeOnly)
    {
        printAnalysis (oracle.analyze (buffer, sampleRate));
        return 0;
    }

    std::printf ("\nbefore:\n");
    printAnalysis (oracle.analyze (buffer, sampleRate));

    Optimizer optimizer (oracle, sampleRate, args.config);
    const OptimizeResult r = optimizer.run (buffer);

    std::printf ("\nafter:\n");
    printAnalysis (oracle.analyze (buffer, sampleRate));

    std::printf ("likelihood %.4f -> %.4f   budget used %.2f / %.2f\n",
                 r.likelihoodBefore, r.likelihoodAfter,
                 r.budgetUsed, args.config.budget);
    std::printf ("audio delta: peak %.1f dBFS, rms %.1f dBFS\n",
                 r.peakDeltaDbFS, r.rmsDeltaDbFS);

    if (r.reachedTarget)
        std::printf ("TARGET REACHED\n");
    else
        std::printf ("REFUSED: %s\n", r.refuseReason.c_str());

    if (! writeWav (args.outPath, buffer, sampleRate)) return 1;
    std::printf ("wrote %s\n", args.outPath.c_str());
    return r.reachedTarget ? 0 : 2;
}
