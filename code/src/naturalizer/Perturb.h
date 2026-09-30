#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <functional>
#include <random>
#include <string>
#include <vector>

//==============================================================================
/**
    Counter-perturbation operators.

    Each operator nudges audio measurements in the opposite direction of one
    or more DAAT feature "suspicious" directions (see DESIGN.md §3 for the
    full map). Operators are deliberately small, local, and strength-scaled
    in [0,1]; the optimizer searches (operator, strength) pairs.

    `costPerUnit` is a perceptual-dose estimate used for the global budget:
    cheap, nearly-inaudible moves cost < 1, audible-risk moves cost more.

    All operators preserve channel count and length, never introduce NaN, and
    renormalize peak level where the transform changes overall gain.
*/
struct PerturbOp
{
    std::string id;
    std::string displayName;
    std::string targets;       // DAAT feature ids this operator aims at
    double costPerUnit = 1.0;

    std::function<void (juce::AudioBuffer<float>& buffer,
                        double strength,
                        std::mt19937_64& rng)> apply;
};

/** All operators, in a stable order. Operators that cannot act on the given
    material (e.g. stereo ops on mono) apply as safe no-ops. */
std::vector<PerturbOp> makeOperators (double sampleRate);
