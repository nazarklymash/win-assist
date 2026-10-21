add_library(wa_common STATIC
    strings.cpp
    log.cpp
    paths.cpp
    pipe.cpp)
target_include_directories(wa_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(wa_common PUBLIC wa_options PRIVATE shell32 ole32)
