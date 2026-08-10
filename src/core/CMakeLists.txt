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

    util/StringUtil.h
    util/StringUtil.cpp
    util/AppPaths.h
    util/AppPaths.cpp
    util/Time.h
    util/Time.cpp
    util/FileIO.h
    util/FileIO.cpp
    util/PathUtil.h
    util/PathUtil.cpp

    db/Schema.h
    db/Schema.cpp
    db/Database.h
    db/Database.cpp

    scan/DirWalker.h
    scan/DirWalker.cpp
    scan/Claims.h
    scan/Claims.cpp
    scan/Indexer.h
    scan/Indexer.cpp

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

target_include_directories(pixet_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${FFMPEG_INCLUDE_DIRS})
target_link_directories(pixet_core PUBLIC ${FFMPEG_LIBRARY_DIRS})

target_link_libraries(pixet_core PUBLIC
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
