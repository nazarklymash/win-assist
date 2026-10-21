add_executable(wa_tests
    main.cpp
    test_router.cpp
    test_hotkey.cpp
    test_manifest.cpp
    test_misc.cpp)
target_link_libraries(wa_tests PRIVATE wa_daemon_lib doctest::doctest)
add_test(NAME wa_tests COMMAND wa_tests)
