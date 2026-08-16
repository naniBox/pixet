find_package(Threads REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
find_package(libraw CONFIG REQUIRED)
find_package(FFMPEG REQUIRED)
find_package(TIFF REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(libavif CONFIG REQUIRED)
find_package(libheif CONFIG REQUIRED)

add_library(pixet_core STATIC
    version.h
    version.cpp

    util/AppPaths.h
    util/Time.h
    util/Time.cpp
    util/FileIO.h
    util/FileMove.h
    util/PathUtil.h
    util/PathUtil.cpp
    util/ProcessId.h
    util/ProcessId.cpp
    util/Profile.h
    util/Profile.cpp
    util/ThreadPool.h
    util/ThreadPool.cpp

    db/Schema.h
    db/Schema.cpp
    db/Database.h
    db/Database.cpp

    scan/DirWalker.h
    scan/DirRows.h
    scan/DirRows.cpp
    scan/Claims.h
    scan/Claims.cpp
    scan/Indexer.h
    scan/Indexer.cpp

    fileops/FileOps.h
    fileops/FileOps.cpp

    meta/JpegExif.h
    meta/JpegExif.cpp

    decode/RgbImage.h
    decode/RgbImage.cpp
    decode/JpegCodec.h
    decode/JpegCodec.cpp
    decode/PngCodec.h
    decode/PngCodec.cpp
    decode/RawCodec.h
    decode/RawCodec.cpp
    decode/TiffCodec.h
    decode/TiffCodec.cpp
    decode/VideoCodec.h
    decode/VideoCodec.cpp
    decode/WebpCodec.h
    decode/WebpCodec.cpp
    decode/AvifCodec.h
    decode/AvifCodec.cpp
    decode/HeifCodec.h
    decode/HeifCodec.cpp
    decode/DisplayCodec.h
    decode/DisplayCodec.cpp

    thumb/ThumbGenerator.h
    thumb/ThumbGenerator.cpp
)

# util/{FileIO,PathUtil,AppPaths}.h and scan/DirWalker.h declare a portable (UTF-8
# std::string) interface with a per-platform implementation. Each platform also gets one
# helper that only it needs, and which is deliberately absent on the other:
# util/StringUtil (UTF-8 <-> UTF-16, for crossing into WinAPI) is Windows-only, and
# util/Unicode (NFC normalization, because macOS hands back decomposed filenames) is
# macOS-only. Both headers therefore live in their platform's branch rather than in the
# unconditional list above - listing StringUtil.h unconditionally used to leave two
# std::wstring declarations with no definition on any non-Windows build.
if(WIN32)
    target_sources(pixet_core PRIVATE
        util/StringUtil.h
        util/StringUtil_win.cpp
        util/FileIO_win.cpp
        util/FileMove_win.cpp
        util/PathUtil_win.cpp
        util/AppPaths_win.cpp
        scan/DirWalker_win.cpp
    )
elseif(APPLE)
    target_sources(pixet_core PRIVATE
        util/Unicode.h
        util/Unicode_mac.cpp
        util/FileIO_mac.cpp
        util/FileMove_mac.mm
        util/PathUtil_mac.cpp
        util/AppPaths_mac.cpp
        scan/DirWalker_mac.cpp
    )
    # CoreFoundation is needed by util/Unicode_mac.cpp alone (CFStringNormalize), in
    # preference to taking on an ICU dependency for the one Unicode operation this
    # codebase performs. Foundation is separate (the Objective-C framework, not just its
    # C-level subset) - needed by util/FileMove_mac.mm's moveToTrash(), the one place
    # this codebase calls into Objective-C (NSFileManager) rather than plain POSIX/Darwin
    # C, since there's no POSIX API for the real per-volume Trash.
    target_link_libraries(pixet_core PUBLIC "-framework CoreFoundation" "-framework Foundation")
else()
    message(FATAL_ERROR
        "pixet_core has no implementation for this platform yet: "
        "util/FileIO, util/PathUtil, util/AppPaths, scan/DirWalker. "
        "See the *_win.cpp / *_mac.cpp pairs for what each needs to do.")
endif()

# The version string lives in exactly one place - project(VERSION) in the top-level
# CMakeLists - and reaches version.cpp through here, so the library and the .app bundle's
# CFBundleShortVersionString can't drift apart.
target_compile_definitions(pixet_core PRIVATE PIXET_VERSION="${PROJECT_VERSION}")

# Checked-in instrumentation, compiled out by default - see util/Profile.h. PUBLIC because the
# macros are used in src/app and src/index too, and a header-level define that only half the
# targets see would silently profile some translation units and not others.
#
# Deliberately not tied to CMAKE_BUILD_TYPE: the interesting measurements are of *release*
# behaviour against a real library (a debug build's decode times are ~8x off and would send you
# after the wrong bottleneck), so this has to be independently switchable.
#   cmake --preset mac-release -DPIXET_PROFILE=ON
option(PIXET_PROFILE "Compile in the scope/counter/timeline profiler (util/Profile.h)" OFF)
if(PIXET_PROFILE)
    target_compile_definitions(pixet_core PUBLIC PIXET_PROFILE)
endif()

# The commit the binary was built from, captured at *build* time. A custom target rather
# than a configure-time execute_process: configure doesn't re-run when you commit, so a
# hash captured there would keep reporting whatever was checked out the last time CMake
# happened to run. The script rewrites its header only when the value actually changes, so
# this doesn't rebuild the world on every build. See cmake/GitVersion.cmake.
set(PIXET_GITVERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pixet_gitversion.h")
add_custom_target(pixet_gitversion
    BYPRODUCTS "${PIXET_GITVERSION_HEADER}"
    COMMAND ${CMAKE_COMMAND}
            -DSRC_DIR=${CMAKE_SOURCE_DIR}
            -DOUTPUT_FILE=${PIXET_GITVERSION_HEADER}
            -P ${CMAKE_SOURCE_DIR}/cmake/GitVersion.cmake
    COMMENT "Capturing git commit for the version string"
    VERBATIM)
add_dependencies(pixet_core pixet_gitversion)
target_include_directories(pixet_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

target_include_directories(pixet_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${FFMPEG_INCLUDE_DIRS})
target_link_directories(pixet_core PUBLIC ${FFMPEG_LIBRARY_DIRS})

target_link_libraries(pixet_core PUBLIC
    Threads::Threads
    unofficial::sqlite3::sqlite3
    JPEG::JPEG
    PNG::PNG
    # Thread-safe variant, not libraw::raw - ThumbGenerator can run concurrently on
    # several worker threads within one process (ThumbLoader, FolderIndexer,
    # BackgroundReconciler all decode independently), and LibRaw's plain build uses
    # some global/static state that isn't safe across concurrent instances.
    libraw::raw_r
    ${FFMPEG_LIBRARIES}
    TIFF::TIFF
    WebP::webp
    WebP::webpdecoder
    avif
    heif
)
