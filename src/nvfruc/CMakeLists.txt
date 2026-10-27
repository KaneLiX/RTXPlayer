cmake_minimum_required(VERSION 3.20)
project(nvfruc CXX)

# VapourSynth plugin wrapping NVIDIA Optical Flow SDK FRUC (NvOFFRUC.dll, loaded at runtime).
# Build needs only headers: VapourSynth4.h (vs\sdk) and NvOFFRUC.h (Optical Flow SDK).
# The SDK is not redistributable: pass your copy, e.g. -DOFSDK_DIR=D:/Optical_Flow_SDK_5.0.7
set(OFSDK_DIR "$ENV{OFSDK_DIR}" CACHE PATH "NVIDIA Optical Flow SDK root")
if(NOT EXISTS "${OFSDK_DIR}/NvOFFRUC/Interface/NvOFFRUC.h")
    message(FATAL_ERROR "Set OFSDK_DIR to the NVIDIA Optical Flow SDK folder (contains NvOFFRUC/Interface/NvOFFRUC.h)")
endif()
set(VS_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../vs/sdk/include/vapoursynth" CACHE PATH "Folder with VapourSynth4.h")

add_library(nvfruc SHARED nvfruc.cpp)
target_compile_features(nvfruc PRIVATE cxx_std_20)
target_include_directories(nvfruc PRIVATE
    "${VS_INCLUDE_DIR}"
    "${OFSDK_DIR}/NvOFFRUC/Interface")
target_compile_definitions(nvfruc PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(nvfruc PRIVATE /utf-8 /W4 /O2)
set_property(TARGET nvfruc PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
target_link_libraries(nvfruc PRIVATE wintrust)
# Development probe, not shipped.
add_executable(frucprobe frucprobe.cpp)
target_include_directories(frucprobe PRIVATE "${OFSDK_DIR}/NvOFFRUC/Interface")
target_compile_definitions(frucprobe PRIVATE UNICODE _UNICODE)
target_compile_options(frucprobe PRIVATE /utf-8)

add_executable(ofprobe ofprobe.cpp)
target_include_directories(ofprobe PRIVATE "${OFSDK_DIR}/NvOFInterface"
    "${OFSDK_DIR}/NvOFFRUC/NvOFFRUCSample/cudart_header")
target_compile_definitions(ofprobe PRIVATE UNICODE _UNICODE)
target_compile_options(ofprobe PRIVATE /utf-8)

add_executable(psnr psnr.cpp)
target_compile_options(psnr PRIVATE /O2)

set_target_properties(nvfruc PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/../../plugins")
