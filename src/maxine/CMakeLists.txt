cmake_minimum_required(VERSION 3.20)
project(nvvfx CXX)

# VapourSynth plugin for the NVIDIA Video Effects (Maxine VFX) SDK, loaded at runtime
# from the user's SDK copy. Builds against the SDK's headers and proxy sources only.
# The SDK is not redistributable: pass your copy, e.g. -DVFXSDK_DIR=D:/VFXSDK/VideoFX
set(VFXSDK_DIR "$ENV{VFXSDK_DIR}" CACHE PATH "NVIDIA VFX SDK root (VideoFX)")
if(NOT EXISTS "${VFXSDK_DIR}/nvvfx/include/nvVideoEffects.h")
    message(FATAL_ERROR "Set VFXSDK_DIR to the NVIDIA Video Effects SDK folder (contains nvvfx/include)")
endif()
set(VS_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../vs/sdk/include/vapoursynth" CACHE PATH "Folder with VapourSynth4.h")
set(VFX_INC "${VFXSDK_DIR}/nvvfx/include")
set(VFX_PROXY "${VFXSDK_DIR}/nvvfx/src/nvVideoEffectsProxy.cpp" "${VFXSDK_DIR}/nvvfx/src/nvCVImageProxy.cpp")

add_library(nvvfx SHARED nvvfx_vs.cpp ${VFX_PROXY})
target_compile_features(nvvfx PRIVATE cxx_std_20)
target_include_directories(nvvfx PRIVATE ${VFX_INC} "${VS_INCLUDE_DIR}")
target_compile_definitions(nvvfx PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(nvvfx PRIVATE /utf-8 /O2)
set_property(TARGET nvvfx PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
target_link_libraries(nvvfx PRIVATE wintrust)
set_target_properties(nvvfx PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/../../plugins")

# Development probe, not shipped.
add_executable(vfxprobe vfxprobe.cpp ${VFX_PROXY})
target_include_directories(vfxprobe PRIVATE ${VFX_INC})
target_compile_definitions(vfxprobe PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(vfxprobe PRIVATE /utf-8)
add_executable(imgcmp imgcmp.cpp)
target_compile_options(imgcmp PRIVATE /O2)
