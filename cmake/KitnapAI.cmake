# Downloads ONNX Runtime and the AI models, converts the models to ONNX, and copies the runtime into
# the built plugins.
#
# The plugin loads ONNX Runtime at run time (it is never linked), so a plugin without these files
# still works: it just skips AI separation.

# 1.23 is the first version whose Core ML support covers every operation in both models.
set(KITNAP_ORT_VERSION 1.23.2)
set(KITNAP_ORT_BASE_URL "https://github.com/microsoft/onnxruntime/releases/download/v${KITNAP_ORT_VERSION}")

if(APPLE)
    set(_ort_package "onnxruntime-osx-universal2-${KITNAP_ORT_VERSION}")
    set(_ort_archive "${_ort_package}.tgz")
    set(_ort_hash "49ae8e3a66ccb18d98ad3fe7f5906b6d7887df8a5edd40f49eb2b14e20885809")
    set(_ort_library "lib/libonnxruntime.${KITNAP_ORT_VERSION}.dylib")
elseif(WIN32)
    set(_ort_package "onnxruntime-win-x64-${KITNAP_ORT_VERSION}")
    set(_ort_archive "${_ort_package}.zip")
    set(_ort_hash "0b38df9af21834e41e73d602d90db5cb06dbd1ca618948b8f1d66d607ac9f3cd")
    set(_ort_library "lib/onnxruntime.dll")
else()
    set(_ort_package "onnxruntime-linux-x64-${KITNAP_ORT_VERSION}")
    set(_ort_archive "${_ort_package}.tgz")
    set(_ort_hash "1fa4dcaef22f6f7d5cd81b28c2800414350c10116f5fdd46a2160082551c5f9b")
    set(_ort_library "lib/libonnxruntime.so.${KITNAP_ORT_VERSION}")
endif()

set(_ai_dir "${CMAKE_BINARY_DIR}/kitnap-ai")
set(_ort_dir "${_ai_dir}/${_ort_package}")

function(_kitnap_download url file hash)
    if(EXISTS "${file}")
        file(SHA256 "${file}" _existing)
        if(_existing STREQUAL hash)
            return()
        endif()
    endif()
    # Large GitHub release downloads now and then fail on CI runners, so try a few times. The hash is
    # checked here rather than by file(DOWNLOAD), which turns a failed download into a hard error.
    foreach(_attempt RANGE 1 3)
        message(STATUS "KITNAP: downloading ${url}")
        file(DOWNLOAD "${url}" "${file}.part" STATUS _status TLS_VERIFY ON)
        list(GET _status 0 _code)
        if(_code EQUAL 0)
            file(SHA256 "${file}.part" _got)
            if(_got STREQUAL hash)
                file(RENAME "${file}.part" "${file}")
                return()
            endif()
            set(_status "checksum mismatch")
        endif()
        file(REMOVE "${file}.part")
        message(STATUS "KITNAP: download attempt ${_attempt} failed (${_status})")
        execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 5)
    endforeach()
    message(WARNING "KITNAP: downloading ${url} failed (${_status})")
    set(_kitnap_download_failed TRUE PARENT_SCOPE)
endfunction()

set(_kitnap_download_failed FALSE)
_kitnap_download("${KITNAP_ORT_BASE_URL}/${_ort_archive}" "${_ai_dir}/${_ort_archive}" "${_ort_hash}")

if(_kitnap_download_failed)
    set(KITNAP_AI_AVAILABLE FALSE)
    return()
endif()

if(NOT EXISTS "${_ort_dir}/include/onnxruntime_c_api.h")
    file(ARCHIVE_EXTRACT INPUT "${_ai_dir}/${_ort_archive}" DESTINATION "${_ai_dir}")
endif()

set(KITNAP_AI_AVAILABLE TRUE)
set(KITNAP_ORT_INCLUDE_DIR "${_ort_dir}/include")
set(KITNAP_ORT_LIBRARY "${_ort_dir}/${_ort_library}")

# The models are converted to ONNX at configure time, which needs Python with torch, onnx,
# onnxruntime, numpy, pyyaml, safetensors and BS-RoFormer==0.4.1. Already converted models can be
# passed instead with -DKITNAP_SEPARATION_ONNX=<file> and -DKITNAP_DRUMSEP_ONNX=<file>. They are
# hundreds of MB, so they aren't copied into every plugin: the installers put them in a shared
# folder once (see DrumSeparator.h).
find_package(Python3 COMPONENTS Interpreter)

# _kitnap_convert(<output> <script> <inputs...>): runs tools/<script> <inputs...> <output>.
function(_kitnap_convert output script)
    if(NOT Python3_FOUND)
        message(WARNING "KITNAP: Python not found, so ${output} can't be converted")
        return()
    endif()
    get_filename_component(_name "${output}" NAME)
    message(STATUS "KITNAP: converting ${_name} to ONNX")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/${script}" ${ARGN} "${output}.part"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _output)
    if(_result EQUAL 0)
        file(RENAME "${output}.part" "${output}")
    else()
        file(REMOVE "${output}.part")
        message(WARNING "KITNAP: converting ${_name} failed (pip install torch onnx onnxruntime numpy pyyaml safetensors BS-RoFormer==0.4.1):\n${_output}")
    endif()
endfunction()

# Drum separation: BS-RoFormer SW (6 stems: bass, drums, other, vocals, guitar, piano; weights
# released by jarredou), of which only the drums are exported.
set(KITNAP_SEPARATION_ONNX "" CACHE FILEPATH "Already converted drum separation model (kitnap_drums_sw.onnx)")
set(KITNAP_SW_URL "https://github.com/mogonzal/mixdown-models/releases/download/bs-roformer-sw-v1/bs_roformer_sw.safetensors")
set(KITNAP_SW_HASH "56c131428745d83d63f72293d237fdbb56f40d8e4b6980cbcde70e28a9f7330f")
set(KITNAP_MODEL_FILE "${_ai_dir}/kitnap_drums_sw.onnx")

if(KITNAP_SEPARATION_ONNX)
    configure_file("${KITNAP_SEPARATION_ONNX}" "${KITNAP_MODEL_FILE}" COPYONLY)
elseif(NOT EXISTS "${KITNAP_MODEL_FILE}")
    set(_kitnap_download_failed FALSE)
    _kitnap_download("${KITNAP_SW_URL}" "${_ai_dir}/bs_roformer_sw.safetensors" "${KITNAP_SW_HASH}")
    if(NOT _kitnap_download_failed)
        _kitnap_convert("${KITNAP_MODEL_FILE}" export_sw.py "${_ai_dir}/bs_roformer_sw.safetensors")
    endif()
endif()
set(KITNAP_SEPARATION_AVAILABLE FALSE)
if(EXISTS "${KITNAP_MODEL_FILE}")
    set(KITNAP_SEPARATION_AVAILABLE TRUE)
endif()

# Drum split model: DrumSep 5 stems v2025 (MDX23C) by jarredou, splitting the drum stem into kick,
# snare, toms, hi-hat and cymbals. Its original release is gone; this copy is from the DrumToMIDI
# project (the hashes pin that copy, there are no official ones).
set(KITNAP_DRUMSEP_ONNX "" CACHE FILEPATH "Already converted drum split model (kitnap_drumsep5.onnx)")
set(KITNAP_DRUMSEP_CKPT_URL "https://media.githubusercontent.com/media/EverlastEngineering/DrumToMIDI/main/mdx_models/drumsep_5stems_mdx23c_jarredou.ckpt")
set(KITNAP_DRUMSEP_YAML_URL "https://raw.githubusercontent.com/EverlastEngineering/DrumToMIDI/main/mdx_models/config_mdx23c.yaml")
set(KITNAP_DRUMSEP_CKPT_HASH "1f8e636fb674b88a52c8399fde9a4ebe2b72b065ca07eed4e03ab1c9f0bfb2e0")
set(KITNAP_DRUMSEP_YAML_HASH "d2962f43d6682e8ea84e77fb50a2680e4641ba1749ee95f8ea6e96bba3278944")
set(KITNAP_SPLIT_MODEL_FILE "${_ai_dir}/kitnap_drumsep5.onnx")

if(KITNAP_WITH_DRUM_SPLIT)
    if(KITNAP_DRUMSEP_ONNX)
        configure_file("${KITNAP_DRUMSEP_ONNX}" "${KITNAP_SPLIT_MODEL_FILE}" COPYONLY)
    elseif(NOT EXISTS "${KITNAP_SPLIT_MODEL_FILE}")
        set(_kitnap_download_failed FALSE)
        _kitnap_download("${KITNAP_DRUMSEP_CKPT_URL}" "${_ai_dir}/drumsep5.ckpt" "${KITNAP_DRUMSEP_CKPT_HASH}")
        if(NOT _kitnap_download_failed)
            _kitnap_download("${KITNAP_DRUMSEP_YAML_URL}" "${_ai_dir}/drumsep5.yaml" "${KITNAP_DRUMSEP_YAML_HASH}")
        endif()
        if(NOT _kitnap_download_failed)
            _kitnap_convert("${KITNAP_SPLIT_MODEL_FILE}" export_drumsep.py "${_ai_dir}/drumsep5.ckpt" "${_ai_dir}/drumsep5.yaml")
        endif()
    endif()
endif()
set(KITNAP_SPLIT_AVAILABLE FALSE)
if(EXISTS "${KITNAP_SPLIT_MODEL_FILE}")
    set(KITNAP_SPLIT_AVAILABLE TRUE)
endif()

# Copies the runtime into `destination` (a generator expression is fine) after `target` builds.
# With WITH_MODELS, the models are copied too (for the tests; plugins find them in the shared folder).
function(kitnap_bundle_ai target destination)
    cmake_parse_arguments(_bundle "WITH_MODELS" "" "" ${ARGN})
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${destination}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KITNAP_ORT_LIBRARY}" "${destination}/"
        VERBATIM)
    if(_bundle_WITH_MODELS AND KITNAP_SEPARATION_AVAILABLE)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KITNAP_MODEL_FILE}" "${destination}/"
            VERBATIM)
    endif()
    if(_bundle_WITH_MODELS AND KITNAP_SPLIT_AVAILABLE)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KITNAP_SPLIT_MODEL_FILE}" "${destination}/"
            VERBATIM)
    endif()
    if(APPLE)
        # Apple Silicon only loads signed code; give the bundled runtime an ad-hoc signature.
        get_filename_component(_lib_name "${KITNAP_ORT_LIBRARY}" NAME)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND codesign --force --sign - "${destination}/${_lib_name}"
            VERBATIM)
    endif()
endfunction()
