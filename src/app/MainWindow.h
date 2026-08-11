#pragma once

#include <QImage>
#include <QMainWindow>

#include <memory>

class QAction;
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
class StatusLabel;
class ThumbGridModel;
class ThumbGridView;
class ThumbLoader;
class PreviewDecoder;
class PreviewPane;
class FolderIndexer;
class BackgroundReconciler;
class RawRenderer;
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
    // Connected (queued, cross-thread) to RawRenderer::prioritize.
    void requestRawRenderPriority(QString path);
    // Connected (queued, cross-thread) to BackgroundReconciler::triggerFullSweepNow -
    // the Preferences dialog's "Re-index Known Folders" button.
    void requestFullReindex();

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
    void onGridItemActivated(int row);
    void onPathBarReturnPressed();
    void onAddBookmark();
    void onRefresh();
    void onForceRethumbnail();
    void onPreferences();
    // T key (View menu) - hides/shows leftPanel_ (tree, bookmarks, preview) so the
    // grid can take the full window width while hunting for a specific photo.
    void onToggleSidePanel();
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
    // BackgroundReconciler found and corrected drift in a directory, or RawRenderer
    // upgraded a RAW file to a full render there - refresh the grid (and the RAW
    // rendered/preview status) if that's the one currently on screen (no-op otherwise).
    // Connected to both workers' directoryChanged signals - same handling either way.
    void onBackgroundDirectoryChanged(QString path);

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
    // Shortcuts are user-configurable (see KeyBindings.h) - kept as members so
    // applyKeyBindingShortcuts() can re-apply them after the Preferences dialog's
    // keybindings editor closes.
    QAction *refreshAction_;
    QAction *toggleSidePanelAction_;
    QAction *addBookmarkAction_;
    // Status bar: separate labels (capped, not fixed, width - see makeStatusLabel()
    // in the constructor) rather than one joined string, so browsing (arrow keys,
    // clicking through images) doesn't visually jitter as filenames/values change
    // length - see updateSelectionStatus().
    StatusLabel *folderStatsLabel_;
    // Empty unless the current folder has any RAW files - see updateSelectionStatus().
    StatusLabel *rawStatusLabel_;
    StatusLabel *fileNameLabel_;
    StatusLabel *formatLabel_;
    StatusLabel *dimsLabel_;
    StatusLabel *sizeLabel_;
    StatusLabel *dateLabel_;
    StatusLabel *durationLabel_;

    // splitter_: left column (tree+bookmarks+preview) vs. the thumbnail grid, 40/60.
    // topSplitter_: within the left column's top area, tree vs. bookmarks, 70/30.
    // leftSplitter_: within the left column, top area vs. preview pane (vertical) -
    // user-draggable, state persisted the same way as the other two (see
    // closeEvent()/the constructor's deferred restore block).
    // leftPanel_: just a thin QWidget wrapper around leftSplitter_ so onToggleSidePanel()
    // has a single thing to show/hide.
    QSplitter *splitter_;
    QSplitter *topSplitter_;
    QSplitter *leftSplitter_;
    QWidget *leftPanel_;
    bool resetLayout_;

    // No QObject parent - moveToThread() silently fails (Qt just warns to stderr,
    // invisible in a WIN32-subsystem app) on an object that already has a parent, so
    // these must be parentless to actually run on their own threads. Cleaned up
    // manually via unique_ptr instead of Qt's parent-child ownership.
    std::unique_ptr<ThumbLoader> thumbLoader_;
    std::unique_ptr<PreviewDecoder> previewDecoder_;
    std::unique_ptr<FolderIndexer> folderIndexer_;
    // Low-priority background sweep that keeps already-indexed folders honest against
    // files changed on disk outside pixet - see BackgroundReconciler's class comment.
    std::unique_ptr<BackgroundReconciler> backgroundReconciler_;
    // Low-priority background upgrade of RAW files from their fast embedded-preview
    // thumbnail to a full demosaic render - see RawRenderer's class comment.
    std::unique_ptr<RawRenderer> rawRenderer_;

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
    // "Reset Index" (Preferences dialog, already confirmed there) - deletes every
    // scanned folder/file/thumbnail row (dirs, files, claims, journal,
    // thumbs.thumbs), deliberately leaving bookmarks alone, then VACUUMs both
    // schemas to actually reclaim disk space. Runs synchronously on db_ (the same
    // connection bookmarks/metadata queries already use) - a rare, deliberate,
    // user-confirmed action, not something worth a background worker for.
    void nukeDatabase();

    // Re-reads keybindings::binding() for refreshAction_/toggleSidePanelAction_/
    // addBookmarkAction_ and applies it via QAction::setShortcut() - called once at
    // construction and again after the Preferences dialog's keybindings editor
    // closes, since a QAction's shortcut doesn't update itself when the underlying
    // setting changes.
    void applyKeyBindingShortcuts();

    // Window position/size and splitter layout persistence (QSettings) - see
    // restoreWindowState()'s doc comment in the .cpp for the off-screen/reset behavior.
    void restoreWindowState();
    bool isWindowOnScreen() const;

    // TODO: was debug-build-only; kept in release too for now (2026-08-11) so it's
    // available on the daily-driver build without a separate debug build/relaunch.
    // Reconsider before any wider distribution - copies window/splitter/grid
    // geometry, DPI, and content counts to the clipboard as plain text, which is
    // harmless but not something an end user needs to see. See the &Debug menu in
    // the constructor. Exists specifically for the grid column-fit bug: rebuilding
    // this info from a live repro is slow and every past attempt at reproducing it
    // synthetically turned out not to match whatever the user was actually seeing.
    // Never remove this - keep it around permanently as the fast path for "it
    // happened again, here's the exact state." Deliberately a plain method, not a
    // slot: moc doesn't reliably see the same NDEBUG definition the real compiler
    // does (it comes from CMake's default CMAKE_CXX_FLAGS_RELWITHDEBINFO string, not
    // a target_compile_definitions() entry AUTOMOC picks up), so an
    // NDEBUG-conditional slots: declaration risks moc/compiler disagreeing on
    // whether the method exists. Moc never looks at plain (non-slot) methods at all,
    // so this sidesteps that entirely - the modern pointer-to-member connect()
    // syntax used to wire up the menu action doesn't require its target to be a
    // registered slot either.
    void onCopyGridDebugInfo();
};
