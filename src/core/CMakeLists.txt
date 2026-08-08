find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(JPEG REQUIRED)

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

    decode/JpegCodec.h
    decode/JpegCodec.cpp

    thumb/ThumbGenerator.h
    thumb/ThumbGenerator.cpp
)

target_include_directories(pixet_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(pixet_core PUBLIC
    unofficial::sqlite3::sqlite3
    JPEG::JPEG
)
