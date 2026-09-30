// naturalizer — offline AI-music naturalizer (prototype).
//
//   naturalizer analyze in.wav
//       Print DAAT's likelihood, verdict, and per-feature suspicion table.
//
//   naturalizer in.wav out.wav [--target 0.28] [--max-iters 12]
//             [--budget 6.0] [--seed 1234]
//       Iteratively perturb the audio until DAAT's likelihood falls below
//       --target. A refused run writes no output file.

#include "naturalizer/Oracle.h"
#include "naturalizer/Optimize.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

namespace
{
    struct Args
    {
        bool analyzeOnly = false;
        std::string inPath, outPath;
        OptimizeConfig config;
    };

    bool parseDoubleArg (const char* text, double& out)
    {
        try
        {
            std::size_t used = 0;
            const std::string s (text);
            out = std::stod (s, &used);
            return used == s.size() && std::isfinite (out);
        }
        catch (...) { return false; }
    }

    bool parseIntArg (const char* text, int& out)
    {
        try
        {
            std::size_t used = 0;
            const std::string s (text);
            const long v = std::stol (s, &used);
            if (used != s.size() || v < 1 || v > std::numeric_limits<int>::max())
                return false;
            out = (int) v;
            return true;
        }
        catch (...) { return false; }
    }

    bool parseSeedArg (const char* text, uint64_t& out)
    {
        try
        {
            std::size_t used = 0;
            const std::string s (text);
            const auto v = std::stoull (s, &used);
            if (used != s.size())
                return false;
            out = (uint64_t) v;
            return true;
        }
        catch (...) { return false; }
    }

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
            const std::string k = argv[i];
            if (++i >= argc)
            {
                std::printf ("missing value for %s\n", k.c_str());
                return false;
            }

            if (k == "--target")
            {
                if (! parseDoubleArg (argv[i], a.config.targetLikelihood)
                    || a.config.targetLikelihood < 0.0
                    || a.config.targetLikelihood > 1.0)
                {
                    std::printf ("--target must be a finite value in [0,1]\n");
                    return false;
                }
            }
            else if (k == "--max-iters")
            {
                if (! parseIntArg (argv[i], a.config.maxIters))
                {
                    std::printf ("--max-iters must be a positive integer\n");
                    return false;
                }
            }
            else if (k == "--budget")
            {
                if (! parseDoubleArg (argv[i], a.config.budget) || a.config.budget < 0.0)
                {
                    std::printf ("--budget must be a finite non-negative value\n");
                    return false;
                }
            }
            else if (k == "--seed")
            {
                if (! parseSeedArg (argv[i], a.config.seed))
                {
                    std::printf ("--seed must be an unsigned integer\n");
                    return false;
                }
            }
            else
            {
                std::printf ("unknown option: %s\n", k.c_str());
                return false;
            }
        }
        return true;
    }

    void usage()
    {
        std::printf ("usage:\n"
                     "  naturalizer analyze in.wav\n"
                     "  naturalizer in.wav out.wav [--target 0.28] [--max-iters 12]\n"
                     "              [--budget 6.0] [--seed 1234]\n");
    }

    bool loadWav (const std::string& path, juce::AudioBuffer<float>& buffer, double& sampleRate)
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (
            fm.createReaderFor (juce::File (juce::String (path))));
        if (! reader) { std::printf ("cannot read %s\n", path.c_str()); return false; }
        if (reader->sampleRate <= 0.0 || reader->numChannels == 0 || reader->lengthInSamples <= 0)
        {
            std::printf ("invalid or empty audio file: %s\n", path.c_str());
            return false;
        }

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
        out->setPosition (0);
        out->truncate();

        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (
            out.get(), sampleRate, (unsigned int) buffer.getNumChannels(), 24, {}, 0));
        if (! writer) { std::printf ("cannot create writer for %s\n", path.c_str()); return false; }
        out.release();
        writer->writeFromAudioSampleBuffer (
            const_cast<juce::AudioBuffer<float>&> (buffer), 0, buffer.getNumSamples());
        return true;
    }

    void printAnalysis (const AnalysisOutcome& o)
    {
        std::printf ("\nlikelihood %.4f   confidence %.3f   verdict %s   (%d windows)\n",
                     o.likelihood, o.confidence,
                     o.verdict.empty() ? "Unscorable" : o.verdict.c_str(),
                     o.numWindows);
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

    std::printf ("\nafter attempted optimization:\n");
    printAnalysis (oracle.analyze (buffer, sampleRate));

    std::printf ("likelihood %.4f -> %.4f   budget used %.2f / %.2f\n",
                 r.likelihoodBefore, r.likelihoodAfter,
                 r.budgetUsed, args.config.budget);
    std::printf ("audio delta: peak %.1f dBFS, rms %.1f dBFS\n",
                 r.peakDeltaDbFS, r.rmsDeltaDbFS);

    if (! r.reachedTarget)
    {
        std::printf ("REFUSED: %s\n", r.refuseReason.c_str());
        std::printf ("no output written; any existing file at %s was left untouched\n",
                     args.outPath.c_str());
        return 2;
    }

    std::printf ("TARGET REACHED\n");
    if (! writeWav (args.outPath, buffer, sampleRate)) return 1;
    std::printf ("wrote %s\n", args.outPath.c_str());
    return 0;
}
