cmake_minimum_required(VERSION 3.22)

project(KITNAP VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")

if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary")
endif()

option(KITNAP_BUILD_TESTS "Build the analyzer test" ON)
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

set(KITNAP_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND KITNAP_FORMATS AU)
endif()
if(UNIX AND NOT APPLE)
    list(APPEND KITNAP_FORMATS LV2)
endif()

juce_add_plugin(KITNAP
    COMPANY_NAME "Kitnap Audio"
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
    Source/engine/KitSampler.cpp
    Source/ui/WaveformView.cpp
    Source/ui/PadComponent.cpp)

target_include_directories(KITNAP PRIVATE Source)

set(KITNAP_DEFINITIONS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_WINDOWS_MEDIA_FORMAT=1
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_compile_definitions(KITNAP PUBLIC ${KITNAP_DEFINITIONS})

target_link_libraries(KITNAP
    PRIVATE
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
        Source/analysis/DrumAnalyzer.cpp)
    target_include_directories(KitnapAnalyzerTest PRIVATE Source)
    target_compile_definitions(KitnapAnalyzerTest PRIVATE ${KITNAP_DEFINITIONS})
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
        Source/engine/KitSampler.cpp
        Source/ui/WaveformView.cpp
        Source/ui/PadComponent.cpp)
    target_include_directories(KitnapScreenshot PRIVATE Source)
    target_compile_definitions(KitnapScreenshot PRIVATE ${KITNAP_DEFINITIONS} JucePlugin_Name="KITNAP"
        JUCE_STANDALONE_APPLICATION=1 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(KitnapScreenshot PRIVATE juce::juce_audio_utils juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags)
endif()
