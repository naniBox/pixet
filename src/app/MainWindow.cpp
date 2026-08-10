#include "MainWindow.h"

#include <QAction>
#include <QCloseEvent>
#include <QDateTime>
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
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

#include "BackgroundReconciler.h"
#include "FolderIndexer.h"
#include "FolderTreeView.h"
#include "FullscreenViewer.h"
#include "PreviewDecoder.h"
#include "PreviewPane.h"
#include "RawRenderer.h"
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
} // namespace

MainWindow::MainWindow(bool resetLayout, QWidget *parent) : QMainWindow(parent), resetLayout_(resetLayout) {
    setWindowTitle(QStringLiteral("pixet %1").arg(pixet::version()));
    resize(1280, 800);

    db_ = std::make_unique<pixet::Database>(pixet::indexDbPath(), pixet::thumbsDbPath(), false);

    // --- left panel: folder tree + bookmarks (top), preview (bottom, forced square) ---
    bookmarks_ = new QListWidget(this);
    bookmarks_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(bookmarks_, &QListWidget::itemClicked, this, &MainWindow::onBookmarkClicked);
    connect(bookmarks_, &QListWidget::customContextMenuRequested, this, &MainWindow::onBookmarksContextMenu);

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

    // --- right: preview pane (constructed here so topSplitter_/leftPanel_ below can
    // reference it - actually placed at the bottom of the left column, see layout) ---
    preview_ = new PreviewPane(this);

    topSplitter_ = new QSplitter(Qt::Horizontal, this);
    topSplitter_->addWidget(tree_);
    topSplitter_->addWidget(bookmarks_);
    topSplitter_->setStretchFactor(0, 7);
    topSplitter_->setStretchFactor(1, 3);
    topSplitter_->setCollapsible(0, false);
    topSplitter_->setCollapsible(1, false);

    leftPanel_ = new QWidget(this);
    auto *leftLayout = new QVBoxLayout(leftPanel_);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(topSplitter_, /*stretch=*/1);
    leftLayout->addWidget(preview_, /*stretch=*/0);
    // preview_'s height is forced to match leftPanel_'s width (see
    // updateLeftSquarePreview()), not left to the layout - that's what keeps it square
    // as the window or the main splitter get resized. No QSplitter handle between the
    // two, deliberately: the split isn't something the user drags, it's derived.
    leftPanel_->installEventFilter(this);

    // --- center: thumbnail grid ---
    gridModel_ = new ThumbGridModel(*db_, this);
    grid_ = new ThumbGridView(this);
    grid_->setModel(gridModel_);
    grid_->setViewMode(QListView::IconMode);
    // Fixed, not the more obvious-looking Adjust: Adjust makes QListView relayout
    // items automatically on *every* resizeEvent, using whatever gridSize() happens to
    // be set at that instant - which, mid-drag, is the previous (stale) column count,
    // since ThumbGridView::updateGridSize() deliberately debounces and only recomputes
    // once resizing settles (see its class comment). That gave Qt's own automatic
    // relayout and our explicit one two independent, uncoordinated opinions about
    // layout during a drag, racing on every intermediate frame. Fixed leaves item
    // positions untouched until something explicitly asks for a relayout -
    // ThumbGridView::updateGridSize() already does that itself (doItemsLayout()), so
    // this doesn't lose any actual behavior, just the redundant/conflicting one.
    grid_->setResizeMode(QListView::Fixed);
    grid_->setMovement(QListView::Static);
    grid_->setIconSize(QSize(ThumbLoader::kThumbIconSize, ThumbLoader::kThumbIconSize));
    // Cell size (grid size) is self-managed by ThumbGridView, recomputed on resize so
    // columns stretch to fill the viewport width evenly - see
    // ThumbGridView::updateGridSize().
    // Deliberately NOT setUniformItemSizes(true): thumbnails arrive asynchronously,
    // so the first layout pass sees empty decorations for most cells. That flag tells
    // Qt to cache whatever size it computes then, from a mostly-decoration-less item,
    // and never revisit it - every thumbnail that streams in afterward gets clipped to
    // that stale (too-small) cached size until something (e.g. a hover) forces a
    // relayout. This was very likely the actual cause of the "only shows a tiny sliver
    // until I hover" bug. setGridSize() already gives fixed, explicit cell dimensions,
    // so the performance case for this flag doesn't really apply here anyway.
    grid_->setSelectionMode(QAbstractItemView::SingleSelection);
    connect(grid_->selectionModel(), &QItemSelectionModel::currentChanged, this, &MainWindow::onGridSelectionChanged);
    grid_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(grid_, &QWidget::customContextMenuRequested, this, &MainWindow::onGridContextMenu);
    connect(grid_, &ThumbGridView::navigateFolderRequested, this, &MainWindow::onNavigateFolderRequested);
    // QAbstractItemView::activated fires on both double-click and Enter/Return by
    // default - exactly the two ways to "open" an item.
    connect(grid_, &QAbstractItemView::activated, this, &MainWindow::onGridItemActivated);

    fullscreenViewer_ = new FullscreenViewer(this);
    // Keep the grid's selection following along while browsing fullscreen, so
    // closing it (Escape/double-click) leaves the grid on whatever image was last
    // shown there instead of wherever it was when fullscreen opened.
    connect(fullscreenViewer_, &FullscreenViewer::rowChanged, this, [this](int row) {
        QModelIndex idx = gridModel_->index(row);
        grid_->setCurrentIndex(idx);
        grid_->scrollTo(idx);
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
        QSettings settings(QStringLiteral("pixet"), QStringLiteral("pixet"));

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

        updateLeftSquarePreview();
    });

    // --- background workers (no parent - see the member declarations in the header) ---
    thumbLoader_ = std::make_unique<ThumbLoader>();
    connect(gridModel_, &ThumbGridModel::thumbNeeded, thumbLoader_.get(), &ThumbLoader::request);
    connect(thumbLoader_.get(), &ThumbLoader::thumbReady, gridModel_, &ThumbGridModel::setThumbnail);
    // Belt-and-suspenders: QListView's IconMode + setUniformItemSizes has shown
    // unreliable partial repaints under many small dataChanged emissions arriving
    // over time (some cells stay stuck showing a placeholder until an unrelated
    // interaction like a hover forces Qt to relayout). An explicit viewport update
    // is cheap - Qt coalesces repeated calls within one event loop iteration - and
    // removes any doubt about whether the model's dataChanged alone was enough.
    connect(thumbLoader_.get(), &ThumbLoader::thumbReady, this,
            [this](qint64, const QPixmap &) { grid_->viewport()->update(); });

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
    auto *bookmarksMenu = menuBar()->addMenu(QStringLiteral("&Bookmarks"));
    bookmarksMenu->addAction(QStringLiteral("Add Current Folder"), this, &MainWindow::onAddBookmark, QKeySequence(QStringLiteral("Ctrl+D")));

    auto *viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    viewMenu->addAction(QStringLiteral("Refresh"), this, &MainWindow::onRefresh, QKeySequence(QStringLiteral("F5")));

    // Fixed pixel widths, sized generously for typical content (not a hard guarantee
    // against every possible value, e.g. an absurdly large dimension could clip) -
    // the point is stability across normal browsing, not exact-fit sizing.
    auto makeStatusLabel = [this](int width) {
        auto *label = new QLabel(this);
        label->setFixedWidth(width);
        statusBar()->addPermanentWidget(label);
        return label;
    };
    folderStatsLabel_ = makeStatusLabel(300);
    rawStatusLabel_ = makeStatusLabel(190);
    fileNameLabel_ = makeStatusLabel(240);
    formatLabel_ = makeStatusLabel(50);
    dimsLabel_ = makeStatusLabel(150);
    sizeLabel_ = makeStatusLabel(85);
    dateLabel_ = makeStatusLabel(115);
    durationLabel_ = makeStatusLabel(45);

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
        case Qt::Key_Down: {
            // Next sibling - or, if this is the last one, walk up until an ancestor
            // actually has a next sibling ("aunt": your parent's next sibling; a
            // grandparent's if the parent is *also* last, and so on), rather than
            // just stopping at the last child in a folder. No-ops only once this
            // walks all the way up without finding one (nothing left in the tree).
            QModelIndex current = idx;
            while (current.isValid()) {
                QModelIndex candidate = fsModel_->index(current.row() + 1, 0, current.parent());
                if (candidate.isValid()) {
                    target = candidate;
                    break;
                }
                current = current.parent();
            }
            break;
        }
        case Qt::Key_Left:
            target = idx.parent();
            break;
        case Qt::Key_Right: {
            // fsModel_'s filter (see constructor) is dirs-only, so any row here is
            // genuinely a subfolder. The current folder is already expanded by the
            // time its thumbnails are on screen (navigateTo() does that), so its
            // children are normally already populated - a folder Ctrl+Right lands on
            // that was never expanded first just no-ops rather than fetching async.
            if (fsModel_->rowCount(idx) > 0) target = fsModel_->index(0, 0, idx);
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

void MainWindow::onGridItemActivated(const QModelIndex &index) {
    if (!index.isValid()) return;
    fullscreenViewer_->openAt(gridModel_, currentPath_, index.row());
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

    QModelIndex idx = grid_->currentIndex();
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
    QSettings settings(QStringLiteral("pixet"), QStringLiteral("pixet"));
    QString last = settings.value(QStringLiteral("lastDirectory")).toString();

    if (!last.isEmpty() && QDir(last).exists()) {
        navigateTo(last);
        return;
    }

    QString pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (!pictures.isEmpty() && QDir(pictures).exists()) navigateTo(pictures);
}

void MainWindow::saveLastDirectory(const QString &path) {
    QSettings settings(QStringLiteral("pixet"), QStringLiteral("pixet"));
    settings.setValue(QStringLiteral("lastDirectory"), path);
}

void MainWindow::closeEvent(QCloseEvent *event) {
    QSettings settings(QStringLiteral("pixet"), QStringLiteral("pixet"));
    settings.setValue(QStringLiteral("windowGeometry"), saveGeometry());
    settings.setValue(QStringLiteral("mainSplitterState"), splitter_->saveState());
    settings.setValue(QStringLiteral("topSplitterState"), topSplitter_->saveState());
    QMainWindow::closeEvent(event);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (watched == leftPanel_ && event->type() == QEvent::Resize) updateLeftSquarePreview();
    if (watched == pathBar_ && event->type() == QEvent::FocusIn) {
        // A plain selectAll() here gets immediately undone by the mouse-press event
        // that triggered this focus-in (it repositions the cursor to the click point,
        // collapsing the selection) - deferring to the next event loop turn lets that
        // click finish being processed first.
        QTimer::singleShot(0, pathBar_, &QLineEdit::selectAll);
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::updateLeftSquarePreview() {
    if (!leftPanel_ || !preview_) return;
    int width = leftPanel_->width();
    // Floor for the tree/bookmarks area so an extreme window aspect ratio (very tall,
    // narrow) can't squeeze it away to nothing in favor of an oversized square.
    constexpr int kMinTopHeight = 80;
    int maxPreviewHeight = qMax(0, leftPanel_->height() - kMinTopHeight);
    preview_->setFixedHeight(qMin(width, maxPreviewHeight));
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
    QModelIndex idx = gridModel_->index(row);
    grid_->setCurrentIndex(idx);
    grid_->scrollTo(idx, QAbstractItemView::PositionAtCenter);
    pendingSelectFileName_.clear();
}

void MainWindow::updateSelectionStatus() {
    // Folder-level aggregates - always shown when a folder is loaded, regardless of
    // whether anything is selected.
    int imageCount = gridModel_->imageCount();
    int videoCount = gridModel_->videoCount();
    int totalCount = imageCount + videoCount;
    folderStatsLabel_->setText(totalCount > 0 ? QStringLiteral("%1 items (%2 img, %3 vid)  ·  %4")
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
    rawStatusLabel_->setText(rawKnown > 0
                                  ? QStringLiteral("%1 RAW: %2 rendered, %3 preview").arg(rawKnown).arg(rawRendered).arg(rawPreview)
                                  : QString());

    QModelIndex idx = grid_->currentIndex();
    if (!idx.isValid()) {
        fileNameLabel_->clear();
        fileNameLabel_->setToolTip(QString());
        formatLabel_->clear();
        dimsLabel_->clear();
        sizeLabel_->clear();
        dateLabel_->clear();
        durationLabel_->clear();
        return;
    }

    QString name = idx.data(Qt::DisplayRole).toString();
    // Elided rather than left to wrap/clip raggedly - the label's fixed width means a
    // long filename would otherwise just get cut off mid-character. Full name still
    // available on hover.
    fileNameLabel_->setText(fileNameLabel_->fontMetrics().elidedText(name, Qt::ElideMiddle, fileNameLabel_->width()));
    fileNameLabel_->setToolTip(name);

    int fmt = idx.data(ThumbGridModel::FormatRole).toInt();
    formatLabel_->setText(QString::fromUtf8(pixet::formatName((pixet::Format)fmt)));

    int w = idx.data(ThumbGridModel::WidthRole).toInt();
    int h = idx.data(ThumbGridModel::HeightRole).toInt();
    if (w > 0 && h > 0) {
        QString dims = QStringLiteral("%1×%2").arg(w).arg(h);
        // Only known once the preview decode lands (see currentPreviewBpp_) - may be
        // briefly absent right after selecting, same as the async width/height fields.
        if (currentPreviewBpp_ > 0) dims += QStringLiteral(" (%1bpp)").arg(currentPreviewBpp_);
        dimsLabel_->setText(dims);
    } else {
        dimsLabel_->clear();
    }

    qint64 size = idx.data(ThumbGridModel::SizeRole).toLongLong();
    sizeLabel_->setText(size > 0 ? QLocale().formattedDataSize(size) : QString());

    qint64 takenAt = idx.data(ThumbGridModel::TakenAtRole).toLongLong();
    dateLabel_->setText(takenAt > 0 ? QDateTime::fromSecsSinceEpoch(takenAt).toString(QStringLiteral("yyyy-MM-dd hh:mm"))
                                     : QString());

    qint64 durationMs = idx.data(ThumbGridModel::DurationMsRole).toLongLong();
    if (durationMs > 0) {
        qint64 totalSec = durationMs / 1000;
        durationLabel_->setText(QStringLiteral("%1:%2").arg(totalSec / 60).arg(totalSec % 60, 2, 10, QChar('0')));
    } else {
        durationLabel_->clear();
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
    QSettings settings(QStringLiteral("pixet"), QStringLiteral("pixet"));

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
