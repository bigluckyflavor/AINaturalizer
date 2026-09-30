cmake_minimum_required(VERSION 3.24)
project(ainaturalizer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- JUCE modules, compiled directly (no JUCE CMake, no X11, headless) -------
include(FetchContent)
set(JUCE_TAG "8.0.6" CACHE STRING "JUCE release tag to fetch")
FetchContent_Declare(
    JUCE
    URL "https://github.com/juce-framework/JUCE/archive/refs/tags/${JUCE_TAG}.zip"
    SOURCE_SUBDIR "nonexistent-subdir"  # don't run JUCE's own CMake
)
FetchContent_MakeAvailable(JUCE)
set(JUCE_MODULES_DIR ${juce_SOURCE_DIR}/modules)

# Only the modules the measurement code and CLI need. Compiling the module
# .cpp files directly is JUCE's classic static-library approach; none of
# these touch the GUI or X11.
add_library(juce_modules STATIC
    src/juce_stamps.cpp
    ${JUCE_MODULES_DIR}/juce_core/juce_core.cpp
    ${JUCE_MODULES_DIR}/juce_events/juce_events.cpp
    ${JUCE_MODULES_DIR}/juce_audio_basics/juce_audio_basics.cpp
    ${JUCE_MODULES_DIR}/juce_dsp/juce_dsp.cpp
    ${JUCE_MODULES_DIR}/juce_audio_formats/juce_audio_formats.cpp
)
target_include_directories(juce_modules PUBLIC ${JUCE_MODULES_DIR})
target_compile_definitions(juce_modules PUBLIC
    JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
    JUCE_STANDALONE_APPLICATION=1
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_DISABLE_JUCE_VERSION_PRINTING=1
)
if (UNIX)
    target_link_libraries(juce_modules PUBLIC pthread dl)
endif()

# ---- Vendored DAAT measurement code ------------------------------------------
# Unmodified sources from the DAAT AI Audio Inspector: the feature extractor,
# the 11 heuristic feature evaluators, and the detection engine. They form the
# measurement oracle — the loss function the optimizer works against.
set(DAAT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/daat)
set(DAAT_SOURCES
    ${DAAT_DIR}/Analysis/AnalysisResult.cpp
    ${DAAT_DIR}/Analysis/FeatureExtractor.cpp
    ${DAAT_DIR}/Analysis/FeatureRegistry.cpp
    ${DAAT_DIR}/Analysis/DetectionProfile.cpp
    ${DAAT_DIR}/Analysis/DetectionEngine.cpp
    ${DAAT_DIR}/Features/DynamicsFeatures.cpp
    ${DAAT_DIR}/Features/NoiseFeatures.cpp
    ${DAAT_DIR}/Features/RepetitionFeatures.cpp
    ${DAAT_DIR}/Features/SpectralFeatures.cpp
    ${DAAT_DIR}/Features/StereoFeatures.cpp
    ${DAAT_DIR}/Features/TemporalFeatures.cpp
    ${DAAT_DIR}/Features/FeatureEvaluation.cpp
    ${DAAT_DIR}/Utility/JsonUtilities.cpp
)

# ---- The naturalizer ----------------------------------------------------------
add_executable(naturalizer
    src/main.cpp
    src/naturalizer/Oracle.cpp
    src/naturalizer/Perturb.cpp
    src/naturalizer/Optimize.cpp
    ${DAAT_SOURCES}
)

target_include_directories(naturalizer PRIVATE
    src
    third_party/daat
)

target_link_libraries(naturalizer PRIVATE juce_modules)

target_compile_definitions(naturalizer PRIVATE
    JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
    JUCE_STANDALONE_APPLICATION=1
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_DISABLE_JUCE_VERSION_PRINTING=1
)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(naturalizer PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()
