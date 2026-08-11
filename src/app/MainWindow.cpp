#include "MainWindow.h"

#include <QAction>
#include <QClipboard>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QGuiApplication>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QProcess>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "BackgroundReconciler.h"
#include "FolderIndexer.h"
#include "FolderTreeView.h"
#include "FullscreenViewer.h"
#include "KeyBindings.h"
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
#include "util/PathUtil.h"
#include "version.h"

namespace {
// dirs.path / files.name are UTF-8 in the DB; QFileSystemModel paths need the same
// backslash-normalized form pixet_core writes, or path-string lookups silently miss.
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
    fsModel_->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot | QDir::Drives);

    tree_ = new FolderTreeView(this);
    tree_->setModel(fsModel_);
    tree_->setRootIndex(fsModel_->index(QString()));
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
    grid_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(grid_, &QWidget::customContextMenuRequested, this, &MainWindow::onGridContextMenu);
    connect(grid_, &ThumbGridView::navigateFolderRequested, this, &MainWindow::onNavigateFolderRequested);
    // Fires on both double-click and Enter/Return - exactly the two ways to "open"
    // an item.
    connect(grid_, &ThumbGridView::activated, this, &MainWindow::onGridItemActivated);

    fullscreenViewer_ = new FullscreenViewer(this);
    // Keep the grid's selection following along while browsing fullscreen, so
    // closing it (Escape/double-click) leaves the grid on whatever image was last
    // shown there instead of wherever it was when fullscreen opened.
    connect(fullscreenViewer_, &FullscreenViewer::rowChanged, this, [this](int row) {
        grid_->setCurrentRow(row);
        grid_->scrollToRow(row);
    });

    // --- path bar: shows/edits currentPath_; Enter navigates (see navigateToInput) ---
    pathBar_ = new QLineEdit(this);
    pathBar_->setPlaceholderText(QStringLiteral("Path..."));
    connect(pathBar_, &QLineEdit::returnPressed, this, &MainWindow::onPathBarReturnPressed);
    // select-all-on-focus is implemented in eventFilter() (QEvent::FocusIn) - that
    // only fires because of this call, which got missed when the feature was
    // originally added.
    pathBar_->installEventFilter(this);

    // --- top-level: path bar above, left column (40%) vs. thumbnail grid (60%) below ---
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
    centralLayout->addWidget(pathBar_);
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

    previewDebounce_ = new QTimer(this);
    previewDebounce_->setSingleShot(true);
    previewDebounce_->setInterval(80);
    connect(previewDebounce_, &QTimer::timeout, this, &MainWindow::triggerPreviewRequest);

    // --- menu ---
    // Shortcuts are user-configurable (see KeyBindings.h/PreferencesDialog) - the
    // QAction pointers are kept as members so onPreferences() can re-apply their
    // shortcuts after the editor closes, since QAction doesn't watch settings
    // itself.
    auto *bookmarksMenu = menuBar()->addMenu(QStringLiteral("&Bookmarks"));
    addBookmarkAction_ = bookmarksMenu->addAction(QStringLiteral("Add Current Folder"), this, &MainWindow::onAddBookmark);

    auto *viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    refreshAction_ = viewMenu->addAction(QStringLiteral("Refresh"), this, &MainWindow::onRefresh);
    toggleSidePanelAction_ = viewMenu->addAction(QStringLiteral("Toggle Side Panel"), this, &MainWindow::onToggleSidePanel);
    applyKeyBindingShortcuts();

    auto *toolsMenu = menuBar()->addMenu(QStringLiteral("&Tools"));
    toolsMenu->addAction(QStringLiteral("Preferences..."), this, &MainWindow::onPreferences);

    // TODO: was debug-build-only; in release too for now (2026-08-11) - see
    // onCopyGridDebugInfo()'s doc comment (MainWindow.h). Permanent fixture either way.
    auto *debugMenu = menuBar()->addMenu(QStringLiteral("&Debug"));
    debugMenu->addAction(QStringLiteral("Copy Grid Debug Info"), this, &MainWindow::onCopyGridDebugInfo);

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
    pathBar_->setText(normalized);
    preview_->clear();
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
    // to, so this doesn't fight a selection the user has since changed manually.
    if (currentPath_.isEmpty()) return;
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

void MainWindow::onGridContextMenu(const QPoint &pos) {
    QMenu menu(this);
    menu.addAction(QStringLiteral("Refresh (check for new/changed files)"), this, &MainWindow::onRefresh);
    menu.addAction(QStringLiteral("Force Re-thumbnail This Folder"), this, &MainWindow::onForceRethumbnail);
    menu.exec(grid_->mapToGlobal(pos));
}

void MainWindow::onGridItemActivated(int row) {
    if (row < 0) return;
    QModelIndex index = gridModel_->index(row);

    // Video has no playback engine in the fullscreen viewer (poster frame only, see
    // FullscreenViewer's class comment) - activating one launches an actual player
    // instead, per the user's own preference, rather than opening the viewer at all.
    if ((pixet::Format)index.data(ThumbGridModel::FormatRole).toInt() == pixet::Format::Video) {
        QString filePath = currentPath_ + QStringLiteral("\\") + index.data(Qt::DisplayRole).toString();
        if (prefs::useSystemVideoPlayer() || prefs::customVideoPlayerPath().isEmpty()) {
            // Also the fallback when "Custom" is selected but no path was ever set -
            // silently doing nothing on activation would look like the double-click
            // just didn't register.
            QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
        } else {
            QProcess::startDetached(prefs::customVideoPlayerPath(), {filePath});
        }
        return;
    }

    fullscreenViewer_->openAt(gridModel_, currentPath_, row);
}

void MainWindow::onPathBarReturnPressed() { navigateToInput(pathBar_->text()); }

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
        preview_->clear();
        pathBar_->setText(currentPath_);
        return;
    }

    QString name = idx.data(Qt::DisplayRole).toString();
    pendingPreviewFmt_ = idx.data(ThumbGridModel::FormatRole).toInt();
    pendingPreviewPath_ = currentPath_ + QStringLiteral("\\") + name;
    pathBar_->setText(pendingPreviewPath_);

    previewDebounce_->start();
}

void MainWindow::triggerPreviewRequest() {
    if (pendingPreviewPath_.isEmpty()) return;
    currentPreviewRequestId_ = ++previewRequestCounter_;
    previewDecoder_->requestPreview(currentPreviewRequestId_, pendingPreviewPath_, pendingPreviewFmt_,
                                     preview_->preferredTargetLongEdge());
}

void MainWindow::onPreviewReady(qint64 requestId, QImage image) {
    if (requestId != currentPreviewRequestId_) return; // superseded by a newer selection
    preview_->setImage(image);
    // QImage::depth() (bits per pixel, e.g. 24 for 8-bit/channel RGB) is the only place
    // this app has bit-depth info without adding a DB column - see the member comment.
    currentPreviewBpp_ = image.isNull() ? 0 : image.depth();
    updateSelectionStatus();
}

void MainWindow::onAddBookmark() {
    if (!currentPath_.isEmpty()) addBookmark(currentPath_);
}

void MainWindow::onRefresh() {
    if (!currentPath_.isEmpty()) navigateTo(currentPath_, /*forceReindex=*/true);
}

void MainWindow::onForceRethumbnail() {
    if (!currentPath_.isEmpty()) navigateTo(currentPath_, /*forceReindex=*/true, /*forceRethumbnail=*/true);
}

void MainWindow::onPreferences() {
    PreferencesDialog dlg(this);
    connect(&dlg, &PreferencesDialog::reindexRequested, this, [this]() { emit requestFullReindex(); });
    connect(&dlg, &PreferencesDialog::thumbnailSizeChanged, this, [this]() {
        int size = prefs::thumbnailIconSize();
        grid_->setIconSize(QSize(size, size)); // recomputes the grid layout itself now
        // Already-decoded pixmaps in the model are sized for the *old* icon size -
        // reloading re-requests every visible thumbnail fresh from ThumbLoader, which
        // now decodes to the new size (see ThumbLoader::processOne()).
        if (!currentPath_.isEmpty()) gridModel_->setDirectory(currentPath_);
    });
    connect(&dlg, &PreferencesDialog::nukeDatabaseRequested, this, &MainWindow::nukeDatabase);
    dlg.exec();
    // Cheap enough to always re-apply regardless of whether the keybindings editor
    // actually changed anything (Cancel discards its own edits before this point) -
    // no need for a dedicated changed-detection signal just for this.
    applyKeyBindingShortcuts();
}

void MainWindow::applyKeyBindingShortcuts() {
    refreshAction_->setShortcut(keybindings::binding(keybindings::Action::Refresh));
    toggleSidePanelAction_->setShortcut(keybindings::binding(keybindings::Action::ToggleSidePanel));
    addBookmarkAction_->setShortcut(keybindings::binding(keybindings::Action::AddBookmark));
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
    if (path == currentPath_) statusBar()->showMessage(QStringLiteral("Indexing %1...").arg(path));
}

void MainWindow::onFilesListed(QString path) {
    // Pass A just finished - the file list is final, even though thumbnails are
    // mostly still pending. Full reload: this is the one point where resetting the
    // model is correct, since nothing meaningful is on screen yet to flicker away.
    if (path != currentPath_) return;
    gridModel_->setDirectory(path);
    updateSelectionStatus(); // folder aggregates changed (this is often the first real file list)
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
    statusBar()->showMessage(QStringLiteral("%1 - %2 items").arg(path).arg(gridModel_->rowCount()), 4000);
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

void MainWindow::closeEvent(QCloseEvent *event) {
    QSettings settings = prefs::settingsStore();
    settings.setValue(QStringLiteral("windowGeometry"), saveGeometry());
    settings.setValue(QStringLiteral("mainSplitterState"), splitter_->saveState());
    settings.setValue(QStringLiteral("topSplitterState"), topSplitter_->saveState());
    settings.setValue(QStringLiteral("leftSplitterState"), leftSplitter_->saveState());
    QMainWindow::closeEvent(event);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (watched == pathBar_ && event->type() == QEvent::FocusIn) {
        // A plain selectAll() here gets immediately undone by the mouse-press event
        // that triggered this focus-in (it repositions the cursor to the click point,
        // collapsing the selection) - deferring to the next event loop turn lets that
        // click finish being processed first.
        QTimer::singleShot(0, pathBar_, &QLineEdit::selectAll);
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
    pathBar_->setText(currentPath_);
}

void MainWindow::trySelectPendingFile() {
    if (pendingSelectFileName_.isEmpty()) return;
    int row = gridModel_->rowForName(pendingSelectFileName_);
    if (row < 0) return; // folder's rows may not be loaded yet - retried on the next reload
    grid_->setCurrentRow(row);
    grid_->scrollToRow(row, /*center=*/true);
    pendingSelectFileName_.clear();
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
    QString name = idx.data(Qt::DisplayRole).toString();
    fileNameLabel_->setStatusText(name, Qt::ElideMiddle);

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

    qint64 size = idx.data(ThumbGridModel::SizeRole).toLongLong();
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
