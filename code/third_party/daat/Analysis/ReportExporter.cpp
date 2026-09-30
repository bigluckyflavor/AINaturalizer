#include "ReportExporter.h"
#include "FeatureRegistry.h"

#include <cmath>

namespace daat::report
{
namespace
{
    const char* const kToolName    = "DAAT AI Audio Inspector";
    const char* const kToolVersion = "0.7.0-research";

    // Non-finite numbers become JSON null (never the invalid token "nan").
    juce::var num (double v)
    {
        return std::isfinite (v) ? juce::var (v) : juce::var();
    }

    juce::DynamicObject::Ptr obj()
    {
        return juce::DynamicObject::Ptr (new juce::DynamicObject());
    }

    juce::var toVar (const juce::DynamicObject::Ptr& o)
    {
        return juce::var (o.get());
    }

    juce::var stringArray (const juce::StringArray& a)
    {
        juce::Array<juce::var> out;
        for (const auto& s : a)
            out.add (s);
        return out;
    }

    juce::var rawToVar (const WindowRawFeatures& r)
    {
        auto o = obj();
        o->setProperty ("rms", num (r.rms));
        o->setProperty ("peak", num (r.peak));
        o->setProperty ("crestFactorDb", num (r.crestFactorDb));
        o->setProperty ("spectralCentroidHz", num (r.spectralCentroidHz));
        o->setProperty ("spectralFlatness", num (r.spectralFlatness));
        o->setProperty ("spectralRolloffHz", num (r.spectralRolloffHz));
        o->setProperty ("highFrequencyRatio", num (r.highFrequencyRatio));
        o->setProperty ("spectralFlux", num (r.spectralFlux));
        o->setProperty ("leftRightCorrelation", num (r.leftRightCorrelation));
        o->setProperty ("midSideRatio", num (r.midSideRatio));
        o->setProperty ("transientVariance", num (r.transientVariance));
        o->setProperty ("noiseFloorStationarity", num (r.noiseFloorStationarity));
        o->setProperty ("microRepetition", num (r.microRepetition));
        return toVar (o);
    }

    juce::var featureToVar (const FeatureResult& f, bool withDescriptor)
    {
        auto o = obj();
        o->setProperty ("id", f.featureId);
        if (withDescriptor)
            if (const auto* d = findFeatureDescriptor (f.featureId))
            {
                o->setProperty ("displayName", d->displayName);
                o->setProperty ("group", toString (d->group));
            }
        o->setProperty ("valid", f.valid);
        o->setProperty ("raw", num (f.rawValue));
        o->setProperty ("suspicion", num (f.suspicionScore));
        o->setProperty ("confidence", num (f.confidence));
        o->setProperty ("weight", num (f.effectiveWeight));
        o->setProperty ("detail", f.explanation);
        return toVar (o);
    }

    juce::var evidenceToVar (const std::vector<EvidenceItem>& items)
    {
        juce::Array<juce::var> out;
        for (const auto& e : items)
        {
            auto o = obj();
            o->setProperty ("id", e.featureId);
            o->setProperty ("displayName", e.displayName);
            o->setProperty ("detail", e.detail);
            o->setProperty ("suspicion", num (e.suspicion));
            o->setProperty ("contribution", num (e.contribution));
            out.add (toVar (o));
        }
        return out;
    }

    juce::String cell (double v)
    {
        return std::isfinite (v) ? juce::String (v, 6) : juce::String();
    }

    const FeatureResult* findIn (const std::vector<FeatureResult>& fs, const juce::String& id)
    {
        for (const auto& f : fs)
            if (f.featureId == id)
                return &f;
        return nullptr;
    }
}

//==============================================================================
const juce::StringArray& provenanceLabels()
{
    static const juce::StringArray labels {
        "Unknown",
        "Human",
        "Fully AI generated",
        "AI vocal only",
        "AI instrumental only",
        "Hybrid",
        "Human recreation of AI composition",
        "AI mastered",
        "Stem separated"
    };
    return labels;
}

//==============================================================================
juce::var buildJsonReport (const AnalysisResult& r,
                           const DetectionProfile& profile,
                           const daat::detect::RuntimeControls& controls,
                           const ReportLabels& labels,
                           bool includeWindows)
{
    auto root = obj();
    root->setProperty ("reportFormatVersion", reportFormatVersion);
    root->setProperty ("tool", kToolName);
    root->setProperty ("toolVersion", kToolVersion);
    root->setProperty ("generatedUtc", juce::Time::getCurrentTime().toISO8601 (true));
    root->setProperty ("disclaimer",
        "Statistical screening estimate only. Audio processing, synthesis, editing, compression, "
        "mastering, and source separation may produce similar characteristics. This report does "
        "not prove how the recording was created. Thresholds are unvalidated research defaults.");

    auto source = obj();
    source->setProperty ("name", r.sourceName);
    source->setProperty ("path", r.sourcePath);
    source->setProperty ("sha256", r.sourceFileHash.isNotEmpty() ? juce::var (r.sourceFileHash) : juce::var());
    source->setProperty ("timelineSeconds", num (r.timelineSeconds));
    source->setProperty ("sampleRate", num (r.sourceSampleRate));
    source->setProperty ("channels", r.channels);
    source->setProperty ("truncated", r.truncated);
    source->setProperty ("audioIncluded", false);
    root->setProperty ("source", toVar (source));

    auto analysis = obj();
    analysis->setProperty ("analysisSampleRate", num (r.analysisSampleRate));
    analysis->setProperty ("analyzedSeconds", num (r.analyzedSeconds));
    analysis->setProperty ("effectiveSeconds", num (r.effectiveSeconds));
    analysis->setProperty ("isRangeAnalysis", r.isRangeAnalysis);
    analysis->setProperty ("rangeStartSeconds", num (r.rangeStartSeconds));
    analysis->setProperty ("rangeEndSeconds", num (r.rangeEndSeconds));
    juce::Array<juce::var> excluded;
    for (const auto& e : r.excludedRanges)
        excluded.add (juce::Array<juce::var> { num (e.getStart()), num (e.getEnd()) });
    analysis->setProperty ("excludedRanges", excluded);
    analysis->setProperty ("windowsTotal", r.totalWindows());
    analysis->setProperty ("windowsUsed", r.windowsUsed);
    root->setProperty ("analysis", toVar (analysis));

    auto result = obj();
    result->setProperty ("verdict", toString (r.verdict));
    result->setProperty ("description", r.summary);
    result->setProperty ("likelihood", num (r.overallLikelihood));
    result->setProperty ("confidence", num (r.confidence));
    result->setProperty ("agreeingGroups", r.numAgreeingGroups);
    result->setProperty ("activeGroups", r.numActiveGroups);
    result->setProperty ("validFeatures", r.numValidFeatures);
    result->setProperty ("peakLevel", num (r.peakLevel));
    result->setProperty ("rmsDb", num (r.rmsDb));
    result->setProperty ("clipped", r.clipped);
    root->setProperty ("result", toVar (result));

    juce::Array<juce::var> groups;
    for (const auto& g : r.groupScores)
    {
        auto o = obj();
        o->setProperty ("group", toString (g.group));
        o->setProperty ("valid", g.valid);
        o->setProperty ("score", num (g.score));
        o->setProperty ("confidence", num (g.confidence));
        o->setProperty ("weight", num (g.weight));
        o->setProperty ("validFeatures", g.numValidFeatures);
        groups.add (toVar (o));
    }
    root->setProperty ("groups", groups);

    juce::Array<juce::var> features;
    for (const auto& f : r.featureAverages)
        features.add (featureToVar (f, true));
    root->setProperty ("features", features);

    auto evidence = obj();
    evidence->setProperty ("supporting", evidenceToVar (r.strongestEvidence));
    evidence->setProperty ("contradictory", evidenceToVar (r.contradictoryEvidence));
    root->setProperty ("evidence", toVar (evidence));

    root->setProperty ("caveats", stringArray (r.caveats));
    root->setProperty ("limitations", stringArray (r.limitations));

    root->setProperty ("profile", profile.toVar());

    auto ctrl = obj();
    ctrl->setProperty ("sensitivity", num (controls.sensitivity));
    ctrl->setProperty ("decisionThreshold", num (controls.decisionThreshold));
    ctrl->setProperty ("minimumConfidence", num (controls.minimumConfidence));
    juce::StringArray enabledGroups;
    for (auto g : { FeatureGroup::spectral, FeatureGroup::temporal, FeatureGroup::dynamics,
                    FeatureGroup::stereo, FeatureGroup::noise, FeatureGroup::repetition,
                    FeatureGroup::vocal, FeatureGroup::fingerprint, FeatureGroup::model })
        if (controls.isGroupEnabled (g))
            enabledGroups.add (toString (g));
    ctrl->setProperty ("enabledGroups", stringArray (enabledGroups));
    root->setProperty ("controls", toVar (ctrl));

    auto lab = obj();
    lab->setProperty ("provenance", labels.provenance.isNotEmpty() ? labels.provenance : juce::String ("Unknown"));
    lab->setProperty ("generator", labels.generator);
    lab->setProperty ("processingHistory", labels.processingHistory);
    lab->setProperty ("notes", labels.notes);
    root->setProperty ("labels", toVar (lab));

    if (includeWindows)
    {
        juce::Array<juce::var> windows;
        for (const auto* ws : { &r.shortWindows, &r.mediumWindows, &r.longWindows })
        {
            for (const auto& w : *ws)
            {
                auto o = obj();
                o->setProperty ("scale", toString (w.scale));
                o->setProperty ("startSeconds", num (w.startSeconds));
                o->setProperty ("endSeconds", num (w.endSeconds));
                o->setProperty ("excluded", r.isExcluded (w.startSeconds, w.endSeconds));
                o->setProperty ("likelihood", num (w.likelihood));
                o->setProperty ("confidence", num (w.confidence));
                o->setProperty ("raw", rawToVar (w.raw));
                juce::Array<juce::var> wf;
                for (const auto& f : w.features)
                    wf.add (featureToVar (f, false));
                o->setProperty ("features", wf);
                windows.add (toVar (o));
            }
        }
        root->setProperty ("windows", windows);
    }

    return toVar (root);
}

juce::String toJsonString (const juce::var& report)
{
    return juce::JSON::toString (report, false);
}

//==============================================================================
juce::StringArray csvColumns()
{
    juce::StringArray c {
        "source_name", "source_sha256", "profile_name", "profile_version",
        "label_provenance", "label_generator", "label_processing",
        "scale", "start_s", "end_s", "excluded", "window_likelihood", "window_confidence",
        "raw_rms", "raw_peak", "raw_crest_db", "raw_centroid_hz", "raw_flatness", "raw_rolloff_hz",
        "raw_hf_ratio", "raw_flux", "raw_lr_correlation", "raw_mid_side", "raw_transient_cv",
        "raw_noise_stationarity", "raw_repetition"
    };

    for (const auto& d : getFeatureRegistry())
    {
        c.add (d.id + "_suspicion");
        c.add (d.id + "_confidence");
    }

    c.addArray ({ "overall_verdict", "overall_likelihood", "overall_confidence", "overall_effective_s" });
    return c;
}

juce::String csvEscape (const juce::String& field)
{
    if (field.containsAnyOf (",\"\r\n"))
        return "\"" + field.replace ("\"", "\"\"") + "\"";
    return field;
}

juce::String buildFeatureCsv (const AnalysisResult& r,
                              const DetectionProfile& profile,
                              const ReportLabels& labels,
                              bool includeHeader)
{
    juce::String out;
    const auto columns = csvColumns();

    if (includeHeader)
        out << columns.joinIntoString (",") << "\n";

    const auto& registry = getFeatureRegistry();
    const auto provenance = labels.provenance.isNotEmpty() ? labels.provenance : juce::String ("Unknown");

    for (const auto* ws : { &r.shortWindows, &r.mediumWindows, &r.longWindows })
    {
        for (const auto& w : *ws)
        {
            juce::StringArray row;
            row.add (csvEscape (r.sourceName));
            row.add (r.sourceFileHash);
            row.add (csvEscape (profile.profileName));
            row.add (juce::String (profile.profileVersion));
            row.add (csvEscape (provenance));
            row.add (csvEscape (labels.generator));
            row.add (csvEscape (labels.processingHistory));
            row.add (toString (w.scale));
            row.add (cell (w.startSeconds));
            row.add (cell (w.endSeconds));
            row.add (r.isExcluded (w.startSeconds, w.endSeconds) ? "1" : "0");
            row.add (cell (w.likelihood));
            row.add (cell (w.confidence));

            const auto& raw = w.raw;
            for (double v : { raw.rms, raw.peak, raw.crestFactorDb, raw.spectralCentroidHz, raw.spectralFlatness,
                              raw.spectralRolloffHz, raw.highFrequencyRatio, raw.spectralFlux,
                              raw.leftRightCorrelation, raw.midSideRatio, raw.transientVariance,
                              raw.noiseFloorStationarity, raw.microRepetition })
                row.add (cell (v));

            // Blank cells where a feature did not run on this window (window
            // assignment, disabled, or not applicable e.g. stereo on mono).
            for (const auto& d : registry)
            {
                const auto* f = findIn (w.features, d.id);
                const bool have = f != nullptr && f->valid;
                row.add (have ? cell (f->suspicionScore) : juce::String());
                row.add (have ? cell (f->confidence)     : juce::String());
            }

            row.add (csvEscape (toString (r.verdict)));
            row.add (cell (r.overallLikelihood));
            row.add (cell (r.confidence));
            row.add (cell (r.effectiveSeconds));

            jassert (row.size() == columns.size());
            out << row.joinIntoString (",") << "\n";
        }
    }

    return out;
}

//==============================================================================
bool writeTextFile (const juce::File& file, const juce::String& text, juce::String& error)
{
    if (! file.getParentDirectory().createDirectory())
    {
        error = "Could not create folder " + file.getParentDirectory().getFullPathName();
        return false;
    }

    juce::TemporaryFile temp (file);
    if (! temp.getFile().replaceWithText (text, false, false, "\n"))
    {
        error = "Could not write " + file.getFullPathName();
        return false;
    }
    if (! temp.overwriteTargetFileWithTemporary())
    {
        error = "Could not replace " + file.getFullPathName();
        return false;
    }

    error.clear();
    return true;
}

} // namespace daat::report
