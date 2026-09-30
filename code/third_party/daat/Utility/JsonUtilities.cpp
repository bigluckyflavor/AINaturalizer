#include "JsonUtilities.h"

namespace daat::json
{
    juce::var parseFile (const juce::File& file, juce::String& errorMessage)
    {
        if (! file.existsAsFile())
        {
            errorMessage = "File does not exist: " + file.getFullPathName();
            return {};
        }

        const auto text = file.loadFileAsString();
        juce::var result;
        const auto parseResult = juce::JSON::parse (text, result);

        if (parseResult.failed())
        {
            errorMessage = "JSON parse error: " + parseResult.getErrorMessage();
            return {};
        }

        errorMessage.clear();
        return result;
    }

    bool writeFile (const juce::File& file, const juce::var& value, juce::String& errorMessage)
    {
        if (! file.getParentDirectory().createDirectory())
        {
            errorMessage = "Could not create directory: " + file.getParentDirectory().getFullPathName();
            return false;
        }

        const auto text = juce::JSON::toString (value, false); // pretty-printed

        juce::TemporaryFile temp (file);
        if (! temp.getFile().replaceWithText (text))
        {
            errorMessage = "Could not write to temporary file for: " + file.getFullPathName();
            return false;
        }

        if (! temp.overwriteTargetFileWithTemporary())
        {
            errorMessage = "Could not replace target file: " + file.getFullPathName();
            return false;
        }

        errorMessage.clear();
        return true;
    }

    bool hasProperty (const juce::var& obj, const juce::Identifier& key)
    {
        if (auto* o = obj.getDynamicObject())
            return o->hasProperty (key);
        return false;
    }

    bool getBool (const juce::var& obj, const juce::Identifier& key, bool defaultValue)
    {
        if (auto* o = obj.getDynamicObject(); o != nullptr && o->hasProperty (key))
            return (bool) o->getProperty (key);
        return defaultValue;
    }

    double getDouble (const juce::var& obj, const juce::Identifier& key, double defaultValue)
    {
        if (auto* o = obj.getDynamicObject(); o != nullptr && o->hasProperty (key))
        {
            const auto v = o->getProperty (key);
            if (v.isDouble() || v.isInt() || v.isInt64())
                return (double) v;
        }
        return defaultValue;
    }

    int getInt (const juce::var& obj, const juce::Identifier& key, int defaultValue)
    {
        if (auto* o = obj.getDynamicObject(); o != nullptr && o->hasProperty (key))
        {
            const auto v = o->getProperty (key);
            if (v.isDouble() || v.isInt() || v.isInt64())
                return (int) v;
        }
        return defaultValue;
    }

    juce::String getString (const juce::var& obj, const juce::Identifier& key, const juce::String& defaultValue)
    {
        if (auto* o = obj.getDynamicObject(); o != nullptr && o->hasProperty (key))
            return o->getProperty (key).toString();
        return defaultValue;
    }
}
