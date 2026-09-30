#pragma once

#include "AnalysisResult.h"
#include "DetectionProfile.h"
#include "DetectionEngine.h"

//==============================================================================
/** User-supplied labels attached to exports for later training / calibration
    (spec §17). None of these are inferred by the plugin. */
struct ReportLabels
{
    juce::String provenance;         // one of daat::report::provenanceLabels()
    juce::String generator;          // free text, e.g. the generator or service, if known
    juce::String processingHistory;  // free text, e.g. "mastered, MP3 128k, stem-separated"
    juce::String notes;
};

//==============================================================================
/**
    JSON report and CSV feature export (spec §17 / Phase 7).

    Exports measurements, scores, settings, provenance, and labels only - never
    audio. Pure functions over an immutable result, so they are safe to call
    from any thread and unit-testable without a host.
*/
namespace daat::report
{
    constexpr int reportFormatVersion = 1;

    /** The spec's provenance labels; "Unknown" is the default. */
    const juce::StringArray& provenanceLabels();

    /** Full report as a JSON object var. With includeWindows, every analysis
        window's raw measurements and per-feature results are included. */
    juce::var buildJsonReport (const AnalysisResult& result,
                               const DetectionProfile& profile,
                               const daat::detect::RuntimeControls& controls,
                               const ReportLabels& labels,
                               bool includeWindows);

    juce::String toJsonString (const juce::var& report);

    /** CSV header for the per-window feature export. Column order is stable,
        so rows from many files can be appended into one training table. */
    juce::StringArray csvColumns();

    /** One CSV row per analysis window (short, medium, then long), with the
        header first if requested. */
    juce::String buildFeatureCsv (const AnalysisResult& result,
                                  const DetectionProfile& profile,
                                  const ReportLabels& labels,
                                  bool includeHeader = true);

    /** RFC 4180 field escaping. */
    juce::String csvEscape (const juce::String& field);

    /** Writes text via a temporary file, so a failed write never leaves a
        truncated report behind. */
    bool writeTextFile (const juce::File& file, const juce::String& text, juce::String& error);
}
