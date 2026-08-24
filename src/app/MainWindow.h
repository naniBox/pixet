#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QList>
#include <QMainWindow>
#include <QPixmap>
#include <QSet>

#include <memory>

#include "FileOpsWorker.h"

class QAction;
class QCloseEvent;
class QComboBox;
class QToolButton;
class QFileSystemModel;
class QFileSystemWatcher;
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

    // Entry point for a path handed to us by the OS rather than typed by the user - on macOS
    // a QEvent::FileOpen from Finder's "Open With", a file dropped on the Dock icon, or
    // `open -a pixet <path>` (see main.cpp's FileOpenForwarder). Accepts a directory or a
    // file; resolution is navigateToInput()'s, so this is a thin public door onto logic the
    // path bar already had.
    void openSystemPath(const QString &path);

signals:
    // Connected (queued, cross-thread) to FolderIndexer::indexFolder.
    void requestIndex(QString path, bool force, bool forceRethumbnail);
    // Connected (queued, cross-thread) to RawRenderer::prioritize.
    void requestRawRenderPriority(QString path);
    // Connected (queued, cross-thread) to BackgroundReconciler::triggerFullSweepNow -
    // the Preferences dialog's "Re-index Known Folders" button.
    void requestFullReindex();
    // Connected (queued, cross-thread) to FileOpsWorker::preflight/execute - see
    // onFilesDroppedOnGrid()/onFileOpPreflightReady() for the two-stage protocol.
    void requestFileOpPreflight(FileOpsWorker::Request req);
    void requestFileOpExecute(FileOpsWorker::Request req);
    // Connected (queued, cross-thread) to FileOpsWorker::deleteFiles - single-stage,
    // unlike the pair above (see FileOpsWorker::deleteFiles's own doc comment). Only
    // ever emitted after MainWindow's own confirmation dialog has already been
    // accepted.
    void requestFileDelete(FileOpsWorker::DeleteRequest req);

protected:
    void closeEvent(QCloseEvent *event) override;
    // Tracks which window the user is actually in - see WindowRegistry::noteActivated().
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onTreeSelectionChanged(const QModelIndex &current);
    void onBookmarkClicked(QListWidgetItem *item);
    void onBookmarksContextMenu(const QPoint &pos);
    // Right-click on the folder tree - bookmarks whatever folder is under the cursor, which
    // isn't necessarily the one currently being browsed. QTreeView doesn't change the
    // selection on a right-click, so this deliberately acts on indexAt(pos) rather than on
    // currentIndex().
    void onTreeContextMenu(const QPoint &pos);
    void onGridSelectionChanged();
    // Ctrl-hover over the grid (see ThumbGridView::ctrlHoverRowChanged) - previews
    // whatever's under the cursor without touching the actual selection, so a
    // multi-select in progress isn't disturbed. row == -1 means "stop peeking" (Ctrl
    // released, mouse left the grid, or moved over empty space) - reverts to
    // whatever's actually selected via onGridSelectionChanged().
    void onGridCtrlHoverChanged(int row);
    void onGridContextMenu(const QPoint &pos);
    // Double-click or Enter/Return on a thumbnail - opens the fullscreen viewer (P3).
    void onGridItemActivated(int row);
    void onPathBarReturnPressed();
    // A history entry was picked from the path bar's dropdown - navigates the same
    // way as typing a path and pressing Enter (see onPathBarReturnPressed()).
    void onPathBarHistoryActivated();
    // Tools > Purge Path History.
    void onPurgePathHistory();
    // Back/forward buttons (left of the path bar) and Alt+Left/Alt+Right - a plain
    // linear back/forward stack over navigateTo() calls, in-memory only (see
    // navHistory_'s doc comment) - deliberately separate from prefs::pathHistory()'s
    // persisted MRU dropdown, which is "recently visited folders" rather than "the
    // sequence of steps that got me here right now."
    void onNavigateBack();
    void onNavigateForward();
    void onEditSelectAll();
    void onEditCut();
    void onEditCopy();
    void onEditPaste();
    // Sends the current selection to the Recycle Bin/Trash (see util/FileMove.h's
    // moveToTrash()) after a confirmation dialog - guarded by focusedLineEdit() the
    // same way Cut/Copy/Paste are, so Delete removes a character while the path bar
    // has focus rather than deleting files.
    void onEditDelete();
    // F2 by default (see KeyBindings.cpp) - renames the single selected item via
    // promptRename() and the same preflight/CollisionDialog/execute pipeline every
    // other file op already goes through. No-ops (and the action is disabled - see
    // updateEditActionsEnabled()) unless exactly one item is selected.
    void onEditRename();
    void updateEditActionsEnabled();
    // Adds `row`'s cached info (ThumbGridView::cachedInfoText) plus, for a JPEG, a
    // synchronous on-demand EXIF read (see hoverinfo::readExifDetailsSync) as
    // disabled label entries at the top of the right-click context menu - and, if
    // the file has GPS coordinates, an enabled "Copy GPS Coordinates" action that
    // puts a Google-Maps-pasteable "lat,lon" string on the clipboard. No-op if
    // `row` is -1 (nothing under the cursor/selected).
    void addFileInfoToContextMenu(QMenu &menu, int row);
    // Right-click inside the fullscreen viewer. Built here rather than in FullscreenViewer
    // because everything it shows - cached info, the on-demand EXIF read, the Edit actions -
    // already lives on this side; the viewer only reports which row was clicked.
    void onFullscreenContextMenu(int row, QPoint globalPos);
    void onAddBookmark();
    void onRefresh();
    // Ctrl+D by default (see KeyBindings.cpp), configurable like the other
    // single-action-trigger keys. No visible menu entry - see focusAddressBarAction_'s
    // own comment for why. Gives the path bar focus and selects its text, matching
    // the "type a new destination immediately" convention address-bar-focus
    // shortcuts have in browsers.
    void onFocusAddressBar();
    void onForceRethumbnail();
    // Status bar thumbnail-size drop-down. Writes the preference, relayouts the grid, and
    // re-thumbnails the current folder only when the stored blobs can't satisfy the new
    // size - so shrinking, or any increase still covered by what's on disk, is instant.
    void onThumbSizeChanged();
    // The freshness dot next to it. Red means this folder has undersized thumbnails; green
    // means it doesn't. Clickable either way - on green it's a deliberate force-regenerate,
    // the same action otherwise buried in the grid's right-click menu.
    void onThumbStatusClicked();
    void onPreferences();
    // File > Choose Folder... - until now there was no folder picker anywhere in the app;
    // roots came only from the tree, the path bar, bookmarks and the startup fallback. That
    // works on Windows, where the tree opens on a short list of drive letters, but on macOS
    // an unrooted QFileSystemModel shows a single "/" and every launch starts four levels
    // from anywhere useful.
    void onChooseFolder();
    // Help > About (relocated into the application menu on macOS).
    void onAbout();
    // T key (View menu) - hides/shows leftPanel_ (tree, bookmarks, preview) so the
    // grid can take the full window width while hunting for a specific photo.
    void onToggleSidePanel();
    // View menu checkbox - enables/disables ThumbGridView's hover-delay tooltip (see
    // prefs::hoverInfoEnabled()). Hides any tooltip already on screen immediately
    // when turned off, rather than waiting for the next mouse move to notice.
    void onToggleHoverInfo(bool enabled);
    // Any of the 4 exclusive sort-key actions, or the reverse-order toggle - all wired
    // to this one slot (see the constructor) since it just re-reads which of them is
    // currently checked rather than needing to know which one fired. Shared by the
    // View > Sort By submenu and the sort button bar next to the path bar - both sets
    // of QToolButtons/QActions are the same QAction objects, so the two can't drift
    // out of sync with each other.
    void onSortOrderChanged();
    void onIndexerStarted(QString path);
    void onFilesListed(QString path);
    // Feed onCopyGridDebugInfo()'s navigation-timing section - see navThumbTimer_'s
    // doc comment. Connected alongside the pre-existing gridModel_/thumbLoader_
    // wiring in the constructor, not in place of it.
    void onNavThumbRequested(qint64 fileId, qint64 thumbId);
    void onNavThumbReady(qint64 fileId, QPixmap pixmap);
    void onThumbsProgress(QString path);
    void onIndexerFinished(QString path);
    void onIndexFailed(QString path, QString message);
    void onPreviewReady(qint64 requestId, QImage image);
    void triggerPreviewRequest();
    // QFileSystemModel populates directory contents asynchronously in the background;
    // an ancestor directory finishing its listing shifts every row below it. Reapplies
    // the tree top-positioning as that settles - see navigateTo()/repositionTreeToTop().
    // Only acts within navSettleTimer_'s window after the most recent navigateTo() -
    // see that member's doc comment for why this can't just run unconditionally for
    // the rest of the session.
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
    // The currently-open folder changed on disk from *outside* pixet - a file
    // manager's own paste of something pixet put on the clipboard (see
    // ClipboardOps), another program, or a second pixet instance. Fires often (once
    // per native change notification, which can be several for one logical
    // operation) - debounced through folderWatchDebounce_ before actually
    // triggering a rescan. See folderWatcher_'s member comment for why this exists
    // as a real-time watch rather than relying solely on BackgroundReconciler's slow
    // rotating sweep.
    void onWatchedDirectoryChanged(const QString &path);

    // Files dropped from Explorer/Finder onto the grid (see
    // ThumbGridView::filesDropped) - kicks off FileOpsWorker's preflight stage
    // targeting the currently-open folder.
    void onFilesDroppedOnGrid(QStringList localPaths, bool move);
    // Preflight stats are back - shows one CollisionDialog per conflict (honoring
    // "apply to all remaining"), then re-emits requestFileOpExecute() with every
    // item's resolution filled in. CancelAll (Escape/close on any dialog) drops the
    // whole batch rather than partially applying it.
    void onFileOpPreflightReady(FileOpsWorker::Request req, QStringList rejected);
    void onFileOpProgress(quint64 id, int done, int total, QString currentName);
    // The batch is done - if the destination is the folder currently on screen,
    // reflects the change incrementally (ThumbGridModel::removeFileById()/
    // insertOrUpdateFileByName()) rather than a full reload, so this doesn't lose
    // scroll position/selection the way a reset would.
    void onFileOpFinished(quint64 id, QString dstDirPath, QList<qint64> srcFileIds, QStringList addedNames,
                          int succeeded, int failed, QStringList errors);
    // The delete batch is done - drops removedFileIds from the grid model if this is
    // the folder currently on screen, same incremental-not-full-reload reasoning as
    // onFileOpFinished().
    void onFileDeleteFinished(quint64 id, QList<qint64> removedFileIds, int succeeded, int failed, QStringList errors);
    // A drag past the OS threshold started on the grid (see
    // ThumbGridView::dragOutRequested) - builds and exec()s the actual QDrag with
    // the selected files' real paths. pixet does not perform the physical move
    // itself here; Explorer/Finder does, as the drop target (see the .cpp for the
    // disclosed limitation on how much control pixet actually has over copy-vs-move).
    void onDragOutRequested();

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
    // Editable QComboBox, not a plain QLineEdit: shows/edits currentPath_ (or a full
    // file path - see onGridSelectionChanged()) exactly as before, but its dropdown
    // now doubles as recently-visited-folder history (prefs::pathHistory(), directories
    // only - see that function's doc comment) - see refreshPathBarHistory().
    QComboBox *pathBar_;
    // Back/forward, shown as toolbar buttons to pathBar_'s left. Fixed shortcuts
    // (Alt+Left/Alt+Right, not configurable - same reasoning as the Edit menu's
    // standard shortcuts in KeyBindings.h) carried by the QAction itself, which is
    // what the toolbar buttons display via QToolButton::setDefaultAction() - one
    // definition of the shortcut/enabled-state/icon shared by both, rather than
    // duplicating it between a button and a separate shortcut registration.
    QAction *backAction_;
    QAction *forwardAction_;
    // Plain linear back/forward stack over navigateTo() calls - every entry is a
    // normalized folder path, appended in recordNavHistory() (called from
    // navigateTo() unless navigatingViaHistory_ says a back/forward button is what's
    // driving this navigateTo() call, in which case the stack itself is already
    // correct and shouldn't be re-recorded/truncated). Deliberately in-memory only,
    // not persisted to prefs::settingsStore() - unlike the path bar's dropdown
    // history, "how did I get to this exact spot in this session" isn't something
    // worth remembering after pixet closes.
    QStringList navHistory_;
    int navHistoryIndex_ = -1; // navHistory_[navHistoryIndex_] == currentPath_, whenever the stack is non-empty
    bool navigatingViaHistory_ = false;
    // Restarted at the top of every navigateTo(). onTreeDirectoryLoaded() only
    // repositions the tree to currentPath_'s row while this is within
    // kTreeSettleWindowMs of that restart - without an expiry, it fires for *every*
    // directory the tree ever finishes listing, for the rest of the session, and
    // currentIndex() staying pinned to currentPath_ (clicking a branch's expand arrow
    // elsewhere doesn't change it) meant expanding some unrelated folder minutes
    // later would silently snap the view back to whatever's currently browsed - a
    // real reported bug, not a hypothetical.
    QElapsedTimer navSettleTimer_;
    static constexpr int kTreeSettleWindowMs = 4000; // just past the last fixed retry delay (3000ms) in navigateTo()

    // Restarted alongside navSettleTimer_ at the top of every navigateTo(). Exists so
    // onCopyGridDebugInfo() can report real thumbnail-fill timing for whatever
    // navigation is currently on screen, rather than needing a fresh reproduction
    // every time a "thumbnails took N seconds" report comes in (see that method's
    // doc comment on why a fast, always-available path for this matters). Tracks
    // distinct file ids rather than raw counts so a folder reload mid-navigation
    // (onFilesListed re-running setDirectory() once Pass A commits) doesn't double-
    // count the same file as two separate requests.
    QElapsedTimer navThumbTimer_;
    QSet<qint64> navThumbsRequested_;
    QSet<qint64> navThumbsReceived_;
    qint64 navFirstThumbMs_ = -1;  // elapsed ms from navigateTo() to the first thumbReady, or -1 if none yet
    qint64 navAllThumbsMs_ = -1;   // elapsed ms at the point requested/received counts last matched, or -1

    // Shortcuts are user-configurable (see KeyBindings.h) - kept as members so
    // applyKeyBindingShortcuts() can re-apply them after the Preferences dialog's
    // keybindings editor closes.
    QAction *refreshAction_;
    // Deliberately never added to a QMenu - unlike refreshAction_/toggleSidePanelAction_/
    // addBookmarkAction_, "focus the address bar" isn't a command anyone would look
    // for in a menu, just a keyboard convenience. Still goes through the exact same
    // QAction + applyKeyBindingShortcuts() machinery (added to the window itself via
    // addAction() instead, so its shortcut is live) so it's configurable in
    // PreferencesDialog like every other single-key action.
    QAction *focusAddressBarAction_;
    QAction *toggleSidePanelAction_;
    QAction *hoverInfoAction_;
    QAction *addBookmarkAction_;
    // Sort By: 4 mutually-exclusive (QActionGroup) key actions plus one independent
    // reverse-order toggle - see onSortOrderChanged(). Not part of the
    // applyKeyBindingShortcuts()/KeyBindings.h system (no default shortcuts assigned),
    // unlike refreshAction_ and friends above.
    QAction *sortByNameAction_;
    QAction *sortByFileDateAction_;
    QAction *sortByTakenDateAction_;
    QAction *sortBySizeAction_;
    QAction *sortReverseAction_;
    // Edit menu: Select All/Cut/Copy/Paste are fixed (non-remappable)
    // QKeySequence::StandardKey actions, wired directly rather than through
    // KeyBindings - see KeyBindings.h's class comment on why standard Edit
    // shortcuts live outside that system.
    QAction *selectAllAction_;
    QAction *cutAction_;
    QAction *copyAction_;
    QAction *pasteAction_;
    // Delete is a fixed QKeySequence::StandardKey action too - same category as the
    // four above (a standard, expected-everywhere Edit shortcut), not the configurable
    // keybindings:: system. Rename is the opposite: F2 isn't a StandardKey, and it's
    // exactly the kind of single-key-trigger action keybindings:: exists for (see
    // KeyBindings.cpp), so its shortcut comes from applyKeyBindingShortcuts() like
    // addBookmarkAction_/focusAddressBarAction_.
    QAction *deleteAction_;
    QAction *renameAction_;
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

    // Thumbnail size control at the far right of the status bar, plus a freshness dot for
    // the folder on screen. Deliberately not StatusBarRow cells: that widget exists to
    // hand-position fixed-width *text* so long values truncate rather than overlap (see its
    // class comment), a problem these two don't have.
    QComboBox *thumbSizeCombo_ = nullptr;
    QToolButton *thumbStatusButton_ = nullptr;
    // Last folder "auto rethumb" fired for, so it fires once per folder rather than every
    // time the freshness check re-runs - see updateThumbStatusIndicator().
    QString autoRethumbPath_;

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
    // BackgroundReconciler (the low-priority sweep that keeps indexed folders honest against
    // on-disk changes) and RawRenderer (the background upgrade of RAW files from a fast
    // embedded preview to a full demosaic render) used to be owned here, one pair per window.
    // They are now application-wide singletons - BackgroundReconciler::shared() /
    // RawRenderer::shared() - because both sweep the entire library regardless of what any
    // window is showing, so one pair per window meant N sweeps of the same rows. Each window
    // still connects to their directoryChanged() and filters it against its own folder.
    // The app's first code path that can move/copy/overwrite a real file - see
    // FileOpsWorker's class comment for the preflight/execute protocol.
    std::unique_ptr<FileOpsWorker> fileOps_;
    quint64 fileOpCounter_ = 0;
    // Set by onEditPaste() when the clipboard's contents were a Cut (not Copy);
    // consumed by onFileOpFinished() to clear the clipboard only once that paste
    // actually completes - reset early instead in onFileOpPreflightReady() if the
    // user cancels the collision dialog or every item got rejected, so a cancelled
    // paste doesn't lose the cut selection for nothing.
    bool pendingCutClipboardClear_ = false;

    // Real-time watch on currentPath_ (native OS notification - ReadDirectoryChangesW
    // on Windows - not polling), so a file that leaves or arrives from *outside*
    // pixet (most concretely: Cut in pixet, then paste in Explorer - the actual move
    // happens entirely in Explorer's process, so pixet has no other way to find out
    // it happened) is reflected without waiting for BackgroundReconciler's slow
    // rotating sweep (one directory every ~1.5s, full-library rest of ~10min between
    // cycles - fine as a self-healing backstop, far too slow to feel like "the UI
    // updated" for the folder actually on screen right now). Re-pointed at
    // currentPath_ every navigateTo(). One native notification can fire several
    // times for one logical operation (e.g. a multi-file paste), so
    // onWatchedDirectoryChanged() only starts folderWatchDebounce_ rather than
    // triggering a rescan directly.
    QFileSystemWatcher *folderWatcher_;
    QTimer *folderWatchDebounce_;
    QTimer *previewDebounce_;
    QString currentPath_;
    // Set by onIndexFailed(), shown by onIndexerFinished() - see the comment there on why it
    // can't just be shown when it arrives.
    QString pendingIndexError_;
    // Memoises countStaleThumbnails(), which is ~88ms on a large folder and asked twice per
    // navigation. -1 means "no valid cached answer"; see invalidateStaleCache().
    mutable QString staleCachePath_;
    mutable int staleCacheNeeded_ = 0;
    mutable int staleCacheValue_ = -1;
    void invalidateStaleCache();
    // Short, human-identifiable name for this window - the current folder's leaf name, with
    // the full path as a fallback at a filesystem root where there is no leaf. Used for both
    // the window title and its View > Windows entry so the two always agree.
    // How long a View-menu window entry is allowed to get. The current folder and its parent
    // are always shown in full even when that alone exceeds this - see windowMenuLabel().
    static constexpr int kWindowMenuLabelChars = 56;
    QString windowMenuLabel() const;
    // Just the current folder's name, for the window title - the Dock, the app switcher and
    // the window title bar all truncate from the right, so the long contextual form used in
    // the View menu would be actively worse there.
    QString windowLeafName() const;
    void updateWindowTitle();
    // The View menu itself, plus the separator that closes the Windows section. The window
    // entries live inline in View (not a submenu), so rebuilding them means inserting before
    // a fixed anchor and deleting exactly what was added last time - hence all three members.
    QMenu *viewMenu_ = nullptr;
    QAction *windowSectionEnd_ = nullptr;
    QList<QAction *> windowSectionActions_;
    QString pendingPreviewPath_;
    int pendingPreviewFmt_ = 0;
    qint64 previewRequestCounter_ = 0;
    qint64 currentPreviewRequestId_ = 0;
    // QImage::depth() of the most recently decoded preview - the only place bit depth
    // is available without adding a DB column (see updateSelectionStatus()). 0 until
    // the preview for the current selection actually lands.
    int currentPreviewBpp_ = 0;
    // True when the in-flight/most recent preview request (pendingPreviewPath_ at the
    // time triggerPreviewRequest() fired it, tracked forward via currentPreviewRequestId_)
    // is a Ctrl-hover peek (see ThumbGridView::ctrlHoverRowChanged) rather than the
    // real selection. onPreviewReady() uses this to show the image without touching
    // currentPreviewBpp_/updateSelectionStatus() - those describe the actual selected
    // item's status bar labels, which a transient hover peek elsewhere must not stomp.
    bool pendingPreviewIsCtrlHover_ = false;
    bool currentPreviewIsCtrlHover_ = false;
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
    // Repopulates pathBar_'s dropdown list from prefs::pathHistory(), preserving
    // whatever text is currently displayed in the edit line (which may not itself be
    // a history entry - e.g. a file path from the current grid selection). Called
    // once at startup and after every history mutation (a new folder visited, or an
    // explicit purge).
    void refreshPathBarHistory();
    // Appends `path` to navHistory_ at navHistoryIndex_+1, truncating any existing
    // "forward" entries first (standard browser convention: a fresh navigation from
    // a back'd-up position discards the redo stack) - unless `path` is already what
    // navHistoryIndex_ points at (a Refresh, or clicking the tree node already
    // selected), in which case this is a no-op rather than a duplicate consecutive
    // entry. Called from navigateTo(), guarded by navigatingViaHistory_.
    void recordNavHistory(const QString &path);
    void updateNavButtonsEnabled();
    // setDirectory() on the *same* path, with selection and scroll position carried
    // across the reset: captures the selected rows' file ids (and the lead row's)
    // beforehand, reloads, then re-derives row indices via
    // ThumbGridModel::rowForFileId() and restores the scrollbar's raw pixel value
    // afterward - safe here because the viewport width (and therefore column count)
    // is untouched by a same-path reload, unlike onToggleSidePanel(), which has to
    // re-center instead. Used wherever the row *set* for the currently-open folder
    // may have changed (Pass A completing, an explicit Refresh) but there's nothing
    // about the change a targeted insertOrUpdateFileByName()/removeFileById() call
    // could describe more precisely (see ThumbGridModel).
    void reloadGridPreservingSelection();

    // The per-item context menu, shared by the grid and the fullscreen viewer so "the usual
    // data" is literally the same menu in both rather than two lists that drift apart.
    // `fromFullscreen` drops the one entry that would be nonsense there ("View Fullscreen").
    void buildItemContextMenu(QMenu &menu, int row, bool fromFullscreen);

    // --- thumbnail size + freshness (status bar, far right) ---

    // Rebuilds the size drop-down's entries and selects prefs::thumbnailIconSize(),
    // inserting that value as an extra entry if it isn't one of the presets (the
    // Preferences spinbox allows any value in 80..400). Signal-blocked, so a programmatic
    // sync can't be mistaken for the user picking something.
    void syncThumbSizeCombo();

    // Applies whatever prefs::thumbnailIconSize() currently is to the live grid, and
    // re-thumbnails the current folder *only if* its stored blobs are too small for it.
    // Shared by the drop-down and the Preferences spinbox so the two can't diverge.
    void applyThumbnailSizeToGrid();

    // Number of thumbnails in `dirPath` too small to display sharply at the current size.
    // 0 means the folder is up to date. See the implementation for why this is judged
    // against the display requirement rather than prefs::thumbnailTargetLongEdge().
    int countStaleThumbnails(const QString &dirPath) const;

    // Long edge, in real device pixels, a thumbnail needs to fill a grid cell without being
    // upscaled.
    int displayThumbLongEdge() const;

    // Repaints the freshness dot (green = nothing to do, red = some thumbnails are
    // undersized here) and its tooltip. Folder-level, so it's called on folder-level events
    // only - not from updateSelectionStatus(), which runs on every selection change and
    // would turn this into a DB query per arrow-key press.
    void updateThumbStatusIndicator();
    void updateSelectionStatus();
    void repositionTreeToTop(const QModelIndex &idx);
    void loadBookmarks();
    void addBookmark(const QString &path);
    // The bookmarks table has no UNIQUE on path, so nothing at the storage level stops the
    // same folder being added twice - this is what the UI checks against instead.
    bool isBookmarked(const QString &path) const;
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

    // A menu-bar QAction's shortcut has Qt::WindowShortcut context and is dispatched
    // *before* key events reach the focused widget - and QLineEdit implements
    // Ctrl+A/C/X/V in its own keyPressEvent, not via actions. Every Edit-menu handler
    // checks this first and forwards to the line edit's own selectAll()/copy()/
    // cut()/paste() instead - otherwise Ctrl+A while editing the path bar would
    // select grid thumbnails instead of the path text.
    QLineEdit *focusedLineEdit() const;

    // A small QDialog (not QInputDialog, which can't pre-select text) prompting for a
    // new name - pre-fills `currentName` and pre-selects just the stem (before the
    // extension), matching Explorer's own rename UX. Returns the new name, or an empty
    // string if cancelled/unchanged (onEditRename() treats both the same way - nothing
    // to do).
    QString promptRename(const QString &currentName);

    // Window position/size and splitter layout persistence (QSettings) - see
    // restoreWindowState()'s doc comment in the .cpp for the off-screen/reset behavior.
    void restoreWindowState();
    // The write half, factored out of closeEvent() because closeEvent is not guaranteed to
    // run: on macOS, Cmd+Q / "Quit pixet" terminates via the application menu without
    // necessarily delivering a close event to the window, which would silently stop
    // persisting geometry and the last directory. Also called from aboutToQuit.
    void saveWindowState();
    bool isWindowOnScreen() const;

    // TODO: was debug-build-only; kept in release too for now (2026-08-11) so it's
    // available on the daily-driver build without a separate debug build/relaunch.
    // Reconsider before any wider distribution - copies window/splitter/grid
    // geometry, DPI, and content counts to the clipboard as plain text, which is
    // harmless but not something an end user needs to see. See the &Debug menu in
    // the constructor. Exists specifically for the grid column-fit bug: rebuilding
    // this info from a live repro is slow and every past attempt at reproducing it
    // synthetically turned out not to match whatever the user was actually seeing.
    // Also now reports navThumbTimer_'s navigation-timing numbers (added during a
    // thumbnail-loading scalability pass - see devlog), for exactly the same reason:
    // if a slow-thumbnail-fill report ever comes in again, this is the fast path to
    // real numbers (requested/received counts, time to first thumbnail, time to
    // catch up) for whatever folder is on screen, instead of another from-scratch
    // reproduction attempt.
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
    // File > New Window (Cmd/Ctrl+N). Opens on this window's current folder - see the
    // implementation for why that beats the persisted lastDirectory.
    void onNewWindow();
    // Rebuilds View > Windows. Connected both to WindowRegistry::changed() (so the list is
    // correct even while the menu is closed, which matters for the Cmd+W-then-reopen case) and
    // to the submenu's own aboutToShow (so the checkmark and folder names are fresh).
    void rebuildWindowsMenu();
    void onCopyGridDebugInfo();
    // Debug > Copy Profile Report. Only present when built -DPIXET_PROFILE=ON (see
    // util/Profile.h); copies the scope/counter/timeline dump for the navigation just
    // performed, since navigateTo() resets the profiler on every folder change.
    void onCopyProfileReport();
};
