#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>

#include <algorithm>
#include <QDateTime>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDrag>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFileSystemWatcher>
#include <QMessageBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QPixmap>
#include <QProcess>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include "BackgroundReconciler.h"
#include "ClipboardOps.h"
#include "CollisionDialog.h"
#include "DatabaseStatsDialog.h"
#include "FileOpsWorker.h"
#include "FolderIndexer.h"
#include "FolderTreeView.h"
#include "FullscreenViewer.h"
#include "HoverInfoWorker.h"
#include "KeyBindings.h"
#include "PathQ.h"
#include "Preferences.h"
#include "PreferencesDialog.h"
#include "PreviewDecoder.h"
#include "PreviewPane.h"
#include "RawRenderer.h"
#include "StatusBarRow.h"
#include "StatusLabel.h"
#include "ThumbGridModel.h"
#include "ThumbGridView.h"
#include "ThumbLoader.h"
#include "db/Database.h"
#include "db/Schema.h"
#include "util/AppPaths.h"
#include "util/FileMove.h"
#include "util/PathUtil.h"
#include "version.h"

namespace {
// dirs.path / files.name are UTF-8 in the DB; a path from QFileSystemModel has to be put
// into the exact same form pixet_core writes before it's used as a lookup key, or the
// query silently misses rows that are really there. That means the platform's separator
// style (QFileSystemModel uses forward slashes even on Windows) and, on macOS, the Unicode
// normalization form too - normalizePath() handles both.
QString normalizeForDb(const QString &path) {
    return QString::fromStdString(pixet::normalizePath(path.toStdString()));
}

// Next sibling of `idx` - or, if it's the last one, walks up until an ancestor
// actually has a next sibling ("aunt": your parent's next sibling; a grandparent's
// if the parent is *also* last, and so on) - rather than just stopping at the last
// child in a folder. Returns an invalid index only once this walks all the way up
// without finding one (nothing left in the tree). Shared by Ctrl+Down (always) and
// Ctrl+Right (only once it's out of children to descend into - see
// MainWindow::onNavigateFolderRequested()).
QModelIndex nextSiblingOrAunt(QFileSystemModel *model, const QModelIndex &idx) {
    QModelIndex current = idx;
    while (current.isValid()) {
        QModelIndex candidate = model->index(current.row() + 1, 0, current.parent());
        if (candidate.isValid()) return candidate;
        current = current.parent();
    }
    return QModelIndex();
}
} // namespace

MainWindow::MainWindow(bool resetLayout, QWidget *parent) : QMainWindow(parent), resetLayout_(resetLayout) {
    setWindowTitle(QStringLiteral("pixet %1").arg(pixet::version()));
    resize(1280, 800);

    db_ = std::make_unique<pixet::Database>(pixet::indexDbPath(), pixet::thumbsDbPath(), false);

    // --- left panel: folder tree + bookmarks (top), preview (bottom, user-resizable) ---
    // palette(mid) (previously used for both titles below) turned out to read as
    // basically black on this app's dark theme - too close to the background to
    // actually see. placeholder-text is Qt's actual semantic role for "muted but
    // still legible" text.
    auto makeSectionTitle = [](const QString &text, QWidget *parent) {
        auto *title = new QLabel(text, parent);
        title->setStyleSheet(QStringLiteral("color: palette(placeholder-text); font-weight: bold;"));
        return title;
    };

    bookmarks_ = new QListWidget(this);
    bookmarks_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(bookmarks_, &QListWidget::itemClicked, this, &MainWindow::onBookmarkClicked);
    connect(bookmarks_, &QListWidget::customContextMenuRequested, this, &MainWindow::onBookmarksContextMenu);

    auto *bookmarksPanel = new QWidget(this);
    auto *bookmarksLayout = new QVBoxLayout(bookmarksPanel);
    bookmarksLayout->setContentsMargins(0, 0, 0, 0);
    bookmarksLayout->setSpacing(2);
    bookmarksLayout->addWidget(makeSectionTitle(QStringLiteral("Bookmarks"), bookmarksPanel));
    bookmarksLayout->addWidget(bookmarks_, /*stretch=*/1);

    fsModel_ = new QFileSystemModel(this);
    fsModel_->setRootPath(QString());
    // QDir::Drives is what surfaces the drive-letter list on Windows and is simply inert
    // elsewhere. Kept unconditionally: it costs nothing on macOS and removing it would only
    // make the Windows behaviour depend on a platform check.
    fsModel_->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot | QDir::Drives);

    tree_ = new FolderTreeView(this);
    tree_->setModel(fsModel_);
    // On Windows an empty root index means "show the drives", which is a short, useful list.
    // On macOS it means a single "/" node, so every launch starts with the user four levels
    // of clicking away from their own photos, past /System and /Library. Rooting the tree at
    // $HOME is the sane default there; external volumes are still reachable because
    // navigateTo() can go anywhere (Choose Folder..., the path bar, or a bookmark), and
    // /Volumes gets seeded as a bookmark on first run - see loadBookmarks().
#ifdef Q_OS_MACOS
    tree_->setRootIndex(fsModel_->index(QDir::homePath()));
#else
    tree_->setRootIndex(fsModel_->index(QString()));
#endif
    tree_->setHeaderHidden(true);
    for (int col = 1; col < fsModel_->columnCount(); ++col) tree_->hideColumn(col);
    // QTreeView defaults to stretchLastSection(true), which forces the (only visible)
    // column to always exactly fill the viewport - a long/deeply-indented name just
    // gets elided with no way to see the rest. Let the column grow to fit its content
    // instead, with a horizontal scrollbar picking up the overflow.
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tree_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    tree_->setTextElideMode(Qt::ElideNone);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QWidget::customContextMenuRequested, this, &MainWindow::onTreeContextMenu);
    connect(tree_->selectionModel(), &QItemSelectionModel::currentChanged, this, &MainWindow::onTreeSelectionChanged);
    connect(fsModel_, &QFileSystemModel::directoryLoaded, this, &MainWindow::onTreeDirectoryLoaded);

    // Wrapped with its own title now too, matching bookmarksPanel - previously
    // considered self-explanatory next to a labeled list, but side by side the
    // asymmetry read as a missing label rather than an intentional omission.
    auto *treePanel = new QWidget(this);
    auto *treePanelLayout = new QVBoxLayout(treePanel);
    treePanelLayout->setContentsMargins(0, 0, 0, 0);
    treePanelLayout->setSpacing(2);
    treePanelLayout->addWidget(makeSectionTitle(QStringLiteral("Folders"), treePanel));
    treePanelLayout->addWidget(tree_, /*stretch=*/1);

    // --- right: preview pane (constructed here so topSplitter_/leftPanel_ below can
    // reference it - actually placed at the bottom of the left column, see layout) ---
    preview_ = new PreviewPane(this);

    topSplitter_ = new QSplitter(Qt::Horizontal, this);
    topSplitter_->addWidget(treePanel);
    topSplitter_->addWidget(bookmarksPanel);
    topSplitter_->setStretchFactor(0, 7);
    topSplitter_->setStretchFactor(1, 3);
    topSplitter_->setCollapsible(0, false);
    topSplitter_->setCollapsible(1, false);

    // Preview used to be forced square (height pinned to match leftPanel_'s width on
    // every resize - see git history). A real QSplitter handle lets the user pick
    // whatever height they actually want instead - e.g. wide-but-short for a
    // panorama, or tall for a portrait shot - and leftSplitterState below persists
    // that choice the same way mainSplitterState/topSplitterState already do.
    leftSplitter_ = new QSplitter(Qt::Vertical, this);
    leftSplitter_->addWidget(topSplitter_);
    leftSplitter_->addWidget(preview_);
    leftSplitter_->setStretchFactor(0, 1);
    leftSplitter_->setStretchFactor(1, 0);
    leftSplitter_->setCollapsible(0, false);
    leftSplitter_->setCollapsible(1, false);

    leftPanel_ = new QWidget(this);
    auto *leftLayout = new QVBoxLayout(leftPanel_);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(leftSplitter_);

    // --- center: thumbnail grid ---
    gridModel_ = new ThumbGridModel(*db_, this);
    grid_ = new ThumbGridView(this);
    grid_->setModel(gridModel_);
    grid_->setIconSize(QSize(prefs::thumbnailIconSize(), prefs::thumbnailIconSize()));
    connect(grid_, &ThumbGridView::currentRowChanged, this, &MainWindow::onGridSelectionChanged);
    // currentRowChanged (above) covers the lead-row-moved case (preview/path bar/
    // status-bar detail); selectionChanged covers count/membership changes that don't
    // necessarily move the lead (e.g. a Ctrl+click toggling some other row) - both
    // feed updateSelectionStatus(), which reads whichever of currentRow()/
    // selectedRows() it needs regardless of which signal triggered it.
    connect(grid_, &ThumbGridView::selectionChanged, this, &MainWindow::updateSelectionStatus);
    connect(grid_, &ThumbGridView::selectionChanged, this, &MainWindow::updateEditActionsEnabled);
    connect(grid_, &ThumbGridView::ctrlHoverRowChanged, this, &MainWindow::onGridCtrlHoverChanged);
    grid_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(grid_, &QWidget::customContextMenuRequested, this, &MainWindow::onGridContextMenu);
    connect(grid_, &ThumbGridView::navigateFolderRequested, this, &MainWindow::onNavigateFolderRequested);
    // Fires on both double-click and Enter/Return - exactly the two ways to "open"
    // an item.
    connect(grid_, &ThumbGridView::activated, this, &MainWindow::onGridItemActivated);
    connect(grid_, &ThumbGridView::filesDropped, this, &MainWindow::onFilesDroppedOnGrid);
    connect(grid_, &ThumbGridView::dragOutRequested, this, &MainWindow::onDragOutRequested);

    fullscreenViewer_ = new FullscreenViewer(this);
    // Keep the grid's selection following along while browsing fullscreen, so
    // closing it (Escape/double-click) leaves the grid on whatever image was last
    // shown there instead of wherever it was when fullscreen opened.
    connect(fullscreenViewer_, &FullscreenViewer::contextMenuRequested, this,
            &MainWindow::onFullscreenContextMenu);
    connect(fullscreenViewer_, &FullscreenViewer::rowChanged, this, [this](int row) {
        grid_->setCurrentRow(row);
        grid_->scrollToRow(row);
    });

    // --- path bar: shows/edits currentPath_; Enter navigates (see navigateToInput).
    // Editable QComboBox rather than a plain QLineEdit so its dropdown can double as
    // recently-visited-folder history (see refreshPathBarHistory()/prefs::pathHistory()) ---
    pathBar_ = new QComboBox(this);
    pathBar_->setEditable(true);
    pathBar_->setInsertPolicy(QComboBox::NoInsert); // history is only ever written via navigateTo(), never by typing
    pathBar_->setPlaceholderText(QStringLiteral("Path..."));
    refreshPathBarHistory();
    // returnPressed lives on the combo's internal line edit, not the combo itself -
    // fires for text typed/pasted directly, matching the old QLineEdit behavior
    // exactly (an arbitrary path doesn't need to already be a history entry).
    connect(pathBar_->lineEdit(), &QLineEdit::returnPressed, this, &MainWindow::onPathBarReturnPressed);
    // Fires when a dropdown entry is actually picked (mouse click, or arrow keys +
    // Enter while the popup is open) - textActivated already updates currentText()
    // before this runs, so the same submit path applies.
    connect(pathBar_, &QComboBox::textActivated, this, &MainWindow::onPathBarHistoryActivated);
    // select-all-on-focus is implemented in eventFilter() (QEvent::FocusIn) - that
    // only fires because of this call, which got missed when the feature was
    // originally added. Installed on the combo's actual line edit, since that's what
    // receives keyboard focus (and therefore FocusIn) for an editable QComboBox, not
    // the QComboBox widget itself.
    pathBar_->lineEdit()->installEventFilter(this);
    // Application-wide, for the mouse's back/forward buttons (see eventFilter()). It has to
    // be this broad: the grid, the tree and the preview pane all accept mouse presses
    // themselves, so an override on this window would only ever see events none of them
    // wanted - which is never, over the areas the user is actually clicking.
    QCoreApplication::instance()->installEventFilter(this);

    // --- back/forward: plain in-memory navigateTo() stack, see navHistory_'s doc
    // comment. Fixed (non-configurable) shortcuts on the QAction itself, same
    // reasoning as the Edit menu's standard shortcuts - the toolbar buttons below
    // just display whichever QAction they're set as the default action for, so the
    // shortcut/enabled-state/icon only ever need to be set in one place. ---
    backAction_ = new QAction(style()->standardIcon(QStyle::SP_ArrowBack), QStringLiteral("Back"), this);
    backAction_->setShortcut(QKeySequence(QStringLiteral("Alt+Left")));
    backAction_->setEnabled(false);
    connect(backAction_, &QAction::triggered, this, &MainWindow::onNavigateBack);

    forwardAction_ = new QAction(style()->standardIcon(QStyle::SP_ArrowForward), QStringLiteral("Forward"), this);
    forwardAction_->setShortcut(QKeySequence(QStringLiteral("Alt+Right")));
    forwardAction_->setEnabled(false);
    connect(forwardAction_, &QAction::triggered, this, &MainWindow::onNavigateForward);

    auto *backButton = new QToolButton(this);
    backButton->setDefaultAction(backAction_);
    backButton->setAutoRaise(true); // flat, address-bar-adjacent look rather than a raised push button
    auto *forwardButton = new QToolButton(this);
    forwardButton->setDefaultAction(forwardAction_);
    forwardButton->setAutoRaise(true);

    // --- top-level: back/forward + path bar above, left column (40%) vs. thumbnail grid (60%) below ---
    splitter_ = new QSplitter(this);
    splitter_->addWidget(leftPanel_);
    splitter_->addWidget(grid_);
    splitter_->setStretchFactor(0, 2);
    splitter_->setStretchFactor(1, 3);
    splitter_->setCollapsible(0, false);
    splitter_->setCollapsible(1, false);

    auto *central = new QWidget(this);
    auto *centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(4, 4, 4, 4);
    auto *navRowLayout = new QHBoxLayout();
    navRowLayout->setContentsMargins(0, 0, 0, 0);
    navRowLayout->addWidget(backButton);
    navRowLayout->addWidget(forwardButton);
    navRowLayout->addWidget(pathBar_, /*stretch=*/1);
    centralLayout->addLayout(navRowLayout);
    centralLayout->addWidget(splitter_, /*stretch=*/1);
    setCentralWidget(central);

    restoreWindowState(); // window position/size only - see the method for why

    // Splitter sizing needs real geometry to mean anything (percentages of width, or
    // restoreState() blobs that encode absolute pixel positions) and none exists yet
    // pre-show() - central widget width is still 0 here. Deferring to after the first
    // real layout pass, rather than computing now and reasserting the same (already
    // wrong) numbers later, is the fix; capturing-then-replaying a pre-layout value was
    // tried first and just reproduced the same "everything squashed into 40px" bug this
    // paragraph exists to warn about.
    QTimer::singleShot(0, this, [this]() {
        QSettings settings = prefs::settingsStore();

        bool splitterRestored = false;
        if (!resetLayout_) {
            QByteArray state = settings.value(QStringLiteral("mainSplitterState")).toByteArray();
            if (!state.isEmpty()) splitterRestored = splitter_->restoreState(state);
        }
        if (!splitterRestored) {
            int w = width();
            splitter_->setSizes({static_cast<int>(w * 0.4), static_cast<int>(w * 0.6)});
        }

        bool topSplitterRestored = false;
        if (!resetLayout_) {
            QByteArray state = settings.value(QStringLiteral("topSplitterState")).toByteArray();
            if (!state.isEmpty()) topSplitterRestored = topSplitter_->restoreState(state);
        }
        if (!topSplitterRestored) {
            int leftW = leftPanel_->width() > 0 ? leftPanel_->width() : static_cast<int>(width() * 0.4);
            topSplitter_->setSizes({static_cast<int>(leftW * 0.7), static_cast<int>(leftW * 0.3)});
        }

        bool leftSplitterRestored = false;
        if (!resetLayout_) {
            QByteArray state = settings.value(QStringLiteral("leftSplitterState")).toByteArray();
            if (!state.isEmpty()) leftSplitterRestored = leftSplitter_->restoreState(state);
        }
        if (!leftSplitterRestored) {
            // Roughly matches the old forced-square look at a typical left-panel
            // width, without actually forcing anything - just a reasonable first
            // impression the user can immediately resize away from.
            int leftW = leftPanel_->width() > 0 ? leftPanel_->width() : static_cast<int>(width() * 0.4);
            int h = leftSplitter_->height() > 0 ? leftSplitter_->height() : height();
            leftSplitter_->setSizes({qMax(0, h - leftW), leftW});
        }
    });

    // --- background workers (no parent - see the member declarations in the header) ---
    thumbLoader_ = std::make_unique<ThumbLoader>();
    // The worker can't safely ask a widget or QScreen for this itself, so it's pushed in from
    // here - see ThumbLoader::setDevicePixelRatio(). Without it grid thumbnails decode to the
    // logical icon size and get upscaled on a Retina display.
    thumbLoader_->setDevicePixelRatio(devicePixelRatioF());
    connect(gridModel_, &ThumbGridModel::thumbNeeded, thumbLoader_.get(), &ThumbLoader::request);
    connect(thumbLoader_.get(), &ThumbLoader::thumbReady, gridModel_, &ThumbGridModel::setThumbnail);
    // ThumbGridModel::setThumbnail() already emits dataChanged(), and
    // ThumbGridView::setModel() already connects that straight to a viewport
    // repaint - no separate "make sure it actually repainted" connection needed
    // here anymore. (A previous QListView-based grid needed exactly that as a
    // belt-and-suspenders fix for IconMode's own unreliable partial repaints; the
    // custom-painted replacement doesn't have that failure mode, since there's no
    // second layout/paint engine left that could disagree with what dataChanged
    // says needs redrawing.)

    previewDecoder_ = std::make_unique<PreviewDecoder>();
    connect(previewDecoder_.get(), &PreviewDecoder::previewReady, this, &MainWindow::onPreviewReady);

    folderIndexer_ = std::make_unique<FolderIndexer>();
    connect(this, &MainWindow::requestIndex, folderIndexer_.get(), &FolderIndexer::indexFolder);
    connect(folderIndexer_.get(), &FolderIndexer::started, this, &MainWindow::onIndexerStarted);
    connect(folderIndexer_.get(), &FolderIndexer::filesListed, this, &MainWindow::onFilesListed);
    connect(folderIndexer_.get(), &FolderIndexer::thumbsProgress, this, &MainWindow::onThumbsProgress);
    connect(folderIndexer_.get(), &FolderIndexer::finished, this, &MainWindow::onIndexerFinished);

    backgroundReconciler_ = std::make_unique<BackgroundReconciler>();
    connect(backgroundReconciler_.get(), &BackgroundReconciler::directoryChanged, this,
            &MainWindow::onBackgroundDirectoryChanged);
    connect(this, &MainWindow::requestFullReindex, backgroundReconciler_.get(),
            &BackgroundReconciler::triggerFullSweepNow);
    backgroundReconciler_->start();

    rawRenderer_ = std::make_unique<RawRenderer>();
    connect(rawRenderer_.get(), &RawRenderer::directoryChanged, this, &MainWindow::onBackgroundDirectoryChanged);
    connect(this, &MainWindow::requestRawRenderPriority, rawRenderer_.get(), &RawRenderer::prioritize);
    rawRenderer_->start();

    fileOps_ = std::make_unique<FileOpsWorker>();
    connect(this, &MainWindow::requestFileOpPreflight, fileOps_.get(), &FileOpsWorker::preflight);
    connect(this, &MainWindow::requestFileOpExecute, fileOps_.get(), &FileOpsWorker::execute);
    connect(fileOps_.get(), &FileOpsWorker::preflightReady, this, &MainWindow::onFileOpPreflightReady);
    connect(fileOps_.get(), &FileOpsWorker::progress, this, &MainWindow::onFileOpProgress);
    connect(fileOps_.get(), &FileOpsWorker::finished, this, &MainWindow::onFileOpFinished);

    previewDebounce_ = new QTimer(this);
    previewDebounce_->setSingleShot(true);
    previewDebounce_->setInterval(80);
    connect(previewDebounce_, &QTimer::timeout, this, &MainWindow::triggerPreviewRequest);

    // See folderWatcher_'s member comment: a real-time watch on whichever folder is
    // currently on screen, so an external change (most concretely, Explorer
    // performing the actual move for a Cut that originated in pixet) shows up
    // immediately instead of waiting on BackgroundReconciler's slow rotation.
    folderWatcher_ = new QFileSystemWatcher(this);
    connect(folderWatcher_, &QFileSystemWatcher::directoryChanged, this, &MainWindow::onWatchedDirectoryChanged);
    folderWatchDebounce_ = new QTimer(this);
    folderWatchDebounce_->setSingleShot(true);
    folderWatchDebounce_->setInterval(400); // coalesces the several native notifications one multi-file paste fires
    connect(folderWatchDebounce_, &QTimer::timeout, this, [this]() {
        if (!currentPath_.isEmpty()) emit requestIndex(currentPath_, /*force=*/true, false);
    });

    // --- menu ---
    // Shortcuts are user-configurable (see KeyBindings.h/PreferencesDialog) - the
    // QAction pointers are kept as members so onPreferences() can re-apply their
    // shortcuts after the editor closes, since QAction doesn't watch settings
    // itself.
    auto *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    fileMenu->addAction(QStringLiteral("Choose Folder..."), this, &MainWindow::onChooseFolder);

    // Select All uses a fixed QKeySequence::StandardKey rather than the configurable
    // KeyBindings system - see KeyBindings.h's class comment. setShortcuts() (plural)
    // installs every platform binding for that standard key, not just the primary
    // one (e.g. Windows also gets the legacy Ins/Del-based Copy/Cut/Paste aliases
    // once those are added in a later phase).
    auto *editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    cutAction_ = editMenu->addAction(QStringLiteral("Cut"), this, &MainWindow::onEditCut);
    cutAction_->setShortcuts(QKeySequence::Cut);
    copyAction_ = editMenu->addAction(QStringLiteral("Copy"), this, &MainWindow::onEditCopy);
    copyAction_->setShortcuts(QKeySequence::Copy);
    pasteAction_ = editMenu->addAction(QStringLiteral("Paste"), this, &MainWindow::onEditPaste);
    pasteAction_->setShortcuts(QKeySequence::Paste);
    editMenu->addSeparator();
    selectAllAction_ = editMenu->addAction(QStringLiteral("Select All"), this, &MainWindow::onEditSelectAll);
    selectAllAction_->setShortcuts(QKeySequence::SelectAll);
    // Cut/Copy need a selection; Paste needs somewhere to land. Kept in sync from
    // two places: selectionChanged (so a disabled action's shortcut correctly
    // doesn't fire even without the menu ever being opened) and aboutToShow (belt
    // and suspenders for anything else that could change either condition).
    connect(editMenu, &QMenu::aboutToShow, this, &MainWindow::updateEditActionsEnabled);

    auto *bookmarksMenu = menuBar()->addMenu(QStringLiteral("&Bookmarks"));
    addBookmarkAction_ = bookmarksMenu->addAction(QStringLiteral("Add Current Folder"), this, &MainWindow::onAddBookmark);

    auto *viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    refreshAction_ = viewMenu->addAction(QStringLiteral("Refresh"), this, &MainWindow::onRefresh);
    toggleSidePanelAction_ = viewMenu->addAction(QStringLiteral("Toggle Side Panel"), this, &MainWindow::onToggleSidePanel);
    viewMenu->addSeparator();
    hoverInfoAction_ = viewMenu->addAction(QStringLiteral("Show Hover Info"), this, &MainWindow::onToggleHoverInfo);
    hoverInfoAction_->setCheckable(true);
    hoverInfoAction_->setChecked(prefs::hoverInfoEnabled());

    // Not in any menu - see the member's own doc comment - just registered on the
    // window so its shortcut is live.
    focusAddressBarAction_ = new QAction(this);
    connect(focusAddressBarAction_, &QAction::triggered, this, &MainWindow::onFocusAddressBar);
    addAction(focusAddressBarAction_);

    applyKeyBindingShortcuts();

    // Tools always exists now (unconditionally, on both platforms) - Force
    // Re-thumbnail lives here rather than buried in the grid's right-click menu,
    // same reasoning as Refresh already living in View: a folder-wide, occasional
    // action belongs in a real menu, not a context menu meant for per-item actions
    // (Cut/Copy/Paste/View Fullscreen - see onGridContextMenu()).
    auto *toolsMenu = menuBar()->addMenu(QStringLiteral("&Tools"));
    toolsMenu->addAction(QStringLiteral("Force Re-thumbnail This Folder"), this, &MainWindow::onForceRethumbnail);
    toolsMenu->addAction(QStringLiteral("Purge Path History"), this, &MainWindow::onPurgePathHistory);

    // Qt's Cocoa menu bar relocates Preferences and About into the macOS *application* menu
    // (as Cmd-, and "About pixet"). So on Apple they're added to File, which still has
    // Choose Folder left in it after the move - this used to be the reason Tools was
    // Windows-only too (a Tools menu holding *only* Preferences would look empty once
    // Cocoa relocated it out), but that no longer applies now that Tools always has Force
    // Re-thumbnail in it regardless of platform.
    QAction *prefsAction = nullptr;
    QAction *aboutAction = nullptr;
#ifdef Q_OS_MACOS
    prefsAction = fileMenu->addAction(QStringLiteral("Preferences..."), this, &MainWindow::onPreferences);
    aboutAction = fileMenu->addAction(QStringLiteral("About pixet"), this, &MainWindow::onAbout);
#else
    prefsAction = toolsMenu->addAction(QStringLiteral("Preferences..."), this, &MainWindow::onPreferences);
    auto *helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    aboutAction = helpMenu->addAction(QStringLiteral("About pixet"), this, &MainWindow::onAbout);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("E&xit"), this, &MainWindow::close);
#endif
    // Set explicitly rather than leaning on Qt's text heuristic. The heuristic does work,
    // but it keys off the strings still containing "preferences"/"about", so renaming a menu
    // item would quietly move it back out of the application menu. There is deliberately no
    // QuitRole action: macOS supplies its own Quit in the application menu, and adding one
    // here would duplicate it.
    prefsAction->setMenuRole(QAction::PreferencesRole);
    aboutAction->setMenuRole(QAction::AboutRole);

    // Behind a build option rather than plain NDEBUG, because MainWindow.h's comment on
    // onCopyGridDebugInfo() is explicit that this should stay available on the daily-driver
    // *release* build - so it defaults ON everywhere and nothing changes for local use.
    // scripts/deploy-mac.sh configures with -DPIXET_DEBUG_MENU=OFF, so it doesn't ship in a
    // DMG handed to someone else. That's the "reconsider before any wider distribution" case
    // the same comment names, without removing the fast path it asks to keep permanently.
#ifdef PIXET_DEBUG_MENU
    auto *debugMenu = menuBar()->addMenu(QStringLiteral("&Debug"));
    debugMenu->addAction(QStringLiteral("Copy Grid Debug Info"), this, &MainWindow::onCopyGridDebugInfo);
#endif

    // Insurance for window/layout persistence. Everything used to be saved only from
    // closeEvent(), which is not guaranteed to run: on macOS, Cmd+Q and "Quit pixet" go
    // through the application menu and terminate without necessarily delivering a close
    // event to the window - so geometry, splitter sizes and the last directory would just
    // silently stop being remembered for the most common way of quitting. Saving twice is
    // harmless (same values, same keys).
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &MainWindow::saveWindowState);

    // Nominal pixel widths, sized generously for typical content - shrink together
    // proportionally (see StatusBarRow.h) if the window is too narrow for all of
    // them at once, rather than either overlapping (a QHBoxLayout of Qt::Fixed-
    // policy cells can't shrink them, so it has nothing left to do but position the
    // next cell at an X offset that assumes a smaller size than the previous one is
    // actually rendered at) or every cell collapsing to the same tiny floor
    // regardless of its own nominal width (what a QHBoxLayout of shrinkable cells
    // actually did here) - both confirmed live via real bugs, not hypothetical ones.
    auto *statusRow = new StatusBarRow(this);
    folderStatsLabel_ = statusRow->addLabel(300);
    rawStatusLabel_ = statusRow->addLabel(190);
    fileNameLabel_ = statusRow->addLabel(240);
    formatLabel_ = statusRow->addLabel(50);
    dimsLabel_ = statusRow->addLabel(150);
    sizeLabel_ = statusRow->addLabel(85);
    dateLabel_ = statusRow->addLabel(115);
    durationLabel_ = statusRow->addLabel(45);
    statusBar()->addPermanentWidget(statusRow);

    // Thumbnail size, at the far right where it's always reachable - the same setting also
    // lives in Preferences (as a free-form spinbox), and both funnel through
    // applyThumbnailSizeToGrid() so they can't drift apart.
    thumbSizeCombo_ = new QComboBox(this);
    thumbSizeCombo_->setToolTip(QStringLiteral("Thumbnail size"));
    thumbSizeCombo_->setFocusPolicy(Qt::NoFocus); // must not steal the grid's arrow keys
    syncThumbSizeCombo();
    connect(thumbSizeCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onThumbSizeChanged);
    statusBar()->addPermanentWidget(thumbSizeCombo_);

    thumbStatusButton_ = new QToolButton(this);
    thumbStatusButton_->setAutoRaise(true);
    thumbStatusButton_->setFocusPolicy(Qt::NoFocus);
    thumbStatusButton_->setText(QStringLiteral("●"));
    connect(thumbStatusButton_, &QToolButton::clicked, this, &MainWindow::onThumbStatusClicked);
    statusBar()->addPermanentWidget(thumbStatusButton_);
    updateThumbStatusIndicator();

    loadBookmarks();
    restoreLastDirectory();

    // So arrow keys work immediately on launch without clicking the grid first.
    // Requesting focus on a not-yet-shown widget is fine - Qt applies it once the
    // window actually becomes visible (show() runs after the constructor returns).
    grid_->setFocus();
}

MainWindow::~MainWindow() = default;

void MainWindow::navigateTo(const QString &path, bool forceReindex, bool forceRethumbnail) {
    QString normalized = normalizeForDb(path);
    if (normalized.isEmpty()) return;

    // A pending "select this file" (from navigateToInput()) only applies to the
    // navigation that requested it - a plain directory change (tree click, bookmark)
    // must not later resurrect a stale one.
    pendingSelectFileName_.clear();
    currentPath_ = normalized;
    navSettleTimer_.restart(); // bounds how long onTreeDirectoryLoaded() keeps chasing this row - see its member comment
    // Directory only, never a file path - `normalized` is always a resolved folder
    // here (navigateToInput() resolves a file path to its parent before ever
    // calling this), which is what keeps a pasted/typed file path out of history.
    prefs::addToPathHistory(normalized);
    refreshPathBarHistory();
    pathBar_->setCurrentText(normalized);
    // Skipped when a back/forward button is what's driving this call - the stack
    // already has this entry (that's the whole point of Back/Forward), so
    // re-recording it here would both be redundant and, worse, truncate the
    // "forward" entries a Back just moved away from.
    if (!navigatingViaHistory_) recordNavHistory(normalized);

    // Re-point the live watch (see folderWatcher_'s member comment) at the folder
    // now on screen. addPath() on a path already being watched, or removePaths()
    // on an empty list, are both harmless no-ops, so this doesn't need to special-
    // case "watching nothing yet" (first navigation) or "re-navigating to the same
    // folder".
    if (!folderWatcher_->directories().isEmpty()) folderWatcher_->removePaths(folderWatcher_->directories());
    folderWatcher_->addPath(normalized);
    preview_->clear();
    grid_->setCurrentFolderPath(normalized); // for the hover tooltip's on-demand EXIF read - see its doc comment
    // Show whatever's already cached instantly. For a folder that's new or stale,
    // this shows 0 rows - onFilesListed (fired once Pass A commits, see FolderIndexer)
    // reloads again with the real file list, well before thumbnails are ready.
    gridModel_->setDirectory(normalized);
    updateSelectionStatus(); // folder aggregates changed even before anything's selected

    QModelIndex idx = fsModel_->index(normalized);
    // Reveal this folder's own immediate children on every navigation, regardless of
    // how it was triggered (tree click, bookmark, path bar) - browsing into a folder
    // should show what's inside it in the tree without an extra manual expand click.
    if (idx.isValid()) tree_->expand(idx);

    if (idx.isValid() && tree_->currentIndex() != idx) {
        // This path only runs for navigation that didn't originate from a click
        // already inside the tree (bookmark click, restoreLastDirectory on startup) -
        // a direct tree click already has currentIndex() == idx by the time we get
        // here. Reveal every ancestor first so idx is actually part of the tree's
        // visible row structure (a collapsed parent means visualRect() below would
        // come back invalid).
        for (QModelIndex parent = idx.parent(); parent.isValid(); parent = parent.parent()) {
            if (!tree_->isExpanded(parent)) tree_->expand(parent);
        }

        tree_->setCurrentIndex(idx);

        // Deliberately not tree_->scrollTo(idx): its default EnsureVisible hint only
        // scrolls the minimum needed, and in the process resets horizontal scroll
        // back to the row's start - hiding whatever of a long name was scrolled into
        // view. Position vertically at the top instead; horizontal position untouched.
        // This first attempt is best-effort - expand() above kicks off asynchronous
        // directory listing, so most ancestors' full row counts (and therefore idx's
        // real position) usually aren't known yet. onTreeDirectoryLoaded() reapplies
        // this as each ancestor's listing finishes, but that alone wasn't enough for a
        // deeply nested path under a directory with many siblings (e.g. a home folder
        // full of app-config dirs) - the fixed-delay retries below are what actually
        // catch up in that case, empirically, up to a few seconds out.
        repositionTreeToTop(idx);
        for (int delayMs : {300, 800, 1500, 3000}) {
            QTimer::singleShot(delayMs, this, [this, normalized]() {
                if (normalized == currentPath_) repositionTreeToTop(fsModel_->index(normalized));
            });
        }
    }

    saveLastDirectory(normalized);
    emit requestIndex(normalized, forceReindex, forceRethumbnail);
    // Whatever RAW files here still need a full render jump ahead of any unrelated
    // backlog elsewhere in the library - see RawRenderer::prioritize().
    emit requestRawRenderPriority(normalized);
}

void MainWindow::repositionTreeToTop(const QModelIndex &idx) {
    if (!idx.isValid()) return;

    // If the row is already visible - or within a couple of rows of the viewport -
    // leave the scroll position alone. Jumping a folder that's already in view (say,
    // the middle of the tree) up to the top on every navigation is disorienting; only
    // reposition when the target genuinely isn't reachable without scrolling. An
    // invalid rect (row not yet part of the laid-out tree structure - still expanding
    // asynchronously) falls through to the repositioning logic below, same as before.
    QRect rowRect = tree_->visualRect(idx);
    if (rowRect.isValid()) {
        int rowHeight = rowRect.height() > 0 ? rowRect.height() : 20;
        QRect tolerance = tree_->viewport()->rect().adjusted(0, -3 * rowHeight, 0, 3 * rowHeight);
        if (tolerance.intersects(rowRect)) return;
    }

    // Trust Qt's own (tested, handles all the nested-expansion/row-height bookkeeping
    // correctly) positioning logic for the vertical part, rather than computing pixel
    // offsets by hand. Only manually intervene for the one specific side effect that's
    // actually a problem: it also resets horizontal scroll to the row's start, hiding
    // whatever of a long name was scrolled into view - so save/restore around it.
    int hScroll = tree_->horizontalScrollBar()->value();
    tree_->scrollTo(idx, QAbstractItemView::PositionAtTop);
    tree_->horizontalScrollBar()->setValue(hScroll);
    // Same class of bug as the thumbnail grid's partial-repaint issue: Qt's internal
    // scroll state ends up correct (verified via visualRect()/scrollbar value) but the
    // viewport doesn't reliably repaint to actually show it without this nudge.
    tree_->viewport()->update();
}

void MainWindow::onTreeDirectoryLoaded(const QString &) {
    // Fires for every directory the tree has ever listed, not just ones relevant to
    // the current navigation - cheap to just recheck unconditionally each time. Only
    // reposition while the tree's own selection still agrees with where we navigated
    // to (so this doesn't fight a selection the user has since changed manually) *and*
    // we're still within the post-navigation settle window - see navSettleTimer_'s
    // doc comment. Without that second check, expanding some unrelated folder (whose
    // currentIndex() never became the browsed one - clicking a branch's expand arrow
    // doesn't select the row) would still snap the view back to currentPath_ every
    // time, indefinitely.
    if (currentPath_.isEmpty()) return;
    if (!navSettleTimer_.isValid() || navSettleTimer_.elapsed() > kTreeSettleWindowMs) return;
    QModelIndex idx = fsModel_->index(currentPath_);
    if (idx.isValid() && tree_->currentIndex() == idx) repositionTreeToTop(idx);
}

void MainWindow::onNavigateFolderRequested(Qt::Key direction) {
    QModelIndex idx = fsModel_->index(currentPath_);
    if (!idx.isValid()) return;

    QModelIndex target;
    switch (direction) {
        case Qt::Key_Up: {
            QModelIndex parent = idx.parent();
            if (idx.row() > 0) target = fsModel_->index(idx.row() - 1, 0, parent);
            break;
        }
        case Qt::Key_Down:
            target = nextSiblingOrAunt(fsModel_, idx);
            break;
        case Qt::Key_Left:
            target = idx.parent();
            break;
        case Qt::Key_Right: {
            // fsModel_'s filter (see constructor) is dirs-only, so any row here is
            // genuinely a subfolder. The current folder is already expanded by the
            // time its thumbnails are on screen (navigateTo() does that), so its
            // children are normally already populated - a folder Ctrl+Right lands on
            // that was never expanded first just no-ops rather than fetching async.
            if (fsModel_->rowCount(idx) > 0) {
                target = fsModel_->index(0, 0, idx);
            } else {
                // Nothing to descend into - continue in tree order instead of just
                // stopping at a leaf, same as Ctrl+Down would from here.
                target = nextSiblingOrAunt(fsModel_, idx);
            }
            break;
        }
        default:
            return;
    }

    if (target.isValid()) navigateTo(fsModel_->filePath(target));
}

void MainWindow::recordNavHistory(const QString &path) {
    if (navHistoryIndex_ >= 0 && navHistoryIndex_ < navHistory_.size() && navHistory_[navHistoryIndex_] == path) {
        return; // re-navigating to the same spot (Refresh, re-clicking the current tree node, ...)
    }

    while (navHistory_.size() > navHistoryIndex_ + 1) navHistory_.removeLast();
    navHistory_.append(path);
    navHistoryIndex_ = navHistory_.size() - 1;
    updateNavButtonsEnabled();
}

void MainWindow::updateNavButtonsEnabled() {
    backAction_->setEnabled(navHistoryIndex_ > 0);
    forwardAction_->setEnabled(navHistoryIndex_ >= 0 && navHistoryIndex_ < navHistory_.size() - 1);
}

void MainWindow::onNavigateBack() {
    if (navHistoryIndex_ <= 0) return;
    --navHistoryIndex_;
    navigatingViaHistory_ = true;
    navigateTo(navHistory_[navHistoryIndex_]);
    navigatingViaHistory_ = false;
    updateNavButtonsEnabled();
}

void MainWindow::onNavigateForward() {
    if (navHistoryIndex_ < 0 || navHistoryIndex_ >= navHistory_.size() - 1) return;
    ++navHistoryIndex_;
    navigatingViaHistory_ = true;
    navigateTo(navHistory_[navHistoryIndex_]);
    navigatingViaHistory_ = false;
    updateNavButtonsEnabled();
}

void MainWindow::onTreeSelectionChanged(const QModelIndex &current) {
    QString path = fsModel_->filePath(current);
    if (!path.isEmpty() && normalizeForDb(path) != currentPath_) navigateTo(path);
}

void MainWindow::onBookmarkClicked(QListWidgetItem *item) {
    QString path = item->data(Qt::UserRole).toString();
    navigateTo(path);
}

void MainWindow::onBookmarksContextMenu(const QPoint &pos) {
    QListWidgetItem *item = bookmarks_->itemAt(pos);
    if (!item) return;

    QMenu menu(this);
    QAction *removeAction = menu.addAction(QStringLiteral("Remove Bookmark"));
    if (menu.exec(bookmarks_->mapToGlobal(pos)) == removeAction) {
        removeBookmark(item->data(Qt::UserRole + 1).toLongLong());
    }
}

void MainWindow::buildItemContextMenu(QMenu &menu, int row, bool fromFullscreen) {
    // Refresh and Force Re-thumbnail moved out to real menus (View and Tools
    // respectively) - this menu is for per-item actions on whatever's under the
    // cursor/selected, not folder-wide ones. Reuses the Edit menu's own
    // cutAction_/copyAction_/pasteAction_ QAction objects directly (Qt actions are
    // meant to be shared across multiple menus/toolbars - same shortcut text,
    // enabled state, and slot everywhere they appear, no duplicate wiring needed)
    // rather than building parallel entries here.
    addFileInfoToContextMenu(menu, row);

    if (!fromFullscreen) {
        QAction *fullscreenAction = menu.addAction(QStringLiteral("View Fullscreen"), this, [this]() {
            if (grid_->currentRow() >= 0) onGridItemActivated(grid_->currentRow());
        });
        fullscreenAction->setEnabled(grid_->currentRow() >= 0);
        menu.addSeparator();
    }

    // Safe to offer from the fullscreen viewer too: it keeps the grid's current row in step
    // with whatever it's showing (see the rowChanged connection in the constructor), so
    // these act on the image on screen.
    menu.addAction(cutAction_);
    menu.addAction(copyAction_);
    menu.addAction(pasteAction_);

    if (row >= 0) {
        menu.addSeparator();
        QString name = gridModel_->index(row).data(Qt::DisplayRole).toString();
        QString fullPath = joinPathQ(currentPath_, name);
        menu.addAction(QStringLiteral("Copy Name"), this,
                        [name]() { QGuiApplication::clipboard()->setText(name); });
        menu.addAction(QStringLiteral("Copy Path"), this,
                        [fullPath]() { QGuiApplication::clipboard()->setText(fullPath); });
    }
}

void MainWindow::onGridContextMenu(const QPoint &pos) {
    updateEditActionsEnabled();
    QMenu menu(this);
    buildItemContextMenu(menu, grid_->currentRow(), /*fromFullscreen=*/false);
    menu.exec(grid_->mapToGlobal(pos));
}

void MainWindow::onFullscreenContextMenu(int row, QPoint globalPos) {
    updateEditActionsEnabled();
    // Parented to the viewer, not to `this`: the viewer is a separate top-level window
    // sitting above the main one, and a menu parented to the window underneath can end up
    // rendered behind it.
    QMenu menu(fullscreenViewer_);
    buildItemContextMenu(menu, row, /*fromFullscreen=*/true);
    menu.exec(globalPos);
}

void MainWindow::addFileInfoToContextMenu(QMenu &menu, int row) {
    if (row < 0) return;

    for (const QString &line : grid_->cachedInfoText(row).split(QLatin1Char('\n'))) {
        QAction *a = menu.addAction(line);
        a->setEnabled(false);
    }

    QModelIndex idx = gridModel_->index(row);
    int fmt = idx.data(ThumbGridModel::FormatRole).toInt();
    QString path = joinPathQ(currentPath_, idx.data(Qt::DisplayRole).toString());

    // Synchronous, unlike the hover tooltip's worker-thread fetch - see
    // hoverinfo::readExifDetailsSync's own doc comment on why that tradeoff is fine
    // for a right-click's single, explicitly-requested file.
    pixet::ExifDetails details = hoverinfo::readExifDetailsSync(path, fmt);
    QString exifText = hoverinfo::formatExifDetails(details);
    if (!exifText.isEmpty()) {
        for (const QString &line : exifText.split(QLatin1Char('\n'))) {
            QAction *a = menu.addAction(line);
            a->setEnabled(false);
        }
    }

    if (details.hasGps) {
        // Plain "lat,lon" decimal degrees - the format Google Maps' own search box
        // accepts pasted directly, no need for a maps.google.com URL.
        QString coords = QStringLiteral("%1,%2").arg(details.gpsLatitude, 0, 'f', 6).arg(details.gpsLongitude, 0, 'f', 6);
        menu.addAction(QStringLiteral("Copy GPS Coordinates (%1)").arg(coords), this,
                        [coords]() { QGuiApplication::clipboard()->setText(coords); });
    }

    menu.addSeparator();
}

void MainWindow::onFilesDroppedOnGrid(QStringList localPaths, bool move) {
    if (currentPath_.isEmpty() || localPaths.isEmpty()) return;

    FileOpsWorker::Request req;
    req.id = ++fileOpCounter_;
    req.move = move;
    req.dstDirPath = currentPath_;
    for (const QString &path : localPaths) {
        FileOpsWorker::Item item;
        item.srcPath = path;
        // srcFileId/srcDirId stay 0 - these are external files, not rows pixet's
        // index already knows about.
        req.items << item;
    }
    emit requestFileOpPreflight(req);
}

void MainWindow::onFileOpPreflightReady(FileOpsWorker::Request req, QStringList rejected) {
    if (!rejected.isEmpty()) {
        statusBar()->showMessage(
            QStringLiteral("%1 item(s) skipped (folders aren't supported yet, or the file is gone)")
                .arg(rejected.size()),
            6000);
    }
    if (req.items.isEmpty()) {
        pendingCutClipboardClear_ = false; // nothing will reach onFileOpFinished() to consume this
        return;
    }

    int totalConflicts = 0;
    for (const auto &item : req.items) {
        if (item.hasConflict) totalConflicts++;
    }

    // One dialog per conflict, resolved entirely here before any I/O starts (see
    // FileOpsWorker's two-stage preflight/execute protocol) - "apply to all
    // remaining" is honored by not asking again for the rest of this loop.
    bool applyToAll = false;
    CollisionDialog::Choice appliedChoice = CollisionDialog::Skip;
    int conflictsSeen = 0;

    for (int i = 0; i < req.items.size(); ++i) {
        FileOpsWorker::Item &item = req.items[i];
        if (!item.hasConflict) continue;

        CollisionDialog::Choice choice;
        if (applyToAll) {
            choice = appliedChoice;
        } else {
            int64_t srcSize = 0, srcMtime = 0;
            // item.conflictSize/conflictMtime describe the *existing destination*
            // file (set by FileOpsWorker::preflight) - the dialog's "Incoming" line
            // needs the source's own stat.
            pixet::statFile(item.srcPath.toStdString(), &srcSize, &srcMtime);
            int remaining = totalConflicts - conflictsSeen - 1;
            bool checkedApplyToAll = false;
            choice = CollisionDialog::ask(this, item.dstName, req.dstDirPath, srcSize, srcMtime, item.conflictSize,
                                          item.conflictMtime, remaining, &checkedApplyToAll);
            if (choice == CollisionDialog::CancelAll) {
                // Cancelling must not clear a Cut clipboard - nothing was actually
                // moved, so the user's cut selection should still be pasteable.
                pendingCutClipboardClear_ = false;
                statusBar()->showMessage(QStringLiteral("File operation cancelled"), 4000);
                return;
            }
            if (checkedApplyToAll) {
                applyToAll = true;
                appliedChoice = choice;
            }
        }
        conflictsSeen++;

        switch (choice) {
            case CollisionDialog::Replace:
                item.resolution = FileOpsWorker::Collision::Replace;
                break;
            case CollisionDialog::Skip:
                item.resolution = FileOpsWorker::Collision::Skip;
                break;
            case CollisionDialog::KeepBoth:
                item.resolution = FileOpsWorker::Collision::KeepBoth;
                break;
            case CollisionDialog::CancelAll:
                break; // unreachable - handled above
        }
    }

    emit requestFileOpExecute(req);
}

void MainWindow::onFileOpProgress(quint64, int done, int total, QString currentName) {
    statusBar()->showMessage(QStringLiteral("%1 / %2 - %3").arg(done).arg(total).arg(currentName));
}

void MainWindow::onFileOpFinished(quint64, QString dstDirPath, QList<qint64> srcFileIds, QStringList addedNames,
                                   int succeeded, int failed, QStringList errors) {
    if (pendingCutClipboardClear_) {
        pendingCutClipboardClear_ = false;
        // Matches Explorer's own behavior of clearing the clipboard once a
        // cut-paste actually completes, so a second Ctrl+V can't try to move
        // already-moved files.
        clipops::clearAfterCutPaste();
    }

    if (dstDirPath == currentPath_) {
        // Safe even for ids that aren't currently loaded (a no-op) - see the
        // signal's doc comment on FileOpsWorker::finished.
        for (qint64 id : srcFileIds) gridModel_->removeFileById(id);

        // Two passes, deliberately not one: insertOrUpdateFileByName()'s return
        // value is only that row's index *at the moment of that one insertion* -
        // inserting "a.jpg" after "b.jpg" shifts "b.jpg" up by one, silently
        // invalidating an index already collected for it. Re-deriving every index
        // fresh via rowForName() only after every insert has landed (rowByName_ is
        // fully rebuilt after each one - see reindexLookups()) is what keeps a
        // multi-file drop from ending up with some of newRows pointing at the wrong
        // row, or a duplicate row, once shifting is accounted for.
        for (const QString &name : addedNames) gridModel_->insertOrUpdateFileByName(name);
        QList<int> newRows;
        for (const QString &name : addedNames) {
            int r = gridModel_->rowForName(name);
            if (r >= 0) newRows << r;
        }
        if (!newRows.isEmpty()) grid_->setSelection(newRows, newRows.last());
        updateSelectionStatus();

        // The new rows are already committed to the DB (state=New) - a plain
        // non-forced index is enough to pick up thumbnails. Pass B runs
        // unconditionally after Indexer's freshness check rather than only inside
        // it, so this never triggers a wasted full re-diff of the rest of the folder.
        emit requestIndex(currentPath_, /*force=*/false, /*forceRethumbnail=*/false);
    } else if (!gridModel_->hasDirectory()) {
        reloadGridPreservingSelection();
    }

    if (failed > 0) {
        statusBar()->showMessage(QStringLiteral("%1 succeeded, %2 failed").arg(succeeded).arg(failed), 8000);
        QMessageBox::warning(this, QStringLiteral("Some items failed"),
                              errors.size() <= 5 ? errors.join(QStringLiteral("\n"))
                                                  : errors.mid(0, 5).join(QStringLiteral("\n")) +
                                                        QStringLiteral("\n...and %1 more").arg(errors.size() - 5));
    } else if (succeeded > 0) {
        statusBar()->showMessage(QStringLiteral("%1 item(s) added").arg(succeeded), 4000);
    }
}

void MainWindow::onDragOutRequested() {
    if (currentPath_.isEmpty()) return;

    QStringList paths;
    for (int r : grid_->selectedRows()) {
        QString name = gridModel_->index(r).data(Qt::DisplayRole).toString();
        paths << joinPathQ(currentPath_, name);
    }
    if (paths.isEmpty()) return;

    QList<QUrl> urls;
    urls.reserve(paths.size());
    for (const QString &p : paths) urls << QUrl::fromLocalFile(p);

    auto *mime = new QMimeData;
    mime->setUrls(urls);
    mime->setText(paths.join(QLatin1Char('\n')));
    clipops::markPreferMove(mime);

    auto *drag = new QDrag(grid_); // Qt deletes it once the drag finishes
    drag->setMimeData(mime);

    QModelIndex leadIdx = gridModel_->index(grid_->currentRow());
    QVariant deco = leadIdx.data(Qt::DecorationRole);
    if (deco.canConvert<QPixmap>()) {
        QPixmap pix = deco.value<QPixmap>();
        if (!pix.isNull()) {
            QPixmap scaled = pix.scaled(QSize(96, 96), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            drag->setPixmap(scaled);
            drag->setHotSpot(QPoint(scaled.width() / 2, scaled.height() / 2));
        }
    }

    // Disclosed limitation, not a bug to chase: pixet does not perform the physical
    // move itself here - Explorer/Finder does, as the drop target, and the actual
    // copy-vs-move outcome remains governed by the OS's own conventions at the drop
    // side (same-volume vs. cross-volume, Ctrl/Shift overrides). markPreferMove()
    // and Qt::MoveAction below are hints a target is free to ignore.
    Qt::DropAction result = drag->exec(Qt::CopyAction | Qt::MoveAction, Qt::MoveAction);
    if (result == Qt::MoveAction) {
        // Explorer/Finder already performed the move; pixet only has to notice.
        // Reusing the already-tested by-name diff (a forced non-recursive rescan)
        // is both correct and simpler than independently guessing which of these
        // specific files are now gone - and it can't accidentally delete rows for
        // files that are still there. Feeds into onFilesListed() ->
        // reloadGridPreservingSelection(), so this doesn't reintroduce
        // scroll-position loss. Never gets the thumb_id-preservation win drag-out
        // (unlike Cut/Paste and drag-in) - pixet never learns the destination path.
        emit requestIndex(currentPath_, /*force=*/true, false);
    }
}

void MainWindow::onGridItemActivated(int row) {
    if (row < 0) return;
    QModelIndex index = gridModel_->index(row);

    // Video has no playback engine in the fullscreen viewer (poster frame only, see
    // FullscreenViewer's class comment) - activating one launches an actual player
    // instead, per the user's own preference, rather than opening the viewer at all.
    if ((pixet::Format)index.data(ThumbGridModel::FormatRole).toInt() == pixet::Format::Video) {
        QString filePath = joinPathQ(currentPath_, index.data(Qt::DisplayRole).toString());
        if (prefs::useSystemVideoPlayer() || prefs::customVideoPlayerPath().isEmpty()) {
            // Also the fallback when "Custom" is selected but no path was ever set -
            // silently doing nothing on activation would look like the double-click
            // just didn't register.
            QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
        } else {
            const QString player = prefs::customVideoPlayerPath();
            bool started = false;
            if (player.endsWith(QStringLiteral(".app"), Qt::CaseInsensitive) || QFileInfo(player).isDir()) {
                // A macOS "application" the user picks in a file dialog is an .app bundle,
                // i.e. a *directory* - startDetached() can't exec it, and the native open
                // panel hands one back happily since it treats bundles as selectable files.
                // `open -a` is the supported way to hand a document to a bundled app.
                started = QProcess::startDetached(QStringLiteral("/usr/bin/open"),
                                                   {QStringLiteral("-a"), player, filePath});
            } else {
                started = QProcess::startDetached(player, {filePath});
            }
            // startDetached's return value used to be discarded, so a misconfigured player
            // path made a double-click do nothing at all with no indication why - the same
            // failure mode the empty-path fallback above was already written to avoid.
            if (!started) {
                statusBar()->showMessage(
                    QStringLiteral("Could not launch video player: %1  (check Preferences)").arg(player), 8000);
            }
        }
        return;
    }

    fullscreenViewer_->openAt(gridModel_, currentPath_, row);
}

void MainWindow::onPathBarReturnPressed() { navigateToInput(pathBar_->currentText()); }

void MainWindow::onPathBarHistoryActivated() { navigateToInput(pathBar_->currentText()); }

void MainWindow::onGridSelectionChanged() {
    currentPreviewBpp_ = 0; // stale from whatever was selected before - cleared until this item's preview lands
    updateSelectionStatus();
    // Same class of unreliable-partial-repaint issue as thumbnail loading (see the
    // thumbReady connection below) - Qt's internal old/new-current-item rect updates
    // aren't always enough to actually repaint the selection border, most noticeably
    // when moving the selection with arrow keys. Cheap and coalesced, same reasoning
    // as elsewhere in this file.
    grid_->viewport()->update();

    QModelIndex idx = gridModel_->index(grid_->currentRow());
    if (!idx.isValid()) {
        pendingPreviewPath_.clear();
        pendingPreviewIsCtrlHover_ = false;
        preview_->clear();
        pathBar_->setCurrentText(currentPath_);
        return;
    }

    QString name = idx.data(Qt::DisplayRole).toString();
    pendingPreviewFmt_ = idx.data(ThumbGridModel::FormatRole).toInt();
    pendingPreviewPath_ = joinPathQ(currentPath_, name);
    pendingPreviewIsCtrlHover_ = false;
    // Display only - the full file path never gets recorded into history (see
    // prefs::pathHistory()'s doc comment). setCurrentText() on an editable combo
    // just sets the edit line's text; it doesn't need to match (or become) a
    // dropdown item.
    pathBar_->setCurrentText(pendingPreviewPath_);

    previewDebounce_->start();
}

void MainWindow::onGridCtrlHoverChanged(int row) {
    if (row < 0) {
        // Ctrl released / mouse left the grid / hovered empty space - go back to
        // showing whatever's actually selected. Reuses onGridSelectionChanged()
        // wholesale rather than a separate revert path: the real selection never
        // changed, so recomputing its status-bar/path-bar/viewport state is
        // redundant but harmless, and this way there's exactly one place that knows
        // how to derive "what should the preview show" from the actual selection.
        onGridSelectionChanged();
        return;
    }

    QModelIndex idx = gridModel_->index(row);
    if (!idx.isValid()) return;

    // Deliberately skips onGridSelectionChanged()'s other side effects (status bar,
    // path bar text, viewport repaint) - the actual selection hasn't changed, only
    // the preview pane should react to a Ctrl-hover peek.
    pendingPreviewFmt_ = idx.data(ThumbGridModel::FormatRole).toInt();
    pendingPreviewPath_ = joinPathQ(currentPath_, idx.data(Qt::DisplayRole).toString());
    pendingPreviewIsCtrlHover_ = true;
    previewDebounce_->start();
}

void MainWindow::triggerPreviewRequest() {
    if (pendingPreviewPath_.isEmpty()) return;
    currentPreviewRequestId_ = ++previewRequestCounter_;
    currentPreviewIsCtrlHover_ = pendingPreviewIsCtrlHover_;
    previewDecoder_->requestPreview(currentPreviewRequestId_, pendingPreviewPath_, pendingPreviewFmt_,
                                     preview_->preferredTargetLongEdge());
}

void MainWindow::onPreviewReady(qint64 requestId, QImage image) {
    if (requestId != currentPreviewRequestId_) return; // superseded by a newer selection
    preview_->setImage(image);
    if (currentPreviewIsCtrlHover_) return; // a hover peek, not the real selection - see the member comment
    // QImage::depth() (bits per pixel, e.g. 24 for 8-bit/channel RGB) is the only place
    // this app has bit-depth info without adding a DB column - see the member comment.
    currentPreviewBpp_ = image.isNull() ? 0 : image.depth();
    updateSelectionStatus();
}

void MainWindow::onAddBookmark() {
    if (!currentPath_.isEmpty()) addBookmark(currentPath_);
}

bool MainWindow::isBookmarked(const QString &path) const {
    if (!db_ || path.isEmpty()) return false;
    auto sel = db_->prepare("SELECT 1 FROM bookmarks WHERE path=? LIMIT 1");
    sel.bind(1, path.toStdString());
    return sel.step();
}

void MainWindow::onTreeContextMenu(const QPoint &pos) {
    // customContextMenuRequested reports `pos` in the *view's* coordinates (the policy is set
    // on tree_, so tree_ is what receives the event), but indexAt() measures from the
    // viewport - which is inset by the frame border. Converting rather than passing `pos`
    // straight through is what keeps a click near a row boundary from resolving to the row
    // above it.
    const QPoint viewportPos = tree_->viewport()->mapFrom(tree_, pos);
    QModelIndex idx = tree_->indexAt(viewportPos);
    if (!idx.isValid()) return; // right-click on empty space below the tree

    // Normalized the same way every other path that reaches the DB is, so the
    // already-bookmarked check compares like with like - QFileSystemModel hands back
    // forward slashes on every platform, and on macOS a different Unicode normalization
    // form than the DB stores.
    const QString path = normalizeForDb(fsModel_->filePath(idx));
    if (path.isEmpty()) return;

    QMenu menu(this);
    // Leading disabled entry naming the target, matching the grid's own context menu - the
    // right-clicked folder isn't necessarily the selected one, so saying which folder this
    // is about is worth a line.
    QAction *header = menu.addAction(QFileInfo(path).fileName().isEmpty() ? path : QFileInfo(path).fileName());
    header->setEnabled(false);
    menu.addSeparator();

    if (isBookmarked(path)) {
        QAction *already = menu.addAction(QStringLiteral("Already in Bookmarks"));
        already->setEnabled(false);
    } else {
        menu.addAction(QStringLiteral("Add to Bookmarks"), this, [this, path]() { addBookmark(path); });
    }

    menu.exec(tree_->mapToGlobal(pos)); // `pos` is view-relative, so map from the view
}

void MainWindow::onFocusAddressBar() {
    pathBar_->setFocus(Qt::ShortcutFocusReason);
    if (QLineEdit *le = pathBar_->lineEdit()) le->selectAll();
}

void MainWindow::onRefresh() {
    if (!currentPath_.isEmpty()) navigateTo(currentPath_, /*forceReindex=*/true);
}

void MainWindow::onForceRethumbnail() {
    if (!currentPath_.isEmpty()) navigateTo(currentPath_, /*forceReindex=*/true, /*forceRethumbnail=*/true);
}

void MainWindow::syncThumbSizeCombo() {
    if (!thumbSizeCombo_) return;
    const int current = prefs::thumbnailIconSize();

    // Shared with the Preferences dialog's own size list - see prefs::thumbnailSizeChoices().
    const QList<int> sizes = prefs::thumbnailSizeChoices(current);

    // Rebuilding fires currentIndexChanged, which would look exactly like the user picking
    // an entry and trigger a re-thumbnail.
    QSignalBlocker blocker(thumbSizeCombo_);
    thumbSizeCombo_->clear();
    for (int px : sizes) thumbSizeCombo_->addItem(QStringLiteral("%1 px").arg(px), px);
    thumbSizeCombo_->setCurrentIndex((int)sizes.indexOf(current));
}

int MainWindow::displayThumbLongEdge() const {
    return qMax(1, qRound(prefs::thumbnailIconSize() * devicePixelRatioF()));
}

int MainWindow::countStaleThumbnails(const QString &dirPath) const {
    if (!db_ || dirPath.isEmpty()) return 0;

    auto dirSel = db_->prepare("SELECT id FROM dirs WHERE path=?");
    dirSel.bind(1, dirPath.toStdString());
    if (!dirSel.step()) return 0; // folder not indexed yet - nothing to be stale
    int64_t dirId = dirSel.columnInt64(0);

    // Judged against what the *display* needs, not against prefs::thumbnailTargetLongEdge()
    // (which is deliberately ~2x for headroom). Using the generation target would light this
    // red whenever a folder happened to be indexed under a smaller setting, even when its
    // blobs are perfectly adequate for what's on screen - blobs stored at 320 shown at 240
    // look fine and shouldn't nag.
    //
    // MIN(needed, original long edge) is what stops a legitimately small photo from counting
    // as stale forever: a 200px original can never produce a 480px thumbnail, so it isn't
    // behind - it's already as good as it will ever get. files.width/height are the
    // ORIGINAL dimensions (thumbs.thumbs.w/h are the thumbnail's), which is exactly the
    // distinction this relies on.
    auto sel = db_->prepare(
        "SELECT COUNT(*) FROM files f JOIN thumbs.thumbs t ON t.id = f.thumb_id "
        "WHERE f.dir_id = ? AND MAX(t.w, t.h) < MIN(?, MAX(f.width, f.height))");
    sel.bind(1, dirId);
    sel.bind(2, (int64_t)displayThumbLongEdge());
    if (!sel.step()) return 0;
    return (int)sel.columnInt64(0);
}

void MainWindow::updateThumbStatusIndicator() {
    if (!thumbStatusButton_) return;

    const int stale = countStaleThumbnails(currentPath_);
    const bool upToDate = (stale == 0);

    // "Auto rethumb" (Preferences > Maintenance): regenerate on arrival instead of waiting
    // to be clicked. Guarded on the path so it fires once per folder - the re-thumbnail
    // navigates back to this same folder, whose completion calls straight back into here,
    // and without the guard a folder that somehow stayed stale would loop forever.
    if (!upToDate && prefs::autoRethumbnail() && autoRethumbPath_ != currentPath_) {
        autoRethumbPath_ = currentPath_;
        onForceRethumbnail();
        return; // the indicator gets refreshed again when that pass reports back
    }
    // Colour via stylesheet on a text bullet rather than an icon, matching the "×" reset
    // buttons in PreferencesDialog - it scales with the UI font instead of needing a pixmap
    // per device pixel ratio.
    thumbStatusButton_->setStyleSheet(upToDate ? QStringLiteral("QToolButton { color: #3fb950; }")
                                                : QStringLiteral("QToolButton { color: #f85149; }"));
    thumbStatusButton_->setToolTip(
        upToDate ? QStringLiteral("Thumbnails here are up to date for this size.\nClick to regenerate anyway.")
                  : QStringLiteral("%1 thumbnail(s) here were made for a smaller size and will look soft.\n"
                                    "Click to regenerate this folder.")
                        .arg(stale));
}

void MainWindow::applyThumbnailSizeToGrid() {
    const int size = prefs::thumbnailIconSize();
    grid_->setIconSize(QSize(size, size)); // recomputes the grid layout itself

    if (currentPath_.isEmpty()) {
        updateThumbStatusIndicator();
        return;
    }

    // Only pay for a re-thumbnail when the blobs on disk genuinely can't satisfy the new
    // size. Shrinking never needs one (the stored thumbnails are already larger), and nor
    // does an increase still covered by what's stored - both stay instant, and the reload
    // below is enough because already-decoded pixmaps in the model were sized for the *old*
    // icon size and have to be re-requested from ThumbLoader regardless.
    if (countStaleThumbnails(currentPath_) > 0) {
        navigateTo(currentPath_, /*forceReindex=*/true, /*forceRethumbnail=*/true);
    } else {
        reloadGridPreservingSelection();
    }
    updateThumbStatusIndicator();
}

void MainWindow::onThumbSizeChanged() {
    if (!thumbSizeCombo_) return;
    const int size = thumbSizeCombo_->currentData().toInt();
    if (size <= 0 || size == prefs::thumbnailIconSize()) return;
    prefs::setThumbnailIconSize(size);
    applyThumbnailSizeToGrid();
}

void MainWindow::onThumbStatusClicked() {
    // Deliberately works when green too: that's a force-regenerate, the same action as the
    // grid context menu's "Force Re-thumbnail This Folder", in the place the dot has just
    // drawn attention to.
    onForceRethumbnail();
}

void MainWindow::onPreferences() {
    PreferencesDialog dlg(this);
    connect(&dlg, &PreferencesDialog::reindexRequested, this, [this]() { emit requestFullReindex(); });
    connect(&dlg, &PreferencesDialog::thumbnailSizeChanged, this, [this]() {
        // Same path the status-bar drop-down takes, so the two controls can't behave
        // differently for the same setting - including the "only re-thumbnail when the
        // stored blobs are actually too small" rule.
        syncThumbSizeCombo();
        applyThumbnailSizeToGrid();
    });
    connect(&dlg, &PreferencesDialog::databaseStatsRequested, this, [this, &dlg]() {
        // Parented to the Preferences dialog it was opened from, so it centres on that
        // rather than on the main window behind it. Safe to capture &dlg: this connection
        // only ever fires while dlg.exec() below is running.
        DatabaseStatsDialog stats(*db_, &dlg);
        stats.exec();
    });
    connect(&dlg, &PreferencesDialog::nukeDatabaseRequested, this, &MainWindow::nukeDatabase);
    dlg.exec();
    // Cheap enough to always re-apply regardless of whether the keybindings editor
    // actually changed anything (Cancel discards its own edits before this point) -
    // no need for a dedicated changed-detection signal just for this.
    applyKeyBindingShortcuts();
}

void MainWindow::onChooseFolder() {
    QString start = currentPath_.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
                                            : currentPath_;
    QString chosen = QFileDialog::getExistingDirectory(this, QStringLiteral("Choose Folder"), start);
    if (!chosen.isEmpty()) navigateTo(normalizeForDb(chosen));
}

void MainWindow::onAbout() {
    // QMessageBox::about renders as a proper About panel in the application menu on macOS.
    // Qt's runtime version is worth showing: the AGL link workaround in src/app/CMakeLists.txt
    // is specific to 6.8, so knowing which Qt a given build was made against is the first
    // thing anyone would want when it eventually stops being needed.
    QMessageBox::about(this, QStringLiteral("About pixet"),
                        QStringLiteral("<b>pixet %1</b><br><br>Photo and video viewer.<br><br>"
                                        "Qt %2<br>Cache: %3")
                            .arg(QString::fromLatin1(pixet::version()), QString::fromLatin1(qVersion()),
                                  QString::fromStdString(pixet::appDataDir())));
}

void MainWindow::openSystemPath(const QString &path) {
    if (path.isEmpty()) return;
    // The request came from outside the app (Finder, the Dock, `open -a`), so the window is
    // very likely behind something or freshly launched - bring it forward, or the file
    // silently opens into a window the user can't see.
    show();
    raise();
    activateWindow();
    navigateToInput(path);
}

void MainWindow::applyKeyBindingShortcuts() {
    refreshAction_->setShortcut(keybindings::binding(keybindings::Action::Refresh));
    toggleSidePanelAction_->setShortcut(keybindings::binding(keybindings::Action::ToggleSidePanel));
    addBookmarkAction_->setShortcut(keybindings::binding(keybindings::Action::AddBookmark));
    focusAddressBarAction_->setShortcut(keybindings::binding(keybindings::Action::FocusAddressBar));
}

QLineEdit *MainWindow::focusedLineEdit() const { return qobject_cast<QLineEdit *>(QApplication::focusWidget()); }

void MainWindow::onEditSelectAll() {
    if (QLineEdit *le = focusedLineEdit()) {
        le->selectAll();
        return;
    }
    grid_->selectAll();
}

void MainWindow::onEditCopy() {
    if (QLineEdit *le = focusedLineEdit()) {
        le->copy();
        return;
    }
    QStringList paths;
    for (int r : grid_->selectedRows()) {
        QString name = gridModel_->index(r).data(Qt::DisplayRole).toString();
        paths << joinPathQ(currentPath_, name);
    }
    if (paths.isEmpty()) return;
    clipops::writeFiles(paths, /*cut=*/false);
    statusBar()->showMessage(QStringLiteral("%1 item(s) copied").arg(paths.size()), 3000);
}

void MainWindow::onEditCut() {
    if (QLineEdit *le = focusedLineEdit()) {
        le->cut();
        return;
    }
    QStringList paths;
    for (int r : grid_->selectedRows()) {
        QString name = gridModel_->index(r).data(Qt::DisplayRole).toString();
        paths << joinPathQ(currentPath_, name);
    }
    if (paths.isEmpty()) return;
    // No I/O here - Cut only marks the clipboard. The actual move happens on
    // Paste, exactly like Explorer's own Ctrl+X.
    clipops::writeFiles(paths, /*cut=*/true);
    statusBar()->showMessage(QStringLiteral("%1 item(s) cut").arg(paths.size()), 3000);
}

void MainWindow::onEditPaste() {
    if (QLineEdit *le = focusedLineEdit()) {
        le->paste();
        return;
    }
    if (currentPath_.isEmpty()) return;

    clipops::Files files = clipops::read();
    if (files.paths.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Nothing to paste"), 3000);
        return;
    }

    FileOpsWorker::Request req;
    req.id = ++fileOpCounter_;
    req.move = files.isCut;
    req.dstDirPath = currentPath_;
    for (const QString &path : files.paths) {
        FileOpsWorker::Item item;
        item.srcPath = path;

        // If this happens to be a file pixet's own index already knows about (e.g.
        // Cut in one pixet-browsed folder, Paste in another), populate srcFileId/
        // srcDirId so the smart move (see fileops::execute()) can preserve its
        // thumbnail rather than re-thumbnailing from scratch - the same lookup
        // navigateTo() already does to turn a filesystem path into a DB row.
        QString normalized = normalizeForDb(path);
        QFileInfo info(normalized);
        auto dirSel = db_->prepare("SELECT id FROM dirs WHERE path=?");
        dirSel.bind(1, normalizeForDb(info.absolutePath()).toStdString());
        if (dirSel.step()) {
            int64_t dirId = dirSel.columnInt64(0);
            auto fileSel = db_->prepare("SELECT id FROM files WHERE dir_id=? AND name=?");
            fileSel.bind(1, dirId);
            fileSel.bind(2, info.fileName().toStdString());
            if (fileSel.step()) {
                item.srcFileId = fileSel.columnInt64(0);
                item.srcDirId = dirId;
            }
        }

        req.items << item;
    }

    pendingCutClipboardClear_ = files.isCut;
    emit requestFileOpPreflight(req);
}

void MainWindow::onToggleSidePanel() {
    // QSplitter automatically gives a hidden child's space to the remaining visible
    // one(s) - grid_ expands to fill the whole width - and ThumbGridView already
    // recomputes its column count on any resize (see relayout()), so hiding/showing
    // leftPanel_ handles the layout itself. But the scroll position is a raw pixel
    // offset, not row-aware - the same offset lands on a completely different set of
    // rows once the column count changes (e.g. 3-wide -> 6-wide), so the current
    // selection can end up scrolled off screen. Re-center on it explicitly, deferred
    // to the next event loop turn since QSplitter's redistribution is a posted
    // layout request rather than synchronous - scrolling immediately here would
    // still measure against the pre-toggle viewport width/column count.
    leftPanel_->setVisible(!leftPanel_->isVisible());
    int row = grid_->currentRow();
    if (row >= 0) QTimer::singleShot(0, grid_, [this, row]() { grid_->scrollToRow(row, /*center=*/true); });
}

void MainWindow::onToggleHoverInfo(bool enabled) {
    prefs::setHoverInfoEnabled(enabled);
    if (!enabled) grid_->hideHoverTooltip();
}

void MainWindow::onCopyGridDebugInfo() {
    QScreen *scr = screen();
    auto rectStr = [](const QRect &r) { return QStringLiteral("%1,%2 %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height()); };
    auto sizeStr = [](const QSize &s) { return QStringLiteral("%1x%2").arg(s.width()).arg(s.height()); };
    auto sizesStr = [](const QList<int> &sizes) {
        QStringList parts;
        for (int s : sizes) parts << QString::number(s);
        return parts.join(QStringLiteral(", "));
    };

    QStringList lines;
    lines << QStringLiteral("=== pixet grid debug info ===");
    lines << QStringLiteral("");
    lines << QStringLiteral("Window:");
    lines << QStringLiteral("  isMaximized: %1").arg(isMaximized() ? "true" : "false");
    lines << QStringLiteral("  isFullScreen: %1").arg(isFullScreen() ? "true" : "false");
    lines << QStringLiteral("  windowState: 0x%1").arg((int)windowState(), 0, 16);
    lines << QStringLiteral("  geometry: %1").arg(rectStr(geometry()));
    lines << QStringLiteral("  frameGeometry: %1").arg(rectStr(frameGeometry()));
    lines << QStringLiteral("  devicePixelRatio: %1").arg(devicePixelRatioF());
    if (scr) {
        lines << QStringLiteral("  screen: %1").arg(scr->name());
        lines << QStringLiteral("  screen geometry: %1").arg(rectStr(scr->geometry()));
        lines << QStringLiteral("  screen devicePixelRatio: %1").arg(scr->devicePixelRatio());
        lines << QStringLiteral("  screen logicalDotsPerInchX: %1").arg(scr->logicalDotsPerInchX());
    }
    lines << QStringLiteral("");
    lines << QStringLiteral("Splitters:");
    lines << QStringLiteral("  mainSplitter sizes: [%1]").arg(sizesStr(splitter_->sizes()));
    lines << QStringLiteral("  topSplitter sizes: [%1]").arg(sizesStr(topSplitter_->sizes()));
    lines << QStringLiteral("  leftSplitter sizes: [%1]").arg(sizesStr(leftSplitter_->sizes()));
    lines << QStringLiteral("  leftPanel visible: %1").arg(leftPanel_->isVisible() ? "true" : "false");
    lines << QStringLiteral("  leftPanel size: %1").arg(sizeStr(leftPanel_->size()));
    lines << QStringLiteral("");
    lines << QStringLiteral("Grid:");
    lines << QStringLiteral("  grid_ size: %1").arg(sizeStr(grid_->size()));
    lines << QStringLiteral("  grid_ viewport size: %1").arg(sizeStr(grid_->viewport()->size()));
    lines << QStringLiteral("  grid_ frameWidth: %1").arg(grid_->frameWidth());
    lines << QStringLiteral("  verticalScrollBar visible: %1, width: %2")
                 .arg(grid_->verticalScrollBar()->isVisible() ? "true" : "false")
                 .arg(grid_->verticalScrollBar()->width());
    lines << QStringLiteral("  cell size: %1").arg(sizeStr(grid_->debugCellSize()));
    lines << QStringLiteral("  iconSize(): %1").arg(sizeStr(grid_->iconSize()));
    lines << QStringLiteral("  computed columns: %1").arg(grid_->debugComputedColumns());
    lines << QStringLiteral("  actually rendered columns: %1 (always == computed now - custom-painted, not QListView IconMode)")
                 .arg(grid_->debugRenderedColumnCount());
    lines << QStringLiteral("  model row count: %1").arg(gridModel_->rowCount());
    lines << QStringLiteral("");
    lines << QStringLiteral("Preferences:");
    lines << QStringLiteral("  thumbnailIconSize: %1").arg(prefs::thumbnailIconSize());
    lines << QStringLiteral("  thumbnailTargetLongEdge: %1").arg(prefs::thumbnailTargetLongEdge());
    lines << QStringLiteral("");
    lines << QStringLiteral("Folder:");
    lines << QStringLiteral("  currentPath: %1").arg(currentPath_);

    QString text = lines.join(QStringLiteral("\n"));
    QGuiApplication::clipboard()->setText(text);
    statusBar()->showMessage(QStringLiteral("Grid debug info copied to clipboard (%1 lines)").arg(lines.size()), 5000);
}

void MainWindow::onIndexerStarted(QString path) {
    if (path != currentPath_) return;
    // Folder name only, not the full path (see addBookmark() for the same
    // drive-root fallback) - a long full path was the same class of collision-with-
    // the-permanent-widget-row bug as onIndexerFinished's old message (see that
    // handler's comment), and is more than anyone needs for "which folder is this
    // about" when it's already the one on screen. No explicit timeout - cleared by
    // onIndexerFinished(), not by a fixed delay, since indexing can take anywhere
    // from milliseconds to well over a minute on a large folder.
    QString label = QFileInfo(path).fileName();
    if (label.isEmpty()) label = path;
    statusBar()->showMessage(QStringLiteral("Indexing %1...").arg(label));
}

void MainWindow::onFilesListed(QString path) {
    // Pass A just finished - the file list is final, even though thumbnails are
    // mostly still pending. This is usually the very first real file list for a
    // freshly-navigated-to folder (nothing selected yet, nothing to preserve), but
    // it also fires on an explicit Refresh of the folder already on screen -
    // reloadGridPreservingSelection() carries the user's selection/scroll position
    // across that case instead of dropping them on every Refresh.
    if (path != currentPath_) return;
    reloadGridPreservingSelection();
    updateSelectionStatus(); // folder aggregates changed (this is often the first real file list)
    updateThumbStatusIndicator(); // the file list is final, so the freshness count is meaningful now
    trySelectPendingFile(); // this folder may not have had rows loaded until just now
}

void MainWindow::onThumbsProgress(QString path) {
    // A Pass B batch just landed - pull in the newly-ready thumbnails without
    // resetting the model, so already-displayed ones don't flicker.
    if (path != currentPath_) return;
    gridModel_->refreshThumbStates();
    grid_->viewport()->update(); // see the comment on the thumbReady connection above
    updateSelectionStatus();     // dimensions/taken-at/duration only land at decode time
}

void MainWindow::onIndexerFinished(QString path) {
    if (path != currentPath_) return;
    gridModel_->refreshThumbStates(); // catch any trailing batch
    grid_->viewport()->update();
    // Used to also post a transient statusBar()->showMessage("<path> - N items", ...)
    // here - redundant with folderStatsLabel_ (which already shows this persistently)
    // and the actual bug: QStatusBar's built-in temporary-message label doesn't elide/
    // shrink the way StatusLabel does (see its class comment), so a long full path
    // routinely ran right into the permanent widget row, e.g. rendering as
    // "...\Models - 4 i" immediately followed by folderStatsLabel_'s own
    // "4 items (4 img, 0 vid) · 9.52 MiB" with no gap - a real reported bug, not
    // hypothetical. updateSelectionStatus() here instead keeps rawStatusLabel_ (RAW
    // rendered/preview counts) fresh too, which this handler previously didn't touch.
    updateSelectionStatus();
    // Thumbnails have settled, so a folder that was red because it was mid-generation can
    // now legitimately go green.
    updateThumbStatusIndicator();
    // Clears onIndexerStarted()'s "Indexing <folder>..." message, which has no
    // timeout of its own (indexing can take anywhere from milliseconds to well over
    // a minute) - without this call, removing the old showMessage() here (above)
    // left nothing to ever replace/clear it, so it silently lingered forever. A real
    // reported regression from that first fix, not hypothetical either.
    statusBar()->clearMessage();
}

void MainWindow::onBackgroundDirectoryChanged(QString path) {
    if (path != currentPath_) return;
    // Same light-touch refresh as onThumbsProgress - a background sweep re-thumbnailing
    // a file the user happens to be looking at shouldn't reset the model and flicker
    // everything else on screen.
    gridModel_->refreshThumbStates();
    grid_->viewport()->update();
    updateSelectionStatus();
}

void MainWindow::onWatchedDirectoryChanged(const QString &path) {
    if (path != currentPath_) return; // stale signal for a folder navigated away from since
    folderWatchDebounce_->start(); // (re)starts - several notifications in a row collapse to one rescan
}

void MainWindow::loadBookmarks() {
    bookmarks_->clear();
    auto sel = db_->prepare("SELECT id, path, label FROM bookmarks ORDER BY sort");
    while (sel.step()) {
        qint64 id = sel.columnInt64(0);
        QString path = QString::fromStdString(sel.columnText(1));
        QString label = QString::fromStdString(sel.columnText(2));
        if (label.isEmpty()) label = path;

        auto *item = new QListWidgetItem(label, bookmarks_);
        item->setData(Qt::UserRole, path);
        item->setData(Qt::UserRole + 1, id);
    }
}

void MainWindow::addBookmark(const QString &path) {
    QString label = QFileInfo(path).fileName();
    if (label.isEmpty()) label = path; // e.g. a drive root like "C:\"

    auto countSel = db_->prepare("SELECT count(*) FROM bookmarks");
    countSel.step();
    int64_t nextSort = countSel.columnInt64(0);

    auto ins = db_->prepare("INSERT INTO bookmarks(path, label, sort) VALUES(?,?,?)");
    ins.bind(1, path.toStdString());
    ins.bind(2, label.toStdString());
    ins.bind(3, nextSort);
    ins.step();

    loadBookmarks();
}

void MainWindow::removeBookmark(qint64 id) {
    auto del = db_->prepare("DELETE FROM bookmarks WHERE id=?");
    del.bind(1, (int64_t)id);
    del.step();
    loadBookmarks();
}

void MainWindow::restoreLastDirectory() {
    QSettings settings = prefs::settingsStore();
    QString last = settings.value(QStringLiteral("lastDirectory")).toString();

    if (!last.isEmpty() && QDir(last).exists()) {
        navigateTo(last);
        return;
    }

    QString pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (!pictures.isEmpty() && QDir(pictures).exists()) navigateTo(pictures);
}

void MainWindow::saveLastDirectory(const QString &path) {
    QSettings settings = prefs::settingsStore();
    settings.setValue(QStringLiteral("lastDirectory"), path);
}

void MainWindow::nukeDatabase() {
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);

    // Deliberately not `bookmarks` - those are user-curated navigation shortcuts,
    // not scan-derived cache, and re-scanning can't regenerate them. Everything else
    // here is exactly what a fresh rescan rebuilds from scratch.
    db_->exec("DELETE FROM files;");
    db_->exec("DELETE FROM dirs;");
    db_->exec("DELETE FROM claims;");
    db_->exec("DELETE FROM journal;");
    db_->exec("DELETE FROM thumbs.thumbs;");
    // Reclaims the actual disk space the deleted rows/blobs held - without this the
    // files would just have a lot of internally-tracked free space, which doesn't
    // match "nuke" for a cache that can genuinely run to several GB of thumbnails.
    db_->exec("VACUUM;");
    db_->exec("VACUUM thumbs;");

    QGuiApplication::restoreOverrideCursor();

    // Every row the grid/tree/status bar currently reference is gone - force a fully
    // fresh Pass A/B of whatever's on screen right now rather than leaving an empty
    // grid the user has to manually Refresh to escape.
    if (!currentPath_.isEmpty()) navigateTo(currentPath_, /*forceReindex=*/true);
}

void MainWindow::saveWindowState() {
    QSettings settings = prefs::settingsStore();
    settings.setValue(QStringLiteral("windowGeometry"), saveGeometry());
    settings.setValue(QStringLiteral("mainSplitterState"), splitter_->saveState());
    settings.setValue(QStringLiteral("topSplitterState"), topSplitter_->saveState());
    settings.setValue(QStringLiteral("leftSplitterState"), leftSplitter_->saveState());
}

void MainWindow::closeEvent(QCloseEvent *event) {
    saveWindowState();
    QMainWindow::closeEvent(event);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    // Mouse back/forward buttons -> folder history, matching Alt+Left / Alt+Right. The type
    // check is first and cheap because this filter is installed application-wide.
    if (event->type() == QEvent::MouseButtonPress) {
        Qt::MouseButton button = static_cast<QMouseEvent *>(event)->button();
        if (button == Qt::BackButton || button == Qt::ForwardButton) {
            // Not while the fullscreen viewer is up: there, "back" reads as the previous
            // image, and silently changing folders underneath it would be worse than doing
            // nothing. Left unhandled rather than repurposed - image navigation already has
            // the arrow keys and the wheel.
            if (fullscreenViewer_ && fullscreenViewer_->isVisible()) return false;
            if (button == Qt::BackButton) onNavigateBack();
            else onNavigateForward();
            return true;
        }
    }

    if (watched == pathBar_->lineEdit() && event->type() == QEvent::FocusIn) {
        // A plain selectAll() here gets immediately undone by the mouse-press event
        // that triggered this focus-in (it repositions the cursor to the click point,
        // collapsing the selection) - deferring to the next event loop turn lets that
        // click finish being processed first.
        QTimer::singleShot(0, pathBar_->lineEdit(), &QLineEdit::selectAll);
    }
    return QMainWindow::eventFilter(watched, event);
}

bool MainWindow::isWindowOnScreen() const {
    QRect frame = frameGeometry();
    for (QScreen *screen : QGuiApplication::screens()) {
        // Require a reasonably-sized chunk of the frame to be reachable (not just a
        // stray pixel), so a window barely clipped at a screen edge still counts as "on
        // screen" rather than getting needlessly recentered.
        QRect visible = screen->availableGeometry().intersected(frame);
        if (visible.width() > 100 && visible.height() > 60) return true;
    }
    return false;
}

void MainWindow::navigateToInput(const QString &input) {
    QString trimmed = input.trimmed();
    if (trimmed.isEmpty()) return;

    QFileInfo info(trimmed);
    if (info.isDir()) {
        navigateTo(trimmed);
        return;
    }
    if (info.isFile()) {
        navigateTo(info.absolutePath());
        // navigateTo() just cleared pendingSelectFileName_ (it's meant to be armed
        // per-navigation, not leftover from some earlier attempt) - set it after,
        // then try immediately for the common case where this folder is already
        // cached/loaded. onFilesListed() catches the case where it wasn't.
        pendingSelectFileName_ = info.fileName();
        trySelectPendingFile();
        return;
    }

    statusBar()->showMessage(QStringLiteral("Not found: %1").arg(trimmed), 4000);
    pathBar_->setCurrentText(currentPath_);
}

void MainWindow::trySelectPendingFile() {
    if (pendingSelectFileName_.isEmpty()) return;
    int row = gridModel_->rowForName(pendingSelectFileName_);
    if (row < 0) return; // folder's rows may not be loaded yet - retried on the next reload
    grid_->setCurrentRow(row);
    grid_->scrollToRow(row, /*center=*/true);
    pendingSelectFileName_.clear();
}

void MainWindow::refreshPathBarHistory() {
    // Preserved explicitly rather than relying on clear()/addItems() to leave it
    // alone - QComboBox::clear() drops the current index (and, with it, an editable
    // combo's displayed text) as a side effect, which would otherwise blow away
    // whatever's currently shown (e.g. a selected file's full path) every time a new
    // folder is visited.
    QString current = pathBar_->currentText();
    QSignalBlocker blocker(pathBar_); // clear()/addItems() below must not look like a user pick
    pathBar_->clear();
    pathBar_->addItems(prefs::pathHistory());
    pathBar_->setCurrentText(current);
}

void MainWindow::onPurgePathHistory() {
    prefs::clearPathHistory();
    refreshPathBarHistory();
    statusBar()->showMessage(QStringLiteral("Path history cleared"), 3000);
}

void MainWindow::reloadGridPreservingSelection() {
    QList<qint64> selectedIds;
    for (int r : grid_->selectedRows()) {
        qint64 id = gridModel_->index(r).data(ThumbGridModel::FileIdRole).toLongLong();
        if (id != 0) selectedIds << id;
    }
    // FileIdRole is 0 for an invalid index (grid_->currentRow() == -1) - rowForFileId(0)
    // below correctly finds nothing, so no separate "was there a selection at all" check.
    qint64 currentId = gridModel_->index(grid_->currentRow()).data(ThumbGridModel::FileIdRole).toLongLong();
    int scrollValue = grid_->verticalScrollBar()->value();

    gridModel_->setDirectory(currentPath_);

    QList<int> rows;
    for (qint64 id : selectedIds) {
        int r = gridModel_->rowForFileId(id);
        if (r >= 0) rows << r;
    }
    grid_->setSelection(rows, gridModel_->rowForFileId(currentId));

    // Set after setSelection() (which itself doesn't scroll) so nothing downstream
    // moves the scrollbar again after this - restores the exact pre-reload position
    // rather than re-deriving it from whichever row ended up selected.
    grid_->verticalScrollBar()->setValue(scrollValue);
}

void MainWindow::updateSelectionStatus() {
    // Folder-level aggregates - always shown when a folder is loaded, regardless of
    // whether anything is selected.
    int imageCount = gridModel_->imageCount();
    int videoCount = gridModel_->videoCount();
    int totalCount = imageCount + videoCount;
    folderStatsLabel_->setStatusText(totalCount > 0 ? QStringLiteral("%1 items (%2 img, %3 vid)  ·  %4")
                                                           .arg(totalCount)
                                                           .arg(imageCount)
                                                           .arg(videoCount)
                                                           .arg(QLocale().formattedDataSize(gridModel_->totalBytes()))
                                                     : QString());

    // How many of this folder's RAW files have a real full-render thumbnail vs. still
    // just the fast embedded-preview one - see RawRenderer/`pixet-index --render-raws`.
    // Blank whenever there's nothing to report (no RAW files here, or none have
    // reached either state yet) rather than showing "0 rendered, 0 preview".
    int rawRendered = gridModel_->rawRenderedCount();
    int rawPreview = gridModel_->rawPreviewCount();
    int rawKnown = rawRendered + rawPreview;
    rawStatusLabel_->setStatusText(
        rawKnown > 0 ? QStringLiteral("%1 RAW: %2 rendered, %3 preview").arg(rawKnown).arg(rawRendered).arg(rawPreview)
                     : QString());

    QModelIndex idx = gridModel_->index(grid_->currentRow());
    if (!idx.isValid()) {
        fileNameLabel_->setStatusText(QString());
        formatLabel_->setStatusText(QString());
        dimsLabel_->setStatusText(QString());
        sizeLabel_->setStatusText(QString());
        dateLabel_->setStatusText(QString());
        durationLabel_->setStatusText(QString());
        return;
    }

    // Elided rather than left to wrap/clip raggedly - the label's fixed width means a
    // long filename would otherwise just get cut off mid-character. Full name still
    // available on hover (StatusLabel::setStatusText() sets it as a tooltip).
    //
    // With more than one item selected, this and the size label switch to an
    // Explorer-style aggregate (count + the previewed item's name; combined size);
    // format/dimensions/date/duration keep describing the previewed (lead) item
    // specifically, same as the single-selection case - blanking them on every
    // Ctrl+click would be exactly the label-jitter StatusBarRow/StatusLabel were
    // built to avoid (see the member comment on these labels in MainWindow.h).
    int selCount = grid_->selectionCount();
    QString name = idx.data(Qt::DisplayRole).toString();
    if (selCount > 1) {
        fileNameLabel_->setStatusText(QStringLiteral("%1 selected · %2").arg(selCount).arg(name), Qt::ElideMiddle);
    } else {
        fileNameLabel_->setStatusText(name, Qt::ElideMiddle);
    }

    int fmt = idx.data(ThumbGridModel::FormatRole).toInt();
    formatLabel_->setStatusText(QString::fromUtf8(pixet::formatName((pixet::Format)fmt)));

    int w = idx.data(ThumbGridModel::WidthRole).toInt();
    int h = idx.data(ThumbGridModel::HeightRole).toInt();
    if (w > 0 && h > 0) {
        QString dims = QStringLiteral("%1×%2").arg(w).arg(h);
        // Only known once the preview decode lands (see currentPreviewBpp_) - may be
        // briefly absent right after selecting, same as the async width/height fields.
        if (currentPreviewBpp_ > 0) dims += QStringLiteral(" (%1bpp)").arg(currentPreviewBpp_);
        dimsLabel_->setStatusText(dims);
    } else {
        dimsLabel_->setStatusText(QString());
    }

    qint64 size = selCount > 1 ? gridModel_->sizeForRows(grid_->selectedRows())
                                : idx.data(ThumbGridModel::SizeRole).toLongLong();
    sizeLabel_->setStatusText(size > 0 ? QLocale().formattedDataSize(size) : QString());

    qint64 takenAt = idx.data(ThumbGridModel::TakenAtRole).toLongLong();
    dateLabel_->setStatusText(takenAt > 0
                                   ? QDateTime::fromSecsSinceEpoch(takenAt).toString(QStringLiteral("yyyy-MM-dd hh:mm"))
                                   : QString());

    qint64 durationMs = idx.data(ThumbGridModel::DurationMsRole).toLongLong();
    if (durationMs > 0) {
        qint64 totalSec = durationMs / 1000;
        durationLabel_->setStatusText(
            QStringLiteral("%1:%2").arg(totalSec / 60).arg(totalSec % 60, 2, 10, QChar('0')));
    } else {
        durationLabel_->setStatusText(QString());
    }

    // Bundled here (rather than left to selectionChanged/aboutToShow alone) so a
    // keyboard-only Ctrl+V works immediately after a navigateTo()/onFilesListed()
    // call, which resets the grid's selection without emitting selectionChanged
    // (see ThumbGridView::setModel's modelReset handling) and doesn't otherwise
    // touch this - without this, currentPath_ becoming non-empty on first launch
    // wouldn't enable Paste until the user opened the Edit menu once.
    updateEditActionsEnabled();
}

void MainWindow::updateEditActionsEnabled() {
    bool hasSelection = grid_->selectionCount() > 0;
    cutAction_->setEnabled(hasSelection);
    copyAction_->setEnabled(hasSelection);
    pasteAction_->setEnabled(!currentPath_.isEmpty());
}

void MainWindow::restoreWindowState() {
    // Restores window geometry saved by closeEvent() (splitter layout is handled
    // separately, deferred until after the first real layout pass - see the
    // QTimer::singleShot in the constructor). resetLayout_ (set from the
    // --reset-layout CLI flag, see main.cpp) skips restoring here - the next normal
    // close then saves fresh default values right back, so a window parked in a
    // strange or off-screen state self-heals with a single relaunch flag rather than
    // needing to hand-edit or delete QSettings.
    QSettings settings = prefs::settingsStore();

    bool geometryRestored = false;
    if (!resetLayout_) {
        QByteArray geometry = settings.value(QStringLiteral("windowGeometry")).toByteArray();
        if (!geometry.isEmpty() && restoreGeometry(geometry) && isWindowOnScreen()) geometryRestored = true;
    }
    if (!geometryRestored) {
        resize(1280, 800);
        if (QScreen *screen = QGuiApplication::primaryScreen()) {
            QRect avail = screen->availableGeometry();
            move(avail.center() - QPoint(width() / 2, height() / 2));
        }
    }
}
