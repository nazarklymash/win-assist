include(FetchContent)

FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)

# libfvad ships autotools only; compile its sources as our own target.
FetchContent_Declare(libfvad
    URL https://github.com/dpirch/libfvad/archive/refs/tags/v1.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR _no_cmake)
FetchContent_MakeAvailable(libfvad)

file(GLOB_RECURSE WA_FVAD_SOURCES CONFIGURE_DEPENDS ${libfvad_SOURCE_DIR}/src/*.c)
add_library(fvad STATIC ${WA_FVAD_SOURCES})
target_include_directories(fvad PUBLIC ${libfvad_SOURCE_DIR}/include)
target_compile_options(fvad PRIVATE /W0)

if(WA_BUILD_TESTS)
    FetchContent_Declare(doctest
        URL https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(doctest)
endif()
