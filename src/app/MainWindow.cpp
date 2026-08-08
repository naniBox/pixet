#include "MainWindow.h"

#include <QAction>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

#include "FolderIndexer.h"
#include "PreviewDecoder.h"
#include "PreviewPane.h"
#include "ThumbGridModel.h"
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

    tree_ = new QTreeView(this);
    tree_->setModel(fsModel_);
    tree_->setRootIndex(fsModel_->index(QString()));
    tree_->setHeaderHidden(true);
    for (int col = 1; col < fsModel_->columnCount(); ++col) tree_->hideColumn(col);
    connect(tree_->selectionModel(), &QItemSelectionModel::currentChanged, this, &MainWindow::onTreeSelectionChanged);

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
    grid_ = new QListView(this);
    grid_->setModel(gridModel_);
    grid_->setViewMode(QListView::IconMode);
    grid_->setResizeMode(QListView::Adjust);
    grid_->setMovement(QListView::Static);
    grid_->setIconSize(QSize(ThumbLoader::kThumbIconSize, ThumbLoader::kThumbIconSize));
    grid_->setGridSize(QSize(ThumbLoader::kThumbIconSize + 20, ThumbLoader::kThumbIconSize + 20));
    grid_->setUniformItemSizes(true);
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

    previewDecoder_ = std::make_unique<PreviewDecoder>();
    connect(previewDecoder_.get(), &PreviewDecoder::previewReady, this, &MainWindow::onPreviewReady);

    folderIndexer_ = std::make_unique<FolderIndexer>();
    connect(this, &MainWindow::requestIndex, folderIndexer_.get(), &FolderIndexer::indexFolder);
    connect(folderIndexer_.get(), &FolderIndexer::started, this, &MainWindow::onIndexerStarted);
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
    gridModel_->setDirectory(normalized); // show whatever's already cached, instantly

    QModelIndex idx = fsModel_->index(normalized);
    if (idx.isValid() && tree_->currentIndex() != idx) {
        tree_->setCurrentIndex(idx);
        tree_->scrollTo(idx);
    }

    saveLastDirectory(normalized);
    emit requestIndex(normalized, forceReindex);
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

void MainWindow::onIndexerFinished(QString path) {
    if (path != currentPath_) return;
    gridModel_->setDirectory(path);
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
