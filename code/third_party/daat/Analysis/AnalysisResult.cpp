#include "AnalysisResult.h"

juce::String toString (AnalysisScale scale)
{
    switch (scale)
    {
        case AnalysisScale::shortScale:  return "short";
        case AnalysisScale::mediumScale: return "medium";
        case AnalysisScale::longScale:   return "long";
    }
    return "short";
}

juce::String toString (Verdict verdict)
{
    switch (verdict)
    {
        case Verdict::Unlikely:          return "Unlikely";
        case Verdict::Inconclusive:      return "Inconclusive";
        case Verdict::Likely:            return "Likely";
        case Verdict::InsufficientAudio: return "Insufficient audio";
        case Verdict::AnalysisFailed:    return "Analysis failed";
    }
    return "Inconclusive";
}
