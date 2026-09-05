cmake_minimum_required(VERSION 3.24)
project(DLSSVideoPlayer VERSION 0.12.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "DLSSVideoPlayer currently targets Windows x64 / D3D12 only.")
endif()
if(NOT MSVC)
    message(FATAL_ERROR "Use Visual Studio 2022 / MSVC for this build.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(DLSS_SDK "${CMAKE_SOURCE_DIR}/external/DLSS" CACHE PATH "Path to NVIDIA/DLSS SDK checkout")
if(NOT EXISTS "${DLSS_SDK}/include/nvsdk_ngx.h")
    message(FATAL_ERROR "NVIDIA DLSS SDK not found at ${DLSS_SDK}. Run build_windows.bat; it clones the official NVIDIA/DLSS repository automatically.")
endif()

set(NVOF_SDK "${CMAKE_SOURCE_DIR}/external/NvOFSDK" CACHE PATH "Staged NVIDIA Optical Flow SDK 5.0 subset")
if(NOT EXISTS "${NVOF_SDK}/NvOFInterface/nvOpticalFlowD3D12.h" OR
   NOT EXISTS "${NVOF_SDK}/Common/NvOFBase/NvOFD3D12.cpp")
    message(FATAL_ERROR "NVIDIA Optical Flow SDK subset not found at ${NVOF_SDK}. Run the Step 02 apply script first.")
endif()

set(DLSS_LIB_DIR "${DLSS_SDK}/lib/Windows_x86_64/x64")
set(DLSS_RUNTIME_REL "${DLSS_SDK}/lib/Windows_x86_64/rel/nvngx_dlss.dll")
set(DLSS_RUNTIME_DEV "${DLSS_SDK}/lib/Windows_x86_64/dev/nvngx_dlss.dll")

add_library(dmp_nvof_sdk STATIC
    "${NVOF_SDK}/Common/NvOFBase/NvOF.cpp"
    "${NVOF_SDK}/Common/NvOFBase/NvOFD3D12.cpp"
    "${NVOF_SDK}/Common/NvOFBase/NvOFD3DCommon.cpp"
)
target_compile_definitions(dmp_nvof_sdk PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)
target_compile_options(dmp_nvof_sdk PRIVATE /W3 /EHsc)
target_include_directories(dmp_nvof_sdk PUBLIC
    "${NVOF_SDK}/NvOFInterface"
    "${NVOF_SDK}/Common/NvOFBase"
)
target_link_libraries(dmp_nvof_sdk PUBLIC d3d12 dxgi)

add_executable(DLSSVideoPlayer WIN32
    src/main.cpp
    src/VideoDecoder.cpp
    src/VideoDecoder.h
    src/DLSSBackend.cpp
    src/DLSSBackend.h
    src/D3D12Renderer.cpp
    src/D3D12Renderer.h
    src/TemporalGuides.cpp
    src/TemporalGuides.h
    src/OpticalFlowEngine.cpp
    src/OpticalFlowEngine.h
    src/AudioPlayer.cpp
    src/AudioPlayer.h
    src/Log.h
    src/Localization.h
)

target_compile_definitions(DLSSVideoPlayer PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)
target_compile_options(DLSSVideoPlayer PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
target_include_directories(DLSSVideoPlayer PRIVATE "${DLSS_SDK}/include")
target_link_directories(DLSSVideoPlayer PRIVATE "${DLSS_LIB_DIR}")
target_link_libraries(DLSSVideoPlayer PRIVATE
    d3d12 dxgi dxguid d3dcompiler
    mfplat mfreadwrite mfuuid
    ole32 shell32 shlwapi comdlg32 comctl32 user32 gdi32 winmm dwmapi
    nvsdk_ngx_d
    dmp_nvof_sdk
)

add_custom_command(TARGET DLSSVideoPlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<IF:$<CONFIG:Debug>,${DLSS_RUNTIME_DEV},${DLSS_RUNTIME_REL}>"
        "$<TARGET_FILE_DIR:DLSSVideoPlayer>/nvngx_dlss.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/README.md"
        "$<TARGET_FILE_DIR:DLSSVideoPlayer>/README.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/LICENSE"
        "$<TARGET_FILE_DIR:DLSSVideoPlayer>/LICENSE"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:DLSSVideoPlayer>/languages"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/languages"
        "$<TARGET_FILE_DIR:DLSSVideoPlayer>/languages"
)

# The one-click BAT stages these binaries automatically. Keep this optional so
# developers can still configure CMake before downloading FFmpeg.
set(FFMPEG_STAGED_DIR "${CMAKE_SOURCE_DIR}/external/ffmpeg/bin")
if(EXISTS "${FFMPEG_STAGED_DIR}/ffmpeg.exe" AND EXISTS "${FFMPEG_STAGED_DIR}/ffprobe.exe")
    add_custom_command(TARGET DLSSVideoPlayer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFMPEG_STAGED_DIR}/ffmpeg.exe"
            "$<TARGET_FILE_DIR:DLSSVideoPlayer>/ffmpeg.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFMPEG_STAGED_DIR}/ffprobe.exe"
            "$<TARGET_FILE_DIR:DLSSVideoPlayer>/ffprobe.exe"
    )
endif()
# === STEP04A1_DEPTH_PROBE_BEGIN ===
# Standalone AI-depth characterization target. It deliberately does not modify
# the DLSSVideoPlayer runtime path yet.
set(DMP_AI_WORKBENCH "C:/PROGETTO_DLSS/AI-Depth-Workbench" CACHE PATH "DMP AI depth workbench")
set(DMP_AI_RUNTIME "${DMP_AI_WORKBENCH}/runtime")
set(DMP_DEPTH_MODEL "${DMP_AI_WORKBENCH}/models/depth_anything_v2_small_fp16.onnx")
set(DMP_ORT_INCLUDE "${DMP_AI_RUNTIME}/include")
set(DMP_ORT_LIB "${DMP_AI_RUNTIME}/lib/onnxruntime.lib")
set(DMP_ORT_DLL "${DMP_AI_RUNTIME}/bin/onnxruntime.dll")
set(DMP_ORT_SHARED_DLL "${DMP_AI_RUNTIME}/bin/onnxruntime_providers_shared.dll")
set(DMP_DIRECTML_DLL "${DMP_AI_RUNTIME}/bin/DirectML.dll")

foreach(DMP_REQUIRED_AI_FILE
    "${DMP_ORT_INCLUDE}/onnxruntime_cxx_api.h"
    "${DMP_ORT_LIB}"
    "${DMP_ORT_DLL}"
    "${DMP_DIRECTML_DLL}"
    "${DMP_DEPTH_MODEL}"
)
    if(NOT EXISTS "${DMP_REQUIRED_AI_FILE}")
        message(FATAL_ERROR "Step 04A-1 AI dependency missing: ${DMP_REQUIRED_AI_FILE}. Run the Step 04A preflight and 01-stage-directml.ps1.")
    endif()
endforeach()

add_executable(DMPDepthProbe
    src/DepthProbe.cpp
    src/DepthEngine.cpp
    src/DepthEngine.h
    src/VideoDecoder.cpp
    src/VideoDecoder.h
    src/Log.h
)
target_compile_definitions(DMPDepthProbe PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)
target_compile_options(DMPDepthProbe PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
target_include_directories(DMPDepthProbe PRIVATE "${DMP_ORT_INCLUDE}")
target_link_libraries(DMPDepthProbe PRIVATE
    "${DMP_ORT_LIB}"
    mfplat mfreadwrite mfuuid
    ole32 shell32 shlwapi windowscodecs
)

add_custom_command(TARGET DMPDepthProbe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${DMP_ORT_DLL}"
        "$<TARGET_FILE_DIR:DMPDepthProbe>/onnxruntime.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${DMP_DIRECTML_DLL}"
        "$<TARGET_FILE_DIR:DMPDepthProbe>/DirectML.dll"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:DMPDepthProbe>/models"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${DMP_DEPTH_MODEL}"
        "$<TARGET_FILE_DIR:DMPDepthProbe>/models/depth_anything_v2_small_fp16.onnx"
)
if(EXISTS "${DMP_ORT_SHARED_DLL}")
    add_custom_command(TARGET DMPDepthProbe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DMP_ORT_SHARED_DLL}"
            "$<TARGET_FILE_DIR:DMPDepthProbe>/onnxruntime_providers_shared.dll"
    )
endif()
if(EXISTS "${FFMPEG_STAGED_DIR}/ffmpeg.exe" AND EXISTS "${FFMPEG_STAGED_DIR}/ffprobe.exe")
    add_custom_command(TARGET DMPDepthProbe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFMPEG_STAGED_DIR}/ffmpeg.exe"
            "$<TARGET_FILE_DIR:DMPDepthProbe>/ffmpeg.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFMPEG_STAGED_DIR}/ffprobe.exe"
            "$<TARGET_FILE_DIR:DMPDepthProbe>/ffprobe.exe"
    )
endif()
# === STEP04A1_DEPTH_PROBE_END ===
# === STEP04A2_TRTRTX_PLAYER_BEGIN ===
# v1.1: TensorRT-RTX/CUDA are isolated in DMPAIDepthWorker.exe. The player
# communicates through shared memory/events and never loads TensorRT/CUDA itself.
set(DMP_TRTRTX_ROOT "" CACHE PATH "TensorRT-RTX SDK root for player AI depth")
if(NOT DMP_TRTRTX_ROOT OR NOT EXISTS "${DMP_TRTRTX_ROOT}/include/NvInfer.h")
    message(FATAL_ERROR "DMP_TRTRTX_ROOT must point to the extracted TensorRT-RTX SDK")
endif()
if(NOT DMP_AI_WORKBENCH)
    set(DMP_AI_WORKBENCH "C:/PROGETTO_DLSS/AI-Depth-Workbench" CACHE PATH "DMP AI depth workbench")
endif()
find_package(CUDAToolkit REQUIRED)
file(READ "${DMP_TRTRTX_ROOT}/include/NvInferVersion.h" _dmp_trtver)
string(REGEX MATCH "TRT_MAJOR_RTX[ \t]+([0-9]+)" _dmp_m1 "${_dmp_trtver}")
set(DMP_TRT_MAJOR "${CMAKE_MATCH_1}")
string(REGEX MATCH "TRT_MINOR_RTX[ \t]+([0-9]+)" _dmp_m2 "${_dmp_trtver}")
set(DMP_TRT_MINOR "${CMAKE_MATCH_1}")
if(NOT DMP_TRT_MAJOR OR NOT DMP_TRT_MINOR)
    message(FATAL_ERROR "Could not parse TensorRT-RTX version header")
endif()
set(DMP_TRT_SOVERSION "${DMP_TRT_MAJOR}_${DMP_TRT_MINOR}")
set(DMP_TRTRTX_LIB "${DMP_TRTRTX_ROOT}/lib/tensorrt_rtx_${DMP_TRT_SOVERSION}.lib")
set(DMP_TRTRTX_DLL "${DMP_TRTRTX_ROOT}/bin/tensorrt_rtx_${DMP_TRT_SOVERSION}.dll")
set(DMP_TRTRTX_ENGINE "${DMP_AI_WORKBENCH}/tensorrt-rtx/engines/depth_anything_v2_small_fp16_dynamic_518.trt")
set(DMP_TRTRTX_CACHE "${DMP_AI_WORKBENCH}/tensorrt-rtx/cache/depth_anything_v2_small_fp16_dynamic_518.cache")
foreach(_req "${DMP_TRTRTX_LIB}" "${DMP_TRTRTX_DLL}" "${DMP_TRTRTX_ENGINE}")
    if(NOT EXISTS "${_req}")
        message(FATAL_ERROR "Step 04A-2 dependency missing: ${_req}")
    endif()
endforeach()

target_sources(DLSSVideoPlayer PRIVATE
    src/AIDepthWorker.cpp
    src/AIDepthWorker.h
    src/AIDepthIpc.h
    src/AIDepthTemporal.cpp
    src/AIDepthTemporal.h
    src/AIDepthTemporalWorker.cpp
    src/AIDepthTemporalWorker.h
    src/FilmMotionStabilizer.cpp
    src/FilmMotionStabilizer.h
)

add_executable(DMPAIDepthWorker
    src/AIDepthWorkerProcess.cpp
    src/AIDepthIpc.h
    src/TrtRtxDepthEngine.cpp
    src/TrtRtxDepthEngine.h
    src/Log.h
)
target_compile_definitions(DMPAIDepthWorker PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(DMPAIDepthWorker PRIVATE "${DMP_TRTRTX_ROOT}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(MSVC)
    target_compile_options(DMPAIDepthWorker PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus /wd4996)
endif()
target_link_libraries(DMPAIDepthWorker PRIVATE "${DMP_TRTRTX_LIB}" CUDA::cudart)
set_target_properties(DMPAIDepthWorker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ai_depth"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release/ai_depth")
add_dependencies(DLSSVideoPlayer DMPAIDepthWorker)

add_custom_command(TARGET DLSSVideoPlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:DLSSVideoPlayer>/models"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DMP_TRTRTX_ENGINE}" "$<TARGET_FILE_DIR:DLSSVideoPlayer>/models/depth_anything_v2_small_fp16_dynamic_518.trt"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:DLSSVideoPlayer>/cache")
if(EXISTS "${DMP_TRTRTX_CACHE}")
    add_custom_command(TARGET DLSSVideoPlayer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DMP_TRTRTX_CACHE}" "$<TARGET_FILE_DIR:DLSSVideoPlayer>/cache/depth_anything_v2_small_fp16_dynamic_518.cache")
endif()

file(GLOB DMP_TRTRTX_RUNTIME_DLLS "${DMP_TRTRTX_ROOT}/bin/*.dll")
foreach(_dll IN LISTS DMP_TRTRTX_RUNTIME_DLLS)
    add_custom_command(TARGET DMPAIDepthWorker POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dll}" "$<TARGET_FILE_DIR:DMPAIDepthWorker>")
endforeach()
file(GLOB DMP_CUDART_RUNTIME_DLLS "${CUDAToolkit_BIN_DIR}/cudart64_*.dll")
foreach(_dll IN LISTS DMP_CUDART_RUNTIME_DLLS)
    add_custom_command(TARGET DMPAIDepthWorker POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dll}" "$<TARGET_FILE_DIR:DMPAIDepthWorker>")
endforeach()
# === STEP04A2_TRTRTX_PLAYER_END ===

# Step 04E soft temporal mask
target_sources(DLSSVideoPlayer PRIVATE src/SoftTemporalMask.cpp)

# Step 04E-1 AI depth-guided temporal mask
target_sources(DLSSVideoPlayer PRIVATE src/MaskDepthGuide.cpp)

# Step 04E-2 scene-cut reset for soft Temporal Mask
target_sources(DLSSVideoPlayer PRIVATE src/SceneCutDetector.cpp)

# Step 04F synchronized split-screen comparator
target_sources(DLSSVideoPlayer PRIVATE src/SplitScreenLayout.cpp)
