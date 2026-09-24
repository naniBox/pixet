add_executable(pixet_tests
    TestHarness.h
    TestPaths.h
    main.cpp
    test_schema.cpp
    test_claims.cpp
    test_pathutil.cpp
    test_filemove.cpp
    test_fileops.cpp
    test_exif.cpp
    test_threadpool.cpp
    test_indexer.cpp
    test_dirrows.cpp
    test_thumbgen.cpp
    test_decodelimits.cpp
    test_pngcodec.cpp
    test_rawcache.cpp
    test_rawcodec.cpp
    test_videocodec.cpp
    test_tiffcodec.cpp
    test_webpcodec.cpp
    test_avifcodec.cpp
    test_heifcodec.cpp
    test_namefilter.cpp

    # The one piece of the app compiled in here: the name filter's matching rules are pure
    # logic, but they are written against QString/QRegularExpression, so this is also why
    # the tests link Qt6::Core (and only Core - nothing here touches a widget).
    ${PROJECT_SOURCE_DIR}/src/app/NameFilter.h
    ${PROJECT_SOURCE_DIR}/src/app/NameFilter.cpp
)

find_package(Qt6 REQUIRED COMPONENTS Core)

target_include_directories(pixet_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(pixet_tests PRIVATE pixet_core Qt6::Core)

# Windows finds a DLL next to the exe or on PATH, and dev builds deliberately keep Qt off
# both (see SETUP.md) - so without this pixet_tests.exe would fail to start when run
# directly, which is how scripts/win-e2e.ps1 runs it. Copying Qt6Core.dll alongside is the
# whole of the deployment it needs. macOS needs nothing: the Qt framework is found through
# the build tree's rpath, the same way the app's is.
if(WIN32)
    add_custom_command(TARGET pixet_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:pixet_tests> $<TARGET_FILE_DIR:pixet_tests>
        COMMAND_EXPAND_LISTS)
endif()

add_test(NAME pixet_tests COMMAND pixet_tests)
