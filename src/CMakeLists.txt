cmake_minimum_required(VERSION 3.20)
project(RTXPlayer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# libmpv headers from the mpv-dev package; the DLL itself is loaded at runtime.
set(MPV_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mpv/include")

add_executable(RTXPlayer WIN32
    main.cpp util.cpp enhance.cpp controlbar.cpp menu.cpp settingsdlg.cpp
    app.h app.rc app.manifest)
target_include_directories(RTXPlayer PRIVATE "${MPV_INCLUDE_DIR}")
target_compile_definitions(RTXPlayer PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(RTXPlayer PRIVATE /utf-8 /W4 /permissive- /EHsc)
target_link_libraries(RTXPlayer PRIVATE shlwapi comdlg32 comctl32 shell32 user32 gdi32 ole32 uuid)
set_property(TARGET RTXPlayer PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
