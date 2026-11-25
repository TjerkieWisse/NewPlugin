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
option(KITNAP_WITH_DRUM_SPLIT "Download the AI drum split model" ON)
option(KITNAP_BUILD_SCREENSHOT "Build the UI screenshot tool (Linux, needs a display)" OFF)

# Licence keys, sold through Polar (polar.sh). Without an organization ID there is no licence check:
# anyone can use the build. Set them here, or with -D when configuring.
#   KITNAP_POLAR_ORGANIZATION_ID  the store's organization ID (Polar: Settings, General)
#   KITNAP_POLAR_BENEFIT_ID       optional: the KITNAP licence key benefit, so other products' keys don't count
#   KITNAP_POLAR_API              https://api.polar.sh, or https://sandbox-api.polar.sh to try with test keys
#   KITNAP_BUY_URL                where "Buy KITNAP" goes (the website, or a Polar checkout link)
#   KITNAP_ACCOUNT_URL            the buyers' Polar customer portal, where their key and computers are
foreach(_setting IN ITEMS
        "KITNAP_POLAR_ORGANIZATION_ID="
        "KITNAP_POLAR_BENEFIT_ID="
        "KITNAP_POLAR_API=https://api.polar.sh"
        "KITNAP_BUY_URL="
        "KITNAP_ACCOUNT_URL=")
    string(FIND "${_setting}" "=" _eq)
    string(SUBSTRING "${_setting}" 0 ${_eq} _name)
    math(EXPR _eq "${_eq} + 1")
    string(SUBSTRING "${_setting}" ${_eq} -1 _value)
    if(NOT DEFINED ${_name})
        set(${_name} "${_value}")
    endif()
endforeach()
string(REPEAT "[0-9a-fA-F]" 4 _hex4) # (CMake's regular expressions can't count)
set(_uuid "^${_hex4}${_hex4}-${_hex4}-${_hex4}-${_hex4}-${_hex4}${_hex4}${_hex4}$")
foreach(_id IN ITEMS KITNAP_POLAR_ORGANIZATION_ID KITNAP_POLAR_BENEFIT_ID)
    if(${_id} AND NOT ${_id} MATCHES "${_uuid}")
        message(FATAL_ERROR "${_id} should look like 1dbfc517-0bbf-4301-9ba8-555ca42b9737, not \"${${_id}}\"")
    endif()
endforeach()
configure_file(Source/licence/LicenceConfig.h.in "${CMAKE_BINARY_DIR}/generated/LicenceConfig.h" @ONLY)
if(KITNAP_POLAR_ORGANIZATION_ID)
    message(STATUS "KITNAP: licence keys from Polar organization ${KITNAP_POLAR_ORGANIZATION_ID} (${KITNAP_POLAR_API})")
else()
    message(STATUS "KITNAP: no licence check (KITNAP_POLAR_ORGANIZATION_ID is not set)")
endif()

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
    Source/ui/CreditsPanel.cpp
    Source/ui/LicencePanel.cpp
    Source/licence/Licence.cpp)

target_include_directories(KITNAP PRIVATE Source "${CMAKE_BINARY_DIR}/generated")

set(KITNAP_DEFINITIONS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_WINDOWS_MEDIA_FORMAT=1
    JUCE_VST3_CAN_REPLACE_VST2=0)

# Checking licence keys needs HTTPS: the operating system's on Mac and Windows, libcurl on Linux
# (loaded when needed, so it isn't a hard dependency).
if(UNIX AND NOT APPLE AND KITNAP_POLAR_ORGANIZATION_ID)
    find_package(CURL)
    if(NOT CURL_FOUND)
        message(FATAL_ERROR "Licence checks on Linux need the libcurl headers: sudo apt install libcurl4-openssl-dev")
    endif()
    list(REMOVE_ITEM KITNAP_DEFINITIONS JUCE_USE_CURL=0)
    list(APPEND KITNAP_DEFINITIONS JUCE_USE_CURL=1 JUCE_LOAD_CURL_SYMBOLS_LAZILY=1)
    # Every target here builds JUCE's network code.
    target_include_directories(KITNAP PRIVATE ${CURL_INCLUDE_DIRS})
    include_directories(${CURL_INCLUDE_DIRS})
endif()

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

# Fonts, the FortyVolts logo and the credits, compiled into the binary (BinaryData.h).
juce_add_binary_data(KitnapAssets SOURCES
    Resources/Credits.txt
    Resources/fonts/Michroma-Regular.ttf
    Resources/fonts/Geist-Light.ttf
    Resources/fonts/Geist-Medium.ttf
    Resources/fonts/GeistMono-Regular.ttf
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
        juce::juce_cryptography
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

if(KITNAP_BUILD_TESTS)
    # The licence logic against a pretend Polar (and, with --live, against the real one).
    juce_add_console_app(KitnapLicenceTest PRODUCT_NAME "KitnapLicenceTest")
    target_sources(KitnapLicenceTest PRIVATE tests/LicenceTest.cpp Source/licence/Licence.cpp)
    target_include_directories(KitnapLicenceTest PRIVATE Source "${CMAKE_BINARY_DIR}/generated")
    target_compile_definitions(KitnapLicenceTest PRIVATE ${KITNAP_DEFINITIONS})
    target_link_libraries(KitnapLicenceTest
        PRIVATE
            juce::juce_core
            juce::juce_events
            juce::juce_cryptography
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
        Source/ui/CreditsPanel.cpp
        Source/ui/LicencePanel.cpp
        Source/licence/Licence.cpp)
    target_include_directories(KitnapScreenshot PRIVATE Source "${CMAKE_BINARY_DIR}/generated")
    kitnap_use_ai(KitnapScreenshot "$<TARGET_FILE_DIR:KitnapScreenshot>/Resources" WITH_MODELS)
    target_compile_definitions(KitnapScreenshot PRIVATE ${KITNAP_DEFINITIONS} JucePlugin_Name="KITNAP"
        JUCE_STANDALONE_APPLICATION=1 JUCE_MODAL_LOOPS_PERMITTED=1 KITNAP_LICENCE_PREVIEW=1)
    target_link_libraries(KitnapScreenshot PRIVATE KitnapAssets juce::juce_audio_utils juce::juce_cryptography juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags)
endif()
