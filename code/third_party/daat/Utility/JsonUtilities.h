#pragma once

#include <juce_core/juce_core.h>

//==============================================================================
/**
    Thin helpers over juce::var / juce::JSON for reading detection profiles.

    Every getter takes a default and never throws, so profile parsing can be
    tolerant of missing keys while still reporting which values were absent or
    out of the expected type. Uses only juce_core - no third-party JSON library.
*/
namespace daat::json
{
    /** Reads a whole file as parsed JSON. Returns a void var on failure and
        sets errorMessage. */
    juce::var parseFile (const juce::File& file, juce::String& errorMessage);

    /** Writes var as pretty-printed JSON to file. Returns false + errorMessage
        on failure. */
    bool writeFile (const juce::File& file, const juce::var& value, juce::String& errorMessage);

    bool   getBool   (const juce::var& obj, const juce::Identifier& key, bool defaultValue);
    double getDouble (const juce::var& obj, const juce::Identifier& key, double defaultValue);
    int    getInt    (const juce::var& obj, const juce::Identifier& key, int defaultValue);
    juce::String getString (const juce::var& obj, const juce::Identifier& key, const juce::String& defaultValue);

    /** True if obj is an object that contains key. */
    bool hasProperty (const juce::var& obj, const juce::Identifier& key);
}
