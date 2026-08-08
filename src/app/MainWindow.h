#pragma once

#include <QImage>
#include <QMainWindow>

#include <memory>

class QTreeView;
class QFileSystemModel;
class QListWidget;
class QListWidgetItem;
class QListView;
class QModelIndex;
class QTimer;

class ThumbGridModel;
class ThumbLoader;
class PreviewDecoder;
class PreviewPane;
class FolderIndexer;

namespace pixet {
class Database;
}

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

signals:
    // Connected (queued, cross-thread) to FolderIndexer::indexFolder.
    void requestIndex(QString path, bool force);

private slots:
    void onTreeSelectionChanged(const QModelIndex &current);
    void onBookmarkClicked(QListWidgetItem *item);
    void onBookmarksContextMenu(const QPoint &pos);
    void onGridSelectionChanged();
    void onAddBookmark();
    void onRefresh();
    void onIndexerStarted(QString path);
    void onIndexerFinished(QString path);
    void onPreviewReady(qint64 requestId, QImage image);
    void triggerPreviewRequest();

private:
    std::unique_ptr<pixet::Database> db_; // main-thread: bookmarks CRUD + fast metadata queries

    QTreeView *tree_;
    QFileSystemModel *fsModel_;
    QListWidget *bookmarks_;
    QListView *grid_;
    ThumbGridModel *gridModel_;
    PreviewPane *preview_;

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

    void navigateTo(const QString &path, bool forceReindex = false);
    void loadBookmarks();
    void addBookmark(const QString &path);
    void removeBookmark(qint64 id);
    void restoreLastDirectory();
    void saveLastDirectory(const QString &path);
};
