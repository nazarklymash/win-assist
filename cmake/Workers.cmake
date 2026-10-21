# whisper.cpp and llama.cpp each vendor their own ggml, so every worker is a
# separate CMake project to keep the two ggml copies from colliding.
include(ExternalProject)

foreach(worker stt llm)
    ExternalProject_Add(wa_worker_${worker}
        SOURCE_DIR ${CMAKE_SOURCE_DIR}/src/workers/${worker}
        BINARY_DIR ${CMAKE_BINARY_DIR}/workers/${worker}
        CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
            -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
            -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
            -DWA_OUT_DIR=${WA_OUT_DIR}
            -DWA_CUDA=${WA_CUDA}
            -DWA_COMMON_DIR=${CMAKE_SOURCE_DIR}/src/common
            -DWA_JSON_INCLUDE=${nlohmann_json_SOURCE_DIR}/include
        INSTALL_COMMAND ""
        BUILD_ALWAYS ON
        USES_TERMINAL_CONFIGURE ON
        USES_TERMINAL_BUILD ON)
endforeach()
