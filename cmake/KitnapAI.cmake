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

# The same build of ONNX Runtime from Microsoft's NuGet package, for when the GitHub release can't be
# downloaded (its files have gone missing before): one package for every system, laid out here like
# the release archive (on a Mac the Apple Silicon and Intel libraries are joined into one).
set(KITNAP_ORT_NUGET_URL "https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime/${KITNAP_ORT_VERSION}")
set(KITNAP_ORT_NUGET_HASH "25fe172f7fcdf34f2b5b02c8b997f6b88abc282956849d4af4335af23d0c4e4a")

function(_kitnap_ort_from_nuget)
    set(_nupkg "${_ai_dir}/onnxruntime-${KITNAP_ORT_VERSION}.nupkg")
    set(_kitnap_download_failed FALSE)
    _kitnap_download("${KITNAP_ORT_NUGET_URL}" "${_nupkg}" "${KITNAP_ORT_NUGET_HASH}")
    if(_kitnap_download_failed)
        set(_kitnap_download_failed TRUE PARENT_SCOPE)
        return()
    endif()
    set(_nuget "${_ai_dir}/onnxruntime-nuget")
    file(REMOVE_RECURSE "${_nuget}")
    file(ARCHIVE_EXTRACT INPUT "${_nupkg}" DESTINATION "${_nuget}")
    file(COPY "${_nuget}/build/native/include/" DESTINATION "${_ort_dir}/include")
    file(COPY "${_nuget}/LICENSE" "${_nuget}/ThirdPartyNotices.txt" DESTINATION "${_ort_dir}")
    get_filename_component(_lib_dir "${_ort_dir}/${_ort_library}" DIRECTORY)
    file(MAKE_DIRECTORY "${_lib_dir}")
    if(APPLE)
        execute_process(COMMAND lipo -create "${_nuget}/runtimes/osx-arm64/native/libonnxruntime.dylib"
                                "${_nuget}/runtimes/osx-x64/native/libonnxruntime.dylib"
                                -output "${_ort_dir}/${_ort_library}"
                        RESULT_VARIABLE _result)
        if(NOT _result EQUAL 0)
            message(WARNING "KITNAP: joining ONNX Runtime's Mac libraries failed")
            set(_kitnap_download_failed TRUE PARENT_SCOPE)
        endif()
    elseif(WIN32)
        file(COPY_FILE "${_nuget}/runtimes/win-x64/native/onnxruntime.dll" "${_ort_dir}/${_ort_library}")
    else()
        file(COPY_FILE "${_nuget}/runtimes/linux-x64/native/libonnxruntime.so" "${_ort_dir}/${_ort_library}")
    endif()
endfunction()

set(_kitnap_download_failed FALSE)
if(NOT EXISTS "${_ort_dir}/include/onnxruntime_c_api.h" OR NOT EXISTS "${_ort_dir}/${_ort_library}")
    _kitnap_download("${KITNAP_ORT_BASE_URL}/${_ort_archive}" "${_ai_dir}/${_ort_archive}" "${_ort_hash}")
    if(NOT _kitnap_download_failed)
        file(ARCHIVE_EXTRACT INPUT "${_ai_dir}/${_ort_archive}" DESTINATION "${_ai_dir}")
    else()
        message(STATUS "KITNAP: getting ONNX Runtime ${KITNAP_ORT_VERSION} from NuGet instead")
        set(_kitnap_download_failed FALSE)
        _kitnap_ort_from_nuget()
    endif()
endif()

if(_kitnap_download_failed OR NOT EXISTS "${_ort_dir}/${_ort_library}")
    set(KITNAP_AI_AVAILABLE FALSE)
    return()
endif()

set(KITNAP_AI_AVAILABLE TRUE)
set(KITNAP_ORT_INCLUDE_DIR "${_ort_dir}/include")
set(KITNAP_ORT_LIBRARY "${_ort_dir}/${_ort_library}")

# The notices the installers put next to the models and the runtime: KITNAP's credits and licences
# (also shown in the plugin) and ONNX Runtime's own third-party notices.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Resources/Credits.txt" "${_ai_dir}/KITNAP-Credits.txt" COPYONLY)
if(EXISTS "${_ort_dir}/ThirdPartyNotices.txt")
    configure_file("${_ort_dir}/ThirdPartyNotices.txt" "${_ai_dir}/ONNXRuntime-ThirdPartyNotices.txt" COPYONLY)
endif()

# The separation model is converted to ONNX at configure time, which needs Python with torch, onnx,
# onnxruntime, numpy and BS-RoFormer==0.4.1; the drum split model is downloaded as ONNX. Already
# converted models can be passed instead with -DKITNAP_SEPARATION_ONNX=<file> and
# -DKITNAP_DRUMSEP_ONNX=<file>. They are hundreds of MB, so they aren't copied into every plugin: the
# installers put them in a shared folder once (see DrumSeparator.h).
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
        message(WARNING "KITNAP: converting ${_name} failed (pip install torch onnx onnxruntime numpy BS-RoFormer==0.4.1):\n${_output}")
    endif()
endfunction()

# Drum separation: ZFTurbo's 4-stem BS-RoFormer (bass, drums, vocals, other; MIT, from his
# Music-Source-Separation-Training releases), of which only the drums are exported.
set(KITNAP_SEPARATION_ONNX "" CACHE FILEPATH "Already converted drum separation model (kitnap_drums_4stem.onnx)")
set(KITNAP_SEPARATION_URL "https://github.com/ZFTurbo/Music-Source-Separation-Training/releases/download/v1.0.12/model_bs_roformer_ep_17_sdr_9.6568.ckpt")
set(KITNAP_SEPARATION_HASH "3e9daecd70aaed5b5a0d1f861cc4d77eaa45afb3fc6301b1cf32c1be0f5868fb")
set(KITNAP_MODEL_FILE "${_ai_dir}/kitnap_drums_4stem.onnx")

if(KITNAP_SEPARATION_ONNX)
    configure_file("${KITNAP_SEPARATION_ONNX}" "${KITNAP_MODEL_FILE}" COPYONLY)
elseif(NOT EXISTS "${KITNAP_MODEL_FILE}")
    set(_kitnap_download_failed FALSE)
    _kitnap_download("${KITNAP_SEPARATION_URL}" "${_ai_dir}/bs_roformer_4stem.ckpt" "${KITNAP_SEPARATION_HASH}")
    if(NOT _kitnap_download_failed)
        _kitnap_convert("${KITNAP_MODEL_FILE}" export_drums.py "${_ai_dir}/bs_roformer_4stem.ckpt")
    endif()
endif()
set(KITNAP_SEPARATION_AVAILABLE FALSE)
if(EXISTS "${KITNAP_MODEL_FILE}")
    set(KITNAP_SEPARATION_AVAILABLE TRUE)
endif()

# Drum split model: KITNAP's own (MDX23C, trained with training/drumsplit on material we may use
# commercially), splitting the drum stem into kick, snare, toms, hi-hat and cymbals. It's already ONNX,
# so it's only downloaded, from this repository's releases.
set(KITNAP_DRUMSEP_ONNX "" CACHE FILEPATH "Already converted drum split model (kitnap_drumsep5.onnx)")
# v2: the xlarge run at update 37926 (v1, update 74000, was trained further with music under the drums).
set(KITNAP_SPLIT_URL "https://github.com/TjerkieWisse/NewPlugin/releases/download/drumsplit-v2/kitnap_drumsep5.onnx")
set(KITNAP_SPLIT_HASH "c16f3c9a804106799c80b5b1f65379c5806bf39cddd55598dae1dffaee53ff63")
set(KITNAP_SPLIT_MODEL_FILE "${_ai_dir}/kitnap_drumsep5.onnx")

if(KITNAP_WITH_DRUM_SPLIT)
    if(KITNAP_DRUMSEP_ONNX)
        configure_file("${KITNAP_DRUMSEP_ONNX}" "${KITNAP_SPLIT_MODEL_FILE}" COPYONLY)
    else()
        # A model from an earlier release (or jarredou's DrumSep, which older builds used) is replaced.
        if(EXISTS "${KITNAP_SPLIT_MODEL_FILE}")
            file(SHA256 "${KITNAP_SPLIT_MODEL_FILE}" _split_hash)
            if(NOT _split_hash STREQUAL KITNAP_SPLIT_HASH)
                file(REMOVE "${KITNAP_SPLIT_MODEL_FILE}")
            endif()
        endif()
        if(NOT EXISTS "${KITNAP_SPLIT_MODEL_FILE}")
            set(_kitnap_download_failed FALSE)
            _kitnap_download("${KITNAP_SPLIT_URL}" "${KITNAP_SPLIT_MODEL_FILE}" "${KITNAP_SPLIT_HASH}")
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
