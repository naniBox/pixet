add_executable(pixet_tests
    TestHarness.h
    TestPaths.h
    main.cpp
    test_schema.cpp
    test_claims.cpp
    test_exif.cpp
    test_thumbgen.cpp
    test_pngcodec.cpp
    test_rawcodec.cpp
    test_videocodec.cpp
    test_tiffcodec.cpp
    test_webpcodec.cpp
    test_avifcodec.cpp
    test_heifcodec.cpp
)

target_link_libraries(pixet_tests PRIVATE pixet_core)

add_test(NAME pixet_tests COMMAND pixet_tests)
