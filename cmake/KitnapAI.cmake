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
    message(STATUS "KITNAP: downloading ${url}")
    file(DOWNLOAD "${url}" "${file}.part" EXPECTED_HASH SHA256=${hash} STATUS _status TLS_VERIFY ON)
    list(GET _status 0 _code)
    if(NOT _code EQUAL 0)
        file(REMOVE "${file}.part")
        message(WARNING "KITNAP: download failed (${_status}), building without AI separation")
        set(_kitnap_download_failed TRUE PARENT_SCOPE)
        return()
    endif()
    file(RENAME "${file}.part" "${file}")
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

# Copies the runtime and model into `destination` (a generator expression is fine) after `target` builds.
function(kitnap_bundle_ai target destination)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${destination}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KITNAP_ORT_LIBRARY}" "${destination}/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KITNAP_MODEL_FILE}" "${destination}/"
        VERBATIM)
    if(APPLE)
        # Apple Silicon only loads signed code; give the bundled runtime an ad-hoc signature.
        get_filename_component(_lib_name "${KITNAP_ORT_LIBRARY}" NAME)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND codesign --force --sign - "${destination}/${_lib_name}"
            VERBATIM)
    endif()
endfunction()
