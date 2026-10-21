cmake_minimum_required(VERSION 3.24)
project(wa-stt LANGUAGES C CXX)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
file(TO_CMAKE_PATH "${WA_COMMON_DIR}" WA_COMMON_DIR)
file(TO_CMAKE_PATH "${WA_JSON_INCLUDE}" WA_JSON_INCLUDE)
file(TO_CMAKE_PATH "${WA_OUT_DIR}" WA_OUT_DIR)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(WHISPER_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(GGML_CUDA ${WA_CUDA} CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(whisper
    URL https://github.com/ggml-org/whisper.cpp/archive/refs/tags/v1.9.5.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(whisper)

add_executable(wa-stt main.cpp ${WA_COMMON_DIR}/pipe.cpp ${WA_COMMON_DIR}/strings.cpp)
target_compile_features(wa-stt PRIVATE cxx_std_20)
target_include_directories(wa-stt PRIVATE ${WA_COMMON_DIR}/.. ${WA_JSON_INCLUDE})
target_compile_definitions(wa-stt PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(wa-stt PRIVATE /utf-8 /EHsc /W3)
target_link_libraries(wa-stt PRIVATE whisper shell32)
set_target_properties(wa-stt PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${WA_OUT_DIR})
