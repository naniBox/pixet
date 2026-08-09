#pragma once

#include <QImage>
#include <QMainWindow>

#include <memory>

class QCloseEvent;
class QFileSystemModel;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QModelIndex;
class QSplitter;
class QTimer;

class FolderTreeView;
class ThumbGridModel;
class ThumbGridView;
class ThumbLoader;
class PreviewDecoder;
class PreviewPane;
class FolderIndexer;
class BackgroundReconciler;
class FullscreenViewer;

namespace pixet {
class Database;
}

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(bool resetLayout = false, QWidget *parent = nullptr);
    ~MainWindow() override;

signals:
    // Connected (queued, cross-thread) to FolderIndexer::indexFolder.
    void requestIndex(QString path, bool force, bool forceRethumbnail);

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onTreeSelectionChanged(const QModelIndex &current);
    void onBookmarkClicked(QListWidgetItem *item);
    void onBookmarksContextMenu(const QPoint &pos);
    void onGridSelectionChanged();
    void onGridContextMenu(const QPoint &pos);
    // Double-click or Enter/Return on a thumbnail - opens the fullscreen viewer (P3).
    void onGridItemActivated(const QModelIndex &index);
    void onPathBarReturnPressed();
    void onAddBookmark();
    void onRefresh();
    void onForceRethumbnail();
    void onIndexerStarted(QString path);
    void onFilesListed(QString path);
    void onThumbsProgress(QString path);
    void onIndexerFinished(QString path);
    void onPreviewReady(qint64 requestId, QImage image);
    void triggerPreviewRequest();
    // QFileSystemModel populates directory contents asynchronously in the background;
    // an ancestor directory finishing its listing shifts every row below it. Reapplies
    // the tree top-positioning as that settles - see navigateTo()/repositionTreeToTop().
    void onTreeDirectoryLoaded(const QString &path);
    // Ctrl+arrow from the grid (see ThumbGridView::navigateFolderRequested) - up/down
    // to the previous/next sibling folder, left to the parent, right into the first
    // subfolder.
    void onNavigateFolderRequested(Qt::Key direction);

private:
    std::unique_ptr<pixet::Database> db_; // main-thread: bookmarks CRUD + fast metadata queries

    FolderTreeView *tree_;
    QFileSystemModel *fsModel_;
    QListWidget *bookmarks_;
    ThumbGridView *grid_;
    ThumbGridModel *gridModel_;
    PreviewPane *preview_;
    // Runs on the main thread like any other widget (only its internal decoder is a
    // background worker) - a normal parented QWidget is fine, unlike the
    // thumbLoader_/previewDecoder_/folderIndexer_ workers below. Created once, reused
    // (shown/hidden) for every fullscreen session rather than per-activation.
    FullscreenViewer *fullscreenViewer_;
    QLineEdit *pathBar_;
    // Status bar: separate fixed-width labels rather than one joined string, so
    // browsing (arrow keys, clicking through images) doesn't visually jitter as
    // filenames/values change length - see updateSelectionStatus().
    QLabel *folderStatsLabel_;
    QLabel *fileNameLabel_;
    QLabel *formatLabel_;
    QLabel *dimsLabel_;
    QLabel *sizeLabel_;
    QLabel *dateLabel_;
    QLabel *durationLabel_;

    // splitter_: left column (tree+bookmarks+preview) vs. the thumbnail grid, 40/60.
    // topSplitter_: within the left column's top area, tree vs. bookmarks, 70/30.
    // leftPanel_: the widget whose width drives the preview pane's forced-square
    // height - see updateLeftSquarePreview().
    QSplitter *splitter_;
    QSplitter *topSplitter_;
    QWidget *leftPanel_;
    bool resetLayout_;

    // No QObject parent - moveToThread() silently fails (Qt just warns to stderr,
    // invisible in a WIN32-subsystem app) on an object that already has a parent, so
    // these must be parentless to actually run on their own threads. Cleaned up
    // manually via unique_ptr instead of Qt's parent-child ownership.
    std::unique_ptr<ThumbLoader> thumbLoader_;
    std::unique_ptr<PreviewDecoder> previewDecoder_;
    std::unique_ptr<FolderIndexer> folderIndexer_;

    QTimer *previewDebounce_;
    QString currentPath_;
    QString pendingPreviewPath_;
    int pendingPreviewFmt_ = 0;
    qint64 previewRequestCounter_ = 0;
    qint64 currentPreviewRequestId_ = 0;
    // QImage::depth() of the most recently decoded preview - the only place bit depth
    // is available without adding a DB column (see updateSelectionStatus()). 0 until
    // the preview for the current selection actually lands.
    int currentPreviewBpp_ = 0;
    // Set by navigateToInput() when the path bar was given a *file* path rather than
    // a directory - applied (and cleared) as soon as the grid model has that file's
    // row, which may not be immediate on a folder that needs indexing. See
    // trySelectPendingFile(), called from every place the grid model gets reloaded.
    QString pendingSelectFileName_;

    void navigateTo(const QString &path, bool forceReindex = false, bool forceRethumbnail = false);
    // Path bar submission handler: resolves `input` as either a directory (navigate
    // into it) or a file (navigate to its parent, then select the file once the grid
    // has it - see pendingSelectFileName_). Invalid input leaves currentPath_ alone
    // and just reports the problem in the status bar.
    void navigateToInput(const QString &input);
    void trySelectPendingFile();
    void updateSelectionStatus();
    void repositionTreeToTop(const QModelIndex &idx);
    void loadBookmarks();
    void addBookmark(const QString &path);
    void removeBookmark(qint64 id);
    void restoreLastDirectory();
    void saveLastDirectory(const QString &path);

    // Window position/size and splitter layout persistence (QSettings) - see
    // restoreWindowState()'s doc comment in the .cpp for the off-screen/reset behavior.
    void restoreWindowState();
    bool isWindowOnScreen() const;
    // Forces preview_'s height to match leftPanel_'s current width, so the pane stays
    // square as the window or the main splitter is resized. Called from eventFilter()
    // whenever leftPanel_ itself resizes.
    void updateLeftSquarePreview();
};
