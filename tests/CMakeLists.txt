add_executable(pixet_tests
    TestHarness.h
    TestPaths.h
    main.cpp
    test_schema.cpp
    test_claims.cpp
    test_exif.cpp
    test_thumbgen.cpp
)

target_link_libraries(pixet_tests PRIVATE pixet_core)

add_test(NAME pixet_tests COMMAND pixet_tests)
