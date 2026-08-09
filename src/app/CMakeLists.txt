find_package(Qt6 REQUIRED COMPONENTS Widgets Sql Concurrent)

add_executable(pixet WIN32
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
)

target_link_libraries(pixet PRIVATE
    pixet_core
    Qt6::Widgets
    Qt6::Sql
    Qt6::Concurrent
)
