add_executable(wa_tests
    main.cpp
    test_router.cpp
    test_hotkey.cpp
    test_manifest.cpp
    test_memory.cpp
    test_misc.cpp
    test_model_router.cpp
    test_ocr.cpp
    test_triggers.cpp)
target_link_libraries(wa_tests PRIVATE wa_daemon_lib doctest::doctest)
add_test(NAME wa_tests COMMAND wa_tests)
