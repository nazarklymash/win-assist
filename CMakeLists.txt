cmake_minimum_required(VERSION 3.24)
project(win-assist VERSION 0.1.0 LANGUAGES C CXX)

if(NOT MSVC)
    message(FATAL_ERROR "win-assist is built with MSVC. Run scripts/build.ps1 or use a VS developer shell.")
endif()

option(WA_BUILD_WORKERS "Build the whisper.cpp / llama.cpp inference workers" ON)
option(WA_BUILD_TESTS "Build unit tests" ON)
option(WA_CUDA "Enable CUDA in the inference workers" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(WA_OUT_DIR ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${WA_OUT_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${WA_OUT_DIR})

include(cmake/Dependencies.cmake)

add_library(wa_options INTERFACE)
target_compile_definitions(wa_options INTERFACE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_compile_options(wa_options INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)

add_subdirectory(src/common)
add_subdirectory(src/core)
add_subdirectory(src/daemon)
add_subdirectory(plugins)

if(WA_BUILD_WORKERS)
    include(cmake/Workers.cmake)
endif()

if(WA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
