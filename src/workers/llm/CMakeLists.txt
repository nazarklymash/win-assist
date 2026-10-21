cmake_minimum_required(VERSION 3.24)
project(wa-llm LANGUAGES C CXX)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
file(TO_CMAKE_PATH "${WA_COMMON_DIR}" WA_COMMON_DIR)
file(TO_CMAKE_PATH "${WA_JSON_INCLUDE}" WA_JSON_INCLUDE)
file(TO_CMAKE_PATH "${WA_OUT_DIR}" WA_OUT_DIR)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_APP OFF CACHE BOOL "" FORCE)
set(LLAMA_OPENSSL OFF CACHE BOOL "" FORCE)
set(LLAMA_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(GGML_CUDA ${WA_CUDA} CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(llama
    URL https://github.com/ggml-org/llama.cpp/archive/refs/tags/b11800.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(llama)

add_executable(wa-llm main.cpp ${WA_COMMON_DIR}/pipe.cpp ${WA_COMMON_DIR}/strings.cpp)
target_compile_features(wa-llm PRIVATE cxx_std_20)
target_include_directories(wa-llm PRIVATE ${WA_COMMON_DIR}/.. ${WA_JSON_INCLUDE})
target_compile_definitions(wa-llm PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(wa-llm PRIVATE /utf-8 /EHsc /W3)
target_link_libraries(wa-llm PRIVATE llama shell32)
set_target_properties(wa-llm PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${WA_OUT_DIR})
