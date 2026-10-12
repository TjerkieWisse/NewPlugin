# Downloads ONNX Runtime and the drum separation model, and copies both into the built plugins.
#
# The plugin loads ONNX Runtime at run time (it is never linked), so a plugin without these files
# still works: it just skips AI separation.

set(KITNAP_ORT_VERSION 1.20.1)
set(KITNAP_ORT_BASE_URL "https://github.com/microsoft/onnxruntime/releases/download/v${KITNAP_ORT_VERSION}")

if(APPLE)
    set(_ort_package "onnxruntime-osx-universal2-${KITNAP_ORT_VERSION}")
    set(_ort_archive "${_ort_package}.tgz")
    set(_ort_hash "da4349e01a7e997f5034563183c7183d069caadc1d95f499b560961787813efd")
    set(_ort_library "lib/libonnxruntime.${KITNAP_ORT_VERSION}.dylib")
elseif(WIN32)
    set(_ort_package "onnxruntime-win-x64-${KITNAP_ORT_VERSION}")
    set(_ort_archive "${_ort_package}.zip")
    set(_ort_hash "78d447051e48bd2e1e778bba378bec4ece11191c9e538cf7b2c4a4565e8f5581")
    set(_ort_library "lib/onnxruntime.dll")
else()
    set(_ort_package "onnxruntime-linux-x64-${KITNAP_ORT_VERSION}")
    set(_ort_archive "${_ort_package}.tgz")
    set(_ort_hash "67db4dc1561f1e3fd42e619575c82c601ef89849afc7ea85a003abbac1a1a105")
    set(_ort_library "lib/libonnxruntime.so.${KITNAP_ORT_VERSION}")
endif()

# MDX-Net drums model (KUIELab, Music Demixing Challenge 2021), as distributed by the UVR project.
set(KITNAP_MODEL_URL "https://github.com/TRvlvr/model_repo/releases/download/all_public_uvr_models/kuielab_a_drums.onnx")
set(KITNAP_MODEL_HASH "40f586b7091934dd6f5563f0cba8f14bad57ce88440da1098bf388ea716c2901")

set(_ai_dir "${CMAKE_BINARY_DIR}/kitnap-ai")
set(_ort_dir "${_ai_dir}/${_ort_package}")
set(KITNAP_MODEL_FILE "${_ai_dir}/kitnap_drums.onnx")

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
if(NOT _kitnap_download_failed)
    _kitnap_download("${KITNAP_MODEL_URL}" "${KITNAP_MODEL_FILE}" "${KITNAP_MODEL_HASH}")
endif()

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

# Drum split model: DrumSep (MDX23C) by aufr33 & jarredou, splitting the drum stem into kick, snare,
# toms, hi-hat, ride and crash. The checkpoint is converted to ONNX at configure time, which needs
# Python with torch, onnx, onnxruntime, pyyaml and numpy; or pass an already converted model with
# -DKITNAP_DRUMSEP_ONNX=<file>. At 440 MB it isn't copied into every plugin: the installers put it
# in a shared folder once (see DrumSeparator.h).
set(KITNAP_DRUMSEP_ONNX "" CACHE FILEPATH "Already converted drum split model (kitnap_drumsep.onnx)")
set(KITNAP_DRUMSEP_URL "https://github.com/openmirlab/mdxnet-infer/releases/download/weights-drumsep-v1/aufr33-jarredou_DrumSep_model_mdx23c_ep_141_sdr_10.8059")
set(KITNAP_DRUMSEP_CKPT_HASH "d2a4aa53eb584d21eead358a4e66d1882ad182911be018f052b5da73be9096d0")
set(KITNAP_DRUMSEP_YAML_HASH "17d1649a227f841165bdb4c11a42082898192a1ea3ceab7e7e0b9293d6589dd6")
set(KITNAP_SPLIT_MODEL_FILE "${_ai_dir}/kitnap_drumsep.onnx")

if(KITNAP_WITH_DRUM_SPLIT)
    if(KITNAP_DRUMSEP_ONNX)
        configure_file("${KITNAP_DRUMSEP_ONNX}" "${KITNAP_SPLIT_MODEL_FILE}" COPYONLY)
    elseif(NOT EXISTS "${KITNAP_SPLIT_MODEL_FILE}")
        _kitnap_download("${KITNAP_DRUMSEP_URL}.ckpt" "${_ai_dir}/drumsep.ckpt" "${KITNAP_DRUMSEP_CKPT_HASH}")
        if(NOT _kitnap_download_failed)
            _kitnap_download("${KITNAP_DRUMSEP_URL}.yaml" "${_ai_dir}/drumsep.yaml" "${KITNAP_DRUMSEP_YAML_HASH}")
        endif()
        find_package(Python3 COMPONENTS Interpreter)
        if(NOT _kitnap_download_failed AND Python3_FOUND)
            message(STATUS "KITNAP: converting the drum split model to ONNX")
            execute_process(
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/export_drumsep.py"
                        "${_ai_dir}/drumsep.ckpt" "${_ai_dir}/drumsep.yaml" "${KITNAP_SPLIT_MODEL_FILE}.part"
                RESULT_VARIABLE _export_result
                OUTPUT_VARIABLE _export_output
                ERROR_VARIABLE _export_output)
            if(_export_result EQUAL 0)
                file(RENAME "${KITNAP_SPLIT_MODEL_FILE}.part" "${KITNAP_SPLIT_MODEL_FILE}")
            else()
                file(REMOVE "${KITNAP_SPLIT_MODEL_FILE}.part")
                message(WARNING "KITNAP: converting the drum split model failed (pip install torch onnx onnxruntime pyyaml numpy):\n${_export_output}")
            endif()
        elseif(NOT Python3_FOUND)
            message(WARNING "KITNAP: Python not found, building without the drum split model")
        endif()
    endif()
endif()
set(KITNAP_SPLIT_AVAILABLE FALSE)
if(EXISTS "${KITNAP_SPLIT_MODEL_FILE}")
    set(KITNAP_SPLIT_AVAILABLE TRUE)
endif()

# Copies the runtime and model into `destination` (a generator expression is fine) after `target` builds.
# With `with_split`, the drum split model is copied too.
function(kitnap_bundle_ai target destination)
    cmake_parse_arguments(_bundle "WITH_SPLIT" "" "" ${ARGN})
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${destination}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KITNAP_ORT_LIBRARY}" "${destination}/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KITNAP_MODEL_FILE}" "${destination}/"
        VERBATIM)
    if(_bundle_WITH_SPLIT AND KITNAP_SPLIT_AVAILABLE)
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
