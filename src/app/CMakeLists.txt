find_package(Qt6 REQUIRED COMPONENTS Widgets)

# Qt 6.8.3 on the macOS 26 SDK: drop a link to a framework the SDK no longer ships.
#
# Without this the app fails to link with "ld: framework 'AGL' not found". AGL was Apple's
# pre-OpenGL-3 legacy API, removed from the macOS SDK years after being deprecated.
#
# The mechanism is worth writing down because it's the reverse of what it looks like. Qt's
# lib/cmake/Qt6/FindWrapOpenGL.cmake (lines 40-46) does
# `find_library(WrapOpenGL_AGL NAMES AGL)` with a fallback to a literal `-framework AGL` if
# that fails. The lookup does *not* fail here - CMake's find_library searches the live
# system, where /System/Library/Frameworks/AGL.framework genuinely still exists - so
# WrapOpenGL::WrapOpenGL ends up carrying that absolute path in its interface, and CMake
# helpfully rewrites an absolute framework path into `-framework AGL` on the link line. The
# linker then resolves that against the *SDK*, which has no AGL at all. So it breaks
# precisely because the find succeeded, which is why matching the literal string
# "-framework AGL" here finds nothing and the filter has to catch the path form.
#
# Removing it is safe rather than merely expedient: nothing in a Qt Widgets app calls AGL,
# and `-framework OpenGL` - added by that same Qt file, and still in the SDK - is what Qt
# actually uses. Done here rather than by editing the Qt install so the fix travels with the
# repo and survives reinstalling Qt. Fixed in later Qt releases; delete this whole block
# when the pinned Qt moves past 6.8.
if(APPLE AND TARGET WrapOpenGL::WrapOpenGL)
    get_target_property(_pixet_wrapopengl_libs WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
    if(_pixet_wrapopengl_libs)
        list(FILTER _pixet_wrapopengl_libs EXCLUDE REGEX "(AGL\\.framework|^-framework AGL$)")
        set_target_properties(WrapOpenGL::WrapOpenGL PROPERTIES
            INTERFACE_LINK_LIBRARIES "${_pixet_wrapopengl_libs}")
    endif()
endif()

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
    PathQ.h

    FolderTreeView.h
    FolderTreeView.cpp

    BookmarkListWidget.h
    BookmarkListWidget.cpp

    ThumbGridModel.h
    ThumbGridModel.cpp
    FolderListModel.h
    FolderListModel.cpp
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

    WindowRegistry.h
    WindowRegistry.cpp

    ThreadShutdown.h
    ThreadShutdown.cpp

    FolderIndexer.h
    FolderIndexer.cpp

    BackgroundReconciler.h
    BackgroundReconciler.cpp

    RawRenderer.h
    RawRenderer.cpp

    DatabaseStatsDialog.h
    DatabaseStatsDialog.cpp

    Preferences.h
    Preferences.cpp
    PreferencesDialog.h
    PreferencesDialog.cpp

    KeyBindings.h
    KeyBindings.cpp

    StatusLabel.h
    StatusLabel.cpp
    StatusBarRow.h
    StatusBarRow.cpp

    FileOpsWorker.h
    FileOpsWorker.cpp
    CollisionDialog.h
    CollisionDialog.cpp
    ClipboardOps.h
    ClipboardOps.cpp

    HoverInfoWorker.h
    HoverInfoWorker.cpp

    SortIcons.h
    SortIcons.cpp

    LicenseDialog.h
    LicenseDialog.cpp

    RawCacheWarmer.h
    RawCacheWarmer.cpp

    ShellOps.h
    ShellOps.cpp

    icons.qrc
)

if(WIN32)
    # pixet.ico is generated from pixet.icns (see pixet.rc's comment for how) - CMake
    # recognizes a .rc source on a Windows target automatically and compiles it via
    # rc.exe, no extra properties needed the way the macOS .icns below requires.
    target_sources(pixet PRIVATE pixet.rc)
endif()

if(APPLE)
    # The .icns has to be a source file with MACOSX_PACKAGE_LOCATION, not just named in the
    # plist - that property is what actually copies it into Contents/Resources. Naming it in
    # CFBundleIconFile alone yields a bundle that references an icon it doesn't contain, and
    # macOS silently falls back to the generic executable icon.
    set(PIXET_ICON "${CMAKE_CURRENT_SOURCE_DIR}/pixet.icns")
    target_sources(pixet PRIVATE ${PIXET_ICON})
    set_source_files_properties(${PIXET_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

    set_target_properties(pixet PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
        MACOSX_BUNDLE_EXECUTABLE_NAME "pixet"
        # Stable and reverse-DNS. This is the key macOS hangs per-app state off - window
        # restore, and the TCC grants for reading ~/Pictures - so it must not change between
        # builds or the app forgets its permissions every time.
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.nanibox.pixet"
        MACOSX_BUNDLE_BUNDLE_NAME "pixet"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "pixet.icns"
        MACOSX_BUNDLE_COPYRIGHT "© naniBox"
    )
endif()

# Defaults ON deliberately, including for release builds: MainWindow.h's comment on
# onCopyGridDebugInfo() asks for the &Debug menu to stay available on the daily-driver build
# permanently, since it's the fast path for capturing state when the grid column-fit bug
# recurs. scripts/deploy-mac.sh turns it OFF so it doesn't ship to anyone else.
option(PIXET_DEBUG_MENU "Include the &Debug menu (grid state capture)" ON)
if(PIXET_DEBUG_MENU)
    target_compile_definitions(pixet PRIVATE PIXET_DEBUG_MENU)
endif()

# Widgets only, and deliberately so. Qt6::Sql and Qt6::Concurrent are the two that look like
# they belong here and don't: the database layer talks to the sqlite3 C API directly (see
# core/db/Database.h) and the thread pools are std::thread (see util/ThreadPool.h). Linking
# them costs 4.8MB in the deployed bundle, because macdeployqt correctly follows the linkage:
# QtSql.framework, QtConcurrent.framework, and all four SQL driver plugins - including
# libqsqlpsql and libqsqlmimer, i.e. Postgres and Mimer client drivers, shipped to users of a
# local photo viewer. QT_DEBUG_PLUGINS=1 confirms the sqldrivers plugins are never loaded.
target_link_libraries(pixet PRIVATE
    pixet_core
    Qt6::Widgets
)
