add_library(wa_core STATIC
    base64.cpp
    config.cpp
    hotkey.cpp
    manifest.cpp
    router.cpp
    tools.cpp)
target_include_directories(wa_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(wa_core PUBLIC wa_common nlohmann_json::nlohmann_json)
