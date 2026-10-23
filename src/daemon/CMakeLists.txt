add_library(wa_daemon_lib STATIC
    ai/gpu.cpp
    ai/http.cpp
    ai/llamacpp.cpp
    ai/model_router.cpp
    ai/ollama.cpp
    ai/ollama_models.cpp
    ai/providers.cpp
    ai/stt.cpp
    audio/capture.cpp
    audio/vad.cpp
    ipc/worker.cpp
    memory/memory.cpp
    overlay/overlay.cpp
    plugins/action.cpp
    plugins/host.cpp
    plugins/script_runner.cpp
    system/autostart.cpp
    system/inject.cpp
    system/ocr.cpp
    system/triggers.cpp
    system/reminders.cpp
    system/screenshot.cpp
    system/target.cpp
    tts/sapi.cpp
    tts/piper.cpp
    audio/devices.cpp
    assistant.cpp
    app.cpp
    tray.cpp)
target_include_directories(wa_daemon_lib PUBLIC ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(wa_daemon_lib PUBLIC
    wa_core fvad
    winhttp d2d1 dwrite windowscodecs shcore ole32 shell32 user32 gdi32 advapi32 sapi psapi dxgi windowsapp)

add_executable(wa-daemon WIN32 main.cpp wa-daemon.manifest)
target_link_libraries(wa-daemon PRIVATE wa_daemon_lib)
set_target_properties(wa-daemon PROPERTIES OUTPUT_NAME wa-daemon)

# Ship models next to the binary when present (scripts/download-models.ps1).
add_custom_command(TARGET wa-daemon POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${WA_OUT_DIR}/models
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR}/models -DDST=${WA_OUT_DIR}/models
            -P ${CMAKE_SOURCE_DIR}/cmake/CopyModels.cmake
    VERBATIM)
