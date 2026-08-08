#include "MainWindow.h"

#include <QAction>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHeaderView>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

#include "FolderIndexer.h"
#include "FolderTreeView.h"
#include "PreviewDecoder.h"
#include "PreviewPane.h"
#include "ThumbGridModel.h"
#include "ThumbGridView.h"
#include "ThumbLoader.h"
#include "db/Database.h"
#include "db/Schema.h"
#include "util/AppPaths.h"
#include "util/PathUtil.h"
#include "util/StringUtil.h"
#include "version.h"

namespace {
// dirs.path / files.name are UTF-8 in the DB; QFileSystemModel paths need the same
// backslash-normalized form pixet_core writes, or path-string lookups silently miss.
QString normalizeForDb(const QString &path) {
    return QString::fromStdWString(pixet::normalizePath(path.toStdWString()));
}
} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("pixet %1").arg(pixet::version()));
    resize(1280, 800);

    db_ = std::make_unique<pixet::Database>(pixet::indexDbPath(), pixet::thumbsDbPath(), false);

    // --- left panel: bookmarks + folder tree ---
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

    auto *leftPanel = new QWidget(this);
    auto *leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    auto *leftSplitter = new QSplitter(Qt::Vertical, leftPanel);
    leftSplitter->addWidget(bookmarks_);
    leftSplitter->addWidget(tree_);
    leftSplitter->setStretchFactor(0, 0);
    leftSplitter->setStretchFactor(1, 1);
    leftLayout->addWidget(leftSplitter);

    // --- center: thumbnail grid ---
    gridModel_ = new ThumbGridModel(*db_, this);
    grid_ = new ThumbGridView(this);
    grid_->setModel(gridModel_);
    grid_->setViewMode(QListView::IconMode);
    grid_->setResizeMode(QListView::Adjust);
    grid_->setMovement(QListView::Static);
    grid_->setIconSize(QSize(ThumbLoader::kThumbIconSize, ThumbLoader::kThumbIconSize));
    grid_->setGridSize(QSize(ThumbLoader::kThumbIconSize + 20, ThumbLoader::kThumbIconSize + 20));
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

    // --- right: preview pane ---
    preview_ = new PreviewPane(this);

    auto *splitter = new QSplitter(this);
    splitter->addWidget(leftPanel);
    splitter->addWidget(grid_);
    splitter->addWidget(preview_);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 0);
    splitter->setCollapsible(0, false);
    splitter->setCollapsible(2, false);
    splitter->setSizes({220, 800, 260});
    setCentralWidget(splitter);

    // Qt's first real layout pass (on show()) can override sizes set before the
    // widget has real geometry - reassert once after that pass actually happens.
    QTimer::singleShot(0, this, [splitter]() { splitter->setSizes({220, 800, 260}); });

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

    previewDebounce_ = new QTimer(this);
    previewDebounce_->setSingleShot(true);
    previewDebounce_->setInterval(80);
    connect(previewDebounce_, &QTimer::timeout, this, &MainWindow::triggerPreviewRequest);

    // --- menu ---
    auto *bookmarksMenu = menuBar()->addMenu(QStringLiteral("&Bookmarks"));
    bookmarksMenu->addAction(QStringLiteral("Add Current Folder"), this, &MainWindow::onAddBookmark, QKeySequence(QStringLiteral("Ctrl+D")));

    auto *viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    viewMenu->addAction(QStringLiteral("Refresh"), this, &MainWindow::onRefresh, QKeySequence(QStringLiteral("F5")));

    statusBar();

    loadBookmarks();
    restoreLastDirectory();
}

MainWindow::~MainWindow() = default;

void MainWindow::navigateTo(const QString &path, bool forceReindex) {
    QString normalized = normalizeForDb(path);
    if (normalized.isEmpty()) return;

    currentPath_ = normalized;
    preview_->clear();
    // Show whatever's already cached instantly. For a folder that's new or stale,
    // this shows 0 rows - onFilesListed (fired once Pass A commits, see FolderIndexer)
    // reloads again with the real file list, well before thumbnails are ready.
    gridModel_->setDirectory(normalized);

    QModelIndex idx = fsModel_->index(normalized);
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
    emit requestIndex(normalized, forceReindex);
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

void MainWindow::onGridSelectionChanged() {
    QModelIndex idx = grid_->currentIndex();
    if (!idx.isValid()) {
        pendingPreviewPath_.clear();
        preview_->clear();
        return;
    }

    QString name = idx.data(Qt::DisplayRole).toString();
    pendingPreviewFmt_ = idx.data(ThumbGridModel::FormatRole).toInt();
    pendingPreviewPath_ = currentPath_ + QStringLiteral("\\") + name;

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
}

void MainWindow::onAddBookmark() {
    if (!currentPath_.isEmpty()) addBookmark(currentPath_);
}

void MainWindow::onRefresh() {
    if (!currentPath_.isEmpty()) navigateTo(currentPath_, /*forceReindex=*/true);
}

void MainWindow::onIndexerStarted(QString path) {
    if (path == currentPath_) statusBar()->showMessage(QStringLiteral("Indexing %1...").arg(path));
}

void MainWindow::onFilesListed(QString path) {
    // Pass A just finished - the file list is final, even though thumbnails are
    // mostly still pending. Full reload: this is the one point where resetting the
    // model is correct, since nothing meaningful is on screen yet to flicker away.
    if (path == currentPath_) gridModel_->setDirectory(path);
}

void MainWindow::onThumbsProgress(QString path) {
    // A Pass B batch just landed - pull in the newly-ready thumbnails without
    // resetting the model, so already-displayed ones don't flicker.
    if (path != currentPath_) return;
    gridModel_->refreshThumbStates();
    grid_->viewport()->update(); // see the comment on the thumbReady connection above
}

void MainWindow::onIndexerFinished(QString path) {
    if (path != currentPath_) return;
    gridModel_->refreshThumbStates(); // catch any trailing batch
    grid_->viewport()->update();
    statusBar()->showMessage(QStringLiteral("%1 - %2 items").arg(path).arg(gridModel_->rowCount()), 4000);
}

void MainWindow::loadBookmarks() {
    bookmarks_->clear();
    auto sel = db_->prepare("SELECT id, path, label FROM bookmarks ORDER BY sort");
    while (sel.step()) {
        qint64 id = sel.columnInt64(0);
        QString path = QString::fromStdWString(pixet::toUtf16(sel.columnText(1)));
        QString label = QString::fromStdWString(pixet::toUtf16(sel.columnText(2)));
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
    ins.bind(1, pixet::toUtf8(path.toStdWString()));
    ins.bind(2, pixet::toUtf8(label.toStdWString()));
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
