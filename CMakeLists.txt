cmake_minimum_required(VERSION 3.22)

project(ORBIT VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")

if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary")
endif()

# JUCE is downloaded automatically. Point ORBIT_JUCE_PATH at a local checkout to skip the download.
set(ORBIT_JUCE_PATH "" CACHE PATH "Optional local JUCE checkout")

if(ORBIT_JUCE_PATH)
    add_subdirectory(${ORBIT_JUCE_PATH} JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

set(ORBIT_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND ORBIT_FORMATS AU)
endif()
if(UNIX AND NOT APPLE)
    list(APPEND ORBIT_FORMATS LV2)
endif()

juce_add_plugin(ORBIT
    COMPANY_NAME "Orbit Audio"
    BUNDLE_ID "com.orbitaudio.orbit"
    PLUGIN_MANUFACTURER_CODE Orba
    PLUGIN_CODE Orb1
    FORMATS ${ORBIT_FORMATS}
    PRODUCT_NAME "ORBIT"
    LV2URI "https://github.com/tjerkiewisse/newplugin"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Delay Reverb
    AU_MAIN_TYPE kAudioUnitType_Effect)

target_sources(ORBIT PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/ui/OrbPad.cpp)

target_include_directories(ORBIT PRIVATE Source)

target_compile_definitions(ORBIT PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(ORBIT
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
