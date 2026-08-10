find_package(Qt6 REQUIRED COMPONENTS Widgets Sql Concurrent)

# WIN32 (no console window) only has an effect on Windows - CMake ignores it
# elsewhere - but macOS needs the analogous MACOSX_BUNDLE instead to get a real,
# double-clickable .app (with an Info.plist) rather than a bare Unix binary.
if(APPLE)
    set(PIXET_APP_TYPE MACOSX_BUNDLE)
else()
    set(PIXET_APP_TYPE WIN32)
endif()

add_executable(pixet ${PIXET_APP_TYPE}
    main.cpp
    MainWindow.h
    MainWindow.cpp

    QtInterop.h

    FolderTreeView.h
    FolderTreeView.cpp

    ThumbGridModel.h
    ThumbGridModel.cpp
    ThumbGridView.h
    ThumbGridView.cpp
    ThumbLoader.h
    ThumbLoader.cpp

    PreviewDecoder.h
    PreviewDecoder.cpp
    PreviewPane.h
    PreviewPane.cpp

    FullscreenDecoder.h
    FullscreenDecoder.cpp
    FullscreenViewer.h
    FullscreenViewer.cpp

    FolderIndexer.h
    FolderIndexer.cpp

    BackgroundReconciler.h
    BackgroundReconciler.cpp

    RawRenderer.h
    RawRenderer.cpp
)

target_link_libraries(pixet PRIVATE
    pixet_core
    Qt6::Widgets
    Qt6::Sql
    Qt6::Concurrent
)
