#include "DetectionProfile.h"
#include "FeatureRegistry.h"
#include "../Utility/JsonUtilities.h"

namespace
{
    bool isPowerOfTwo (int x) noexcept
    {
        return x > 0 && (x & (x - 1)) == 0;
    }

    juce::var cloneVar (const juce::var& v)
    {
        // Round-trip through JSON to deep-copy an object var.
        return juce::JSON::fromString (juce::JSON::toString (v));
    }

    juce::DynamicObject::Ptr newObject()
    {
        return juce::DynamicObject::Ptr (new juce::DynamicObject());
    }
}

//==============================================================================
DetectionProfile DetectionProfile::getFactoryDefault()
{
    DetectionProfile p;
    p.profileName    = "Balanced Research";
    p.profileVersion = currentVersion;
    // analysis / decision keep their struct defaults.

    for (const auto& d : getFeatureRegistry())
    {
        FeatureSetting fs;
        fs.enabled  = d.defaultEnabled;
        fs.weight   = d.defaultWeight;
        fs.scales   = d.defaultScales;
        fs.settings = cloneVar (d.defaultSettings);
        p.features[d.id] = fs;
    }

    // Group weights. Groups with no implemented features default to weight 0 so
    // they cannot influence a score before they actually exist.
    auto setGroup = [&p] (FeatureGroup g, bool enabled, double weight)
    {
        GroupSetting gs;
        gs.enabled = enabled;
        gs.weight  = weight;
        p.groups[toString (g)] = gs;
    };
    setGroup (FeatureGroup::spectral,    true,  1.00);
    setGroup (FeatureGroup::temporal,    true,  1.00);
    setGroup (FeatureGroup::dynamics,    true,  0.80);
    setGroup (FeatureGroup::stereo,      true,  0.80);
    setGroup (FeatureGroup::noise,       true,  0.90);
    setGroup (FeatureGroup::repetition,  true,  1.00);
    setGroup (FeatureGroup::vocal,       false, 0.00); // not implemented yet
    setGroup (FeatureGroup::fingerprint, false, 0.00); // not implemented yet
    setGroup (FeatureGroup::model,       false, 0.00); // Phase 8 (ONNX)

    return p;
}

const GroupSetting* DetectionProfile::enabledGroup (FeatureGroup group) const
{
    const auto it = groups.find (toString (group));
    if (it == groups.end() || ! it->second.enabled || it->second.weight <= 0.0)
        return nullptr;
    return &it->second;
}

double DetectionProfile::featureParam (const juce::String& featureId, const char* key, double fallback) const
{
    const auto it = features.find (featureId);
    if (it == features.end())
        return fallback;
    return daat::json::getDouble (it->second.settings, key, fallback);
}

bool DetectionProfile::requiresReanalysisComparedTo (const DetectionProfile& o) const
{
    const auto& a = analysis;
    const auto& b = o.analysis;

    return a.sampleRate != b.sampleRate
        || a.fftSize != b.fftSize
        || a.hopSize != b.hopSize
        || a.shortWindowSeconds != b.shortWindowSeconds
        || a.mediumWindowSeconds != b.mediumWindowSeconds
        || a.longWindowSeconds != b.longWindowSeconds
        || a.shortOverlap != b.shortOverlap
        || a.mediumOverlap != b.mediumOverlap
        || a.longOverlap != b.longOverlap
        || a.minimumAnalyzedSeconds != b.minimumAnalyzedSeconds
        || hfCutoffHz() != o.hfCutoffHz()
        || repetitionMinLagMs() != o.repetitionMinLagMs()
        || repetitionMaxLagMs() != o.repetitionMaxLagMs();
}

//==============================================================================
juce::var DetectionProfile::toVar() const
{
    auto root = newObject();
    root->setProperty ("profileName", profileName);
    root->setProperty ("profileVersion", profileVersion);

    auto a = newObject();
    a->setProperty ("sampleRate", analysis.sampleRate);
    a->setProperty ("fftSize", analysis.fftSize);
    a->setProperty ("hopSize", analysis.hopSize);
    a->setProperty ("shortWindowSeconds", analysis.shortWindowSeconds);
    a->setProperty ("mediumWindowSeconds", analysis.mediumWindowSeconds);
    a->setProperty ("longWindowSeconds", analysis.longWindowSeconds);
    a->setProperty ("shortOverlap", analysis.shortOverlap);
    a->setProperty ("mediumOverlap", analysis.mediumOverlap);
    a->setProperty ("longOverlap", analysis.longOverlap);
    a->setProperty ("minimumAnalyzedSeconds", analysis.minimumAnalyzedSeconds);
    root->setProperty ("analysis", juce::var (a.get()));

    auto d = newObject();
    d->setProperty ("likelyThreshold", decision.likelyThreshold);
    d->setProperty ("unlikelyThreshold", decision.unlikelyThreshold);
    d->setProperty ("minimumConfidence", decision.minimumConfidence);
    d->setProperty ("minimumActiveFeatureWeight", decision.minimumActiveFeatureWeight);
    d->setProperty ("requireMultipleFeatureGroups", decision.requireMultipleFeatureGroups);
    d->setProperty ("minimumAgreeingGroups", decision.minimumAgreeingGroups);
    root->setProperty ("decision", juce::var (d.get()));

    auto g = newObject();
    for (const auto& [name, gs] : groups)
    {
        auto entry = newObject();
        entry->setProperty ("enabled", gs.enabled);
        entry->setProperty ("weight", gs.weight);
        g->setProperty (juce::Identifier (name), juce::var (entry.get()));
    }
    root->setProperty ("groups", juce::var (g.get()));

    auto f = newObject();
    for (const auto& [id, fs] : features)
    {
        auto entry = newObject();
        entry->setProperty ("enabled", fs.enabled);
        entry->setProperty ("weight", fs.weight);
        entry->setProperty ("windows", scaleMaskToString (fs.scales));

        // Flatten the per-feature settings object into the same entry.
        if (auto* s = fs.settings.getDynamicObject())
            for (const auto& prop : s->getProperties())
                entry->setProperty (prop.name, prop.value);

        f->setProperty (juce::Identifier (id), juce::var (entry.get()));
    }
    root->setProperty ("features", juce::var (f.get()));

    return juce::var (root.get());
}

juce::String DetectionProfile::toJsonString() const
{
    return juce::JSON::toString (toVar(), false);
}

//==============================================================================
DetectionProfile DetectionProfile::fromVar (const juce::var& sourceIn, ProfileValidation& result)
{
    DetectionProfile p = getFactoryDefault();

    if (! sourceIn.isObject())
    {
        result.addError ("Profile root is not a JSON object.");
        return p;
    }

    // Deep copy: migration edits `source` in place, and a plain var copy would
    // share (and so rewrite) the caller's DynamicObjects.
    juce::var source = sourceIn.clone();

    const int version = daat::json::getInt (source, "profileVersion", 0);
    result.loadedVersion = version;

    if (version <= 0)
        result.addWarning ("Profile has no version; assuming current (" + juce::String (currentVersion) + ").");
    else if (version > currentVersion)
        result.addError ("Profile version " + juce::String (version)
                         + " is newer than this build supports (" + juce::String (currentVersion) + ").");
    else if (version < currentVersion)
        migrate (source, version, result);

    p.profileName    = daat::json::getString (source, "profileName", p.profileName);
    p.profileVersion = currentVersion;

    if (auto av = source.getDynamicObject(); av != nullptr && av->hasProperty ("analysis"))
    {
        const auto a = av->getProperty ("analysis");
        p.analysis.sampleRate             = daat::json::getDouble (a, "sampleRate", p.analysis.sampleRate);
        p.analysis.fftSize                = daat::json::getInt    (a, "fftSize", p.analysis.fftSize);
        p.analysis.hopSize                = daat::json::getInt    (a, "hopSize", p.analysis.hopSize);
        p.analysis.shortWindowSeconds     = daat::json::getDouble (a, "shortWindowSeconds", p.analysis.shortWindowSeconds);
        p.analysis.mediumWindowSeconds    = daat::json::getDouble (a, "mediumWindowSeconds", p.analysis.mediumWindowSeconds);
        p.analysis.longWindowSeconds      = daat::json::getDouble (a, "longWindowSeconds", p.analysis.longWindowSeconds);
        p.analysis.shortOverlap           = daat::json::getDouble (a, "shortOverlap", p.analysis.shortOverlap);
        p.analysis.mediumOverlap          = daat::json::getDouble (a, "mediumOverlap", p.analysis.mediumOverlap);
        p.analysis.longOverlap            = daat::json::getDouble (a, "longOverlap", p.analysis.longOverlap);
        p.analysis.minimumAnalyzedSeconds = daat::json::getDouble (a, "minimumAnalyzedSeconds", p.analysis.minimumAnalyzedSeconds);
    }

    if (auto dv = source.getDynamicObject(); dv != nullptr && dv->hasProperty ("decision"))
    {
        const auto d = dv->getProperty ("decision");
        p.decision.likelyThreshold             = daat::json::getDouble (d, "likelyThreshold", p.decision.likelyThreshold);
        p.decision.unlikelyThreshold           = daat::json::getDouble (d, "unlikelyThreshold", p.decision.unlikelyThreshold);
        p.decision.minimumConfidence           = daat::json::getDouble (d, "minimumConfidence", p.decision.minimumConfidence);
        p.decision.minimumActiveFeatureWeight  = daat::json::getDouble (d, "minimumActiveFeatureWeight", p.decision.minimumActiveFeatureWeight);
        p.decision.requireMultipleFeatureGroups = daat::json::getBool  (d, "requireMultipleFeatureGroups", p.decision.requireMultipleFeatureGroups);
        p.decision.minimumAgreeingGroups       = daat::json::getInt    (d, "minimumAgreeingGroups", p.decision.minimumAgreeingGroups);
    }

    if (auto gv = source.getDynamicObject(); gv != nullptr && gv->hasProperty ("groups"))
    {
        const auto groupsVar = gv->getProperty ("groups");
        if (auto* gobj = groupsVar.getDynamicObject())
        {
            for (const auto& prop : gobj->getProperties())
            {
                const juce::String name = prop.name.toString();
                bool known = false;
                featureGroupFromString (name, known);
                if (! known)
                {
                    result.addWarning ("Unknown feature group '" + name + "' ignored.");
                    continue;
                }

                GroupSetting gs = p.groups.count (name) ? p.groups[name] : GroupSetting{};
                gs.enabled = daat::json::getBool   (prop.value, "enabled", gs.enabled);
                gs.weight  = daat::json::getDouble (prop.value, "weight", gs.weight);
                p.groups[name] = gs;
            }
        }
    }

    if (auto fv = source.getDynamicObject(); fv != nullptr && fv->hasProperty ("features"))
    {
        const auto featuresVar = fv->getProperty ("features");
        if (auto* fobj = featuresVar.getDynamicObject())
        {
            for (const auto& prop : fobj->getProperties())
            {
                const juce::String id = prop.name.toString();
                const auto entry = prop.value;

                if (findFeatureDescriptor (id) == nullptr)
                {
                    result.addWarning ("Unknown feature '" + id + "' ignored.");
                    continue;
                }

                FeatureSetting fs = p.features.count (id) ? p.features[id] : FeatureSetting{};
                fs.enabled = daat::json::getBool   (entry, "enabled", fs.enabled);
                fs.weight  = daat::json::getDouble (entry, "weight", fs.weight);

                if (daat::json::hasProperty (entry, "windows"))
                {
                    bool scalesOk = false;
                    const auto mask = scaleMaskFromString (daat::json::getString (entry, "windows", {}), scalesOk);
                    if (scalesOk)
                        fs.scales = mask;
                    else
                        result.addError ("Feature '" + id + "' has an unrecognised window list "
                                         "(expected any of short, medium, long).");
                }

                // Merge the remaining keys over the defaults, so a profile that
                // omits a threshold keeps its default instead of losing it.
                auto merged = cloneVar (fs.settings);
                if (! merged.isObject())
                    merged = juce::var (newObject().get());

                if (auto* eobj = entry.getDynamicObject())
                    for (const auto& ep : eobj->getProperties())
                        if (ep.name != juce::Identifier ("enabled")
                            && ep.name != juce::Identifier ("weight")
                            && ep.name != juce::Identifier ("windows"))
                            merged.getDynamicObject()->setProperty (ep.name, ep.value);
                fs.settings = merged;

                p.features[id] = fs;
            }
        }
    }

    // Fold structural validation into the same result object.
    const auto v = p.validate();
    result.errors.addArray (v.errors);
    result.warnings.addArray (v.warnings);
    if (! v.ok)
        result.ok = false;

    return p;
}

DetectionProfile DetectionProfile::fromJsonString (const juce::String& json, ProfileValidation& result)
{
    juce::var parsed;
    const auto parseResult = juce::JSON::parse (json, parsed);
    if (parseResult.failed())
    {
        result.addError ("JSON parse error: " + parseResult.getErrorMessage());
        return getFactoryDefault();
    }
    return fromVar (parsed, result);
}

//==============================================================================
void DetectionProfile::migrate (juce::var& source, int fromVersion, ProfileValidation& result)
{
    // Each step upgrades one version in place, so old profiles walk the chain.
    int version = fromVersion;

    if (version == 1)
    {
        // v1 -> v2 (Phases 4-6):
        //  - transientVariance became a coefficient of variation; the v1
        //    placeholder threshold 0.10 means almost nothing on that scale.
        //  - Group weights and per-feature window assignment did not exist.
        auto* root = source.getDynamicObject();
        auto* features = root != nullptr ? root->getProperty ("features").getDynamicObject() : nullptr;

        if (features != nullptr)
        {
            if (auto* tv = features->getProperty ("transientVariance").getDynamicObject())
            {
                const auto thr = tv->getProperty ("suspiciousThreshold");
                if ((thr.isDouble() || thr.isInt()) && std::abs ((double) thr - 0.10) < 1.0e-9)
                {
                    tv->setProperty ("suspiciousThreshold", 0.35);
                    result.addWarning ("transientVariance threshold 0.10 (v1 placeholder) predates the "
                                       "coefficient-of-variation measure; replaced with 0.35.");
                }
            }
        }

        if (root != nullptr && ! root->hasProperty ("groups"))
            result.addWarning ("Group weights were added in version 2; factory group weights applied.");

        bool anyWindows = false;
        if (features != nullptr)
            for (const auto& f : features->getProperties())
                if (auto* fo = f.value.getDynamicObject(); fo != nullptr && fo->hasProperty ("windows"))
                    anyWindows = true;
        if (! anyWindows)
            result.addWarning ("Per-feature window assignment was added in version 2; defaults applied.");

        version = 2;
    }

    // Future: if (version == 2) { ...; version = 3; }

    result.migrated = true;
    result.addWarning ("Profile migrated from version " + juce::String (fromVersion)
                       + " to " + juce::String (version) + ".");
}

//==============================================================================
bool DetectionProfile::saveToFile (const juce::File& file, juce::String& errorMessage) const
{
    return daat::json::writeFile (file, toVar(), errorMessage);
}

DetectionProfile DetectionProfile::loadFromFile (const juce::File& file, ProfileValidation& result)
{
    juce::String err;
    const auto parsed = daat::json::parseFile (file, err);
    if (err.isNotEmpty())
    {
        result.addError (err);
        return getFactoryDefault();
    }
    return fromVar (parsed, result);
}

//==============================================================================
double DetectionProfile::totalActiveWeight() const
{
    double sum = 0.0;
    for (const auto& [id, fs] : features)
        if (fs.enabled)
            sum += juce::jmax (0.0, fs.weight);
    return sum;
}

ProfileValidation DetectionProfile::validate() const
{
    ProfileValidation r;
    r.loadedVersion = profileVersion;

    // --- Analysis block ---
    if (analysis.fftSize <= 0)
        r.addError ("FFT size must be positive (got " + juce::String (analysis.fftSize) + ").");
    else if (! isPowerOfTwo (analysis.fftSize))
        r.addError ("FFT size must be a power of two (got " + juce::String (analysis.fftSize) + ").");

    if (analysis.hopSize <= 0)
        r.addError ("Hop size must be positive (got " + juce::String (analysis.hopSize) + ").");
    else if (analysis.hopSize > analysis.fftSize)
        r.addError ("Hop size (" + juce::String (analysis.hopSize)
                    + ") must not exceed FFT size (" + juce::String (analysis.fftSize) + ").");

    if (analysis.sampleRate < 22050.0 || analysis.sampleRate > 96000.0)
        r.addError ("Analysis sample rate " + juce::String (analysis.sampleRate, 0)
                    + " Hz is unsupported (allowed 22050-96000 Hz).");
    else if (analysis.sampleRate != 44100.0 && analysis.sampleRate != 48000.0)
        r.addWarning ("Analysis sample rate " + juce::String (analysis.sampleRate, 0)
                      + " Hz is outside the tested set (44100 / 48000).");

    if (analysis.fftSize > 32768)
        r.addError ("FFT size " + juce::String (analysis.fftSize)
                    + " would need excessive memory and time (maximum 32768).");

    if (analysis.longWindowSeconds > 120.0)
        r.addError ("Long window of " + juce::String (analysis.longWindowSeconds, 1)
                    + " s is excessive (maximum 120 s).");

    // Worst-case working buffer: 300 s cap x analysis rate x 2 channels x 4 bytes.
    const double workingMB = 300.0 * analysis.sampleRate * 2.0 * 4.0 / (1024.0 * 1024.0);
    if (workingMB > 512.0)
        r.addError ("Settings would need about " + juce::String (workingMB, 0)
                    + " MB of working memory (maximum 512 MB).");

    auto checkWindow = [&r] (const char* name, double secs)
    {
        if (secs <= 0.0)
            r.addError (juce::String (name) + " window seconds must be positive (got "
                        + juce::String (secs, 2) + ").");
    };
    checkWindow ("Short",  analysis.shortWindowSeconds);
    checkWindow ("Medium", analysis.mediumWindowSeconds);
    checkWindow ("Long",   analysis.longWindowSeconds);

    if (! (analysis.shortWindowSeconds <= analysis.mediumWindowSeconds
           && analysis.mediumWindowSeconds <= analysis.longWindowSeconds))
        r.addWarning ("Window sizes are not ordered short <= medium <= long.");

    auto checkOverlap = [&r] (const char* name, double o)
    {
        if (o < 0.0 || o > 0.95)
            r.addError (juce::String (name) + " overlap must be within 0..0.95 (got "
                        + juce::String (o, 2) + ").");
    };
    checkOverlap ("Short",  analysis.shortOverlap);
    checkOverlap ("Medium", analysis.mediumOverlap);
    checkOverlap ("Long",   analysis.longOverlap);

    if (analysis.minimumAnalyzedSeconds < 0.0)
        r.addError ("minimumAnalyzedSeconds must be >= 0.");

    // --- Decision block ---
    auto checkUnit = [&r] (const char* name, double v)
    {
        if (v < 0.0 || v > 1.0)
            r.addError (juce::String (name) + " must be within 0..1 (got " + juce::String (v, 3) + ").");
    };
    checkUnit ("likelyThreshold", decision.likelyThreshold);
    checkUnit ("unlikelyThreshold", decision.unlikelyThreshold);
    checkUnit ("minimumConfidence", decision.minimumConfidence);
    checkUnit ("minimumActiveFeatureWeight", decision.minimumActiveFeatureWeight);

    if (decision.unlikelyThreshold >= decision.likelyThreshold)
        r.addError ("unlikelyThreshold (" + juce::String (decision.unlikelyThreshold, 3)
                    + ") must be below likelyThreshold (" + juce::String (decision.likelyThreshold, 3) + ").");

    if (decision.minimumAgreeingGroups < 1)
        r.addError ("minimumAgreeingGroups must be >= 1.");

    // --- Features ---
    if (features.empty())
        r.addError ("Profile contains no features.");

    const double nyquist = 0.5 * analysis.sampleRate;

    bool anyEnabledPositive = false;
    for (const auto& [id, fs] : features)
    {
        if (fs.weight < 0.0)
            r.addError ("Feature '" + id + "' has a negative weight.");
        if (fs.enabled && fs.weight > 0.0)
            anyEnabledPositive = true;

        if (fs.enabled && (fs.scales & ScaleMask::all) == 0)
            r.addError ("Feature '" + id + "' is enabled but assigned to no window scale.");

        const auto& s = fs.settings;
        const auto has = [&s] (const char* k) { return daat::json::hasProperty (s, k); };
        const auto num = [&s] (const char* k) { return daat::json::getDouble (s, k, 0.0); };

        if (has ("suspiciousLow") && has ("suspiciousHigh") && num ("suspiciousLow") >= num ("suspiciousHigh"))
            r.addError ("Feature '" + id + "': suspiciousLow must be below suspiciousHigh.");

        if (has ("suspiciousLowHz") && has ("suspiciousHighHz"))
        {
            const double lo = num ("suspiciousLowHz"), hi = num ("suspiciousHighHz");
            if (lo <= 0.0 || lo >= hi || hi > nyquist)
                r.addError ("Feature '" + id + "': frequency range " + juce::String (lo, 0) + "-"
                            + juce::String (hi, 0) + " Hz is invalid (need 0 < low < high <= "
                            + juce::String (nyquist, 0) + " Hz).");
        }

        if (has ("cutoffHz") && (num ("cutoffHz") <= 0.0 || num ("cutoffHz") >= nyquist))
            r.addError ("Feature '" + id + "': cutoff " + juce::String (num ("cutoffHz"), 0)
                        + " Hz must be between 0 and Nyquist (" + juce::String (nyquist, 0) + " Hz).");

        if (has ("minimumLagMs") && has ("maximumLagMs")
            && (num ("minimumLagMs") <= 0.0 || num ("minimumLagMs") >= num ("maximumLagMs")
                || num ("maximumLagMs") > 10000.0))
            r.addError ("Feature '" + id + "': lag range must satisfy 0 < minimum < maximum <= 10000 ms.");

        if (has ("expectedHumanStdDev") && num ("expectedHumanStdDev") <= 0.0)
            r.addError ("Feature '" + id + "': expectedHumanStdDev must be positive.");

        // Features whose raw measurement lives in 0..1.
        const bool unitRange = id == "stereoCorrelation" || id == "noiseFloorStationarity"
                            || id == "microRepetition"   || id == "transientVariance";
        if (unitRange && has ("suspiciousThreshold")
            && (num ("suspiciousThreshold") < 0.0 || num ("suspiciousThreshold") >= 1.0))
            r.addError ("Feature '" + id + "': suspiciousThreshold must be within 0..1 (exclusive of 1).");

        if (id == "spectralFlux" && has ("suspiciousThreshold") && num ("suspiciousThreshold") <= 0.0)
            r.addError ("Feature 'spectralFlux': suspiciousThreshold must be positive.");

        if (has ("suspiciousThresholdDb")
            && (num ("suspiciousThresholdDb") <= 0.0 || num ("suspiciousThresholdDb") > 40.0))
            r.addError ("Feature '" + id + "': suspiciousThresholdDb must be within 0..40 dB.");
    }
    if (! anyEnabledPositive)
        r.addError ("No enabled feature has a positive weight - nothing to score.");

    // --- Groups ---
    bool anyGroupPositive = false;
    for (const auto& [name, gs] : groups)
    {
        if (gs.weight < 0.0)
            r.addError ("Group '" + name + "' has a negative weight.");
        if (gs.enabled && gs.weight > 0.0)
            anyGroupPositive = true;
    }
    if (! groups.empty() && ! anyGroupPositive)
        r.addError ("No enabled feature group has a positive weight - nothing to score.");

    return r;
}
