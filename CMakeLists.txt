cmake_minimum_required(VERSION 3.22)

project(KITNAP VERSION 0.2.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")

if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary")
endif()

option(KITNAP_BUILD_TESTS "Build the analyzer test" ON)
option(KITNAP_WITH_AI "Bundle ONNX Runtime and the AI drum separation model" ON)
option(KITNAP_WITH_DRUM_SPLIT "Convert the AI drum split model (needs Python with torch and onnx)" ON)
option(KITNAP_BUILD_SCREENSHOT "Build the UI screenshot tool (Linux, needs a display)" OFF)

# JUCE is downloaded automatically. Point KITNAP_JUCE_PATH at a local checkout to skip the download.
set(KITNAP_JUCE_PATH "" CACHE PATH "Optional local JUCE checkout")

if(KITNAP_JUCE_PATH)
    add_subdirectory(${KITNAP_JUCE_PATH} JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

if(KITNAP_WITH_AI)
    include(cmake/KitnapAI.cmake)
endif()

set(KITNAP_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND KITNAP_FORMATS AU)
endif()
if(UNIX AND NOT APPLE)
    list(APPEND KITNAP_FORMATS LV2)
endif()

juce_add_plugin(KITNAP
    COMPANY_NAME "FortyVolts"
    BUNDLE_ID "com.kitnapaudio.kitnap"
    PLUGIN_MANUFACTURER_CODE Ktnp
    PLUGIN_CODE Ktn1
    FORMATS ${KITNAP_FORMATS}
    PRODUCT_NAME "KITNAP"
    LV2URI "https://github.com/tjerkiewisse/newplugin"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Instrument Drum Sampler
    AU_MAIN_TYPE kAudioUnitType_MusicDevice)

target_sources(KITNAP PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/analysis/DrumAnalyzer.cpp
    Source/analysis/SongSections.cpp
    Source/separation/DrumSeparator.cpp
    Source/engine/KitSampler.cpp
    Source/ui/WaveformView.cpp
    Source/ui/PadComponent.cpp
    Source/ui/FilmLook.cpp)

target_include_directories(KITNAP PRIVATE Source)

set(KITNAP_DEFINITIONS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_WINDOWS_MEDIA_FORMAT=1
    JUCE_VST3_CAN_REPLACE_VST2=0)

# Sets up a target to use AI separation: ONNX Runtime headers, and the runtime copied to
# `resources` (plus the models with WITH_MODELS).
function(kitnap_use_ai target resources)
    if(KITNAP_AI_AVAILABLE)
        target_include_directories(${target} PRIVATE "${KITNAP_ORT_INCLUDE_DIR}")
        # The runtime's exact file name, so a stale copy of another version is never picked up.
        get_filename_component(_ort_name "${KITNAP_ORT_LIBRARY}" NAME)
        target_compile_definitions(${target} PRIVATE KITNAP_HAS_ONNX=1 KITNAP_ORT_LIBRARY_NAME="${_ort_name}")
        if(resources)
            kitnap_bundle_ai(${target} "${resources}" ${ARGN})
        endif()
    endif()
endfunction()

target_compile_definitions(KITNAP PUBLIC ${KITNAP_DEFINITIONS})
kitnap_use_ai(KITNAP "")

# Fonts and the FortyVolts logo, compiled into the binary (BinaryData.h).
juce_add_binary_data(KitnapAssets SOURCES
    Resources/fonts/Michroma-Regular.ttf
    Resources/fonts/IBMPlexSans-Regular.ttf
    Resources/fonts/IBMPlexSans-Medium.ttf
    Resources/logo/fortyvolts-lockup.svg)

# The runtime goes into Contents/Resources of every bundle, or Resources/ next to the binary.
foreach(format IN LISTS KITNAP_FORMATS)
    set(format_target KITNAP_${format})
    if(format STREQUAL "VST3" OR format STREQUAL "AU" OR (APPLE AND format STREQUAL "Standalone"))
        kitnap_use_ai(${format_target} "$<GENEX_EVAL:$<TARGET_PROPERTY:${format_target},JUCE_PLUGIN_ARTEFACT_FILE>>/Contents/Resources")
    else()
        kitnap_use_ai(${format_target} "$<TARGET_FILE_DIR:${format_target}>/Resources")
    endif()
endforeach()

target_link_libraries(KITNAP
    PRIVATE
        KitnapAssets
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

if(KITNAP_BUILD_TESTS)
    juce_add_console_app(KitnapAnalyzerTest PRODUCT_NAME "KitnapAnalyzerTest")
    target_sources(KitnapAnalyzerTest PRIVATE
        tests/AnalyzerTest.cpp
        Source/analysis/DrumAnalyzer.cpp
        Source/analysis/SongSections.cpp
        Source/separation/DrumSeparator.cpp)
    target_include_directories(KitnapAnalyzerTest PRIVATE Source)
    target_compile_definitions(KitnapAnalyzerTest PRIVATE ${KITNAP_DEFINITIONS})
    kitnap_use_ai(KitnapAnalyzerTest "$<TARGET_FILE_DIR:KitnapAnalyzerTest>/Resources" WITH_MODELS)
    target_link_libraries(KitnapAnalyzerTest
        PRIVATE
            juce::juce_audio_formats
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endif()

if(KITNAP_BUILD_SCREENSHOT)
    juce_add_gui_app(KitnapScreenshot PRODUCT_NAME "KitnapScreenshot")
    target_sources(KitnapScreenshot PRIVATE
        tests/Screenshot.cpp
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/analysis/DrumAnalyzer.cpp
        Source/analysis/SongSections.cpp
        Source/separation/DrumSeparator.cpp
        Source/engine/KitSampler.cpp
        Source/ui/WaveformView.cpp
        Source/ui/PadComponent.cpp
        Source/ui/FilmLook.cpp)
    target_include_directories(KitnapScreenshot PRIVATE Source)
    kitnap_use_ai(KitnapScreenshot "$<TARGET_FILE_DIR:KitnapScreenshot>/Resources" WITH_MODELS)
    target_compile_definitions(KitnapScreenshot PRIVATE ${KITNAP_DEFINITIONS} JucePlugin_Name="KITNAP"
        JUCE_STANDALONE_APPLICATION=1 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(KitnapScreenshot PRIVATE KitnapAssets juce::juce_audio_utils juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags)
endif()
