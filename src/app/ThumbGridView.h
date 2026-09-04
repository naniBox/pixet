#pragma once

#include <QAbstractScrollArea>
#include <QBitArray>
#include <QKeySequence>
#include <QList>
#include <QPair>
#include <QPoint>

#include <memory>

class QAbstractItemModel;
class QDragEnterEvent;
class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;
class QTimer;
class HoverInfoWorker;

// Fully custom-painted, virtualized thumbnail grid, deliberately not built on QListView:
// its IconMode flow-layout (Qt's own internal algorithm for deciding how many same-size
// cells fit per row) sometimes disagrees with a plain, exact, no-remainder division by a
// column count. Confirmed via a real
// side-by-side repro, not a hypothetical: a 748px-wide viewport with 187px cells
// rendered 4 columns (4*187=748, exact); an 8px-wider, 756px viewport with 189px
// cells - an equally exact division, 4*189=756 - rendered only 3, with
// devicePixelRatio()==1 in both cases, ruling out DPI-scaling rounding as the cause.
// There's no way to predict that kind of internal-to-Qt rounding from the outside, so
// rather than trying to guess it correctly - a guess-then-verify-then-backoff loop against
// exactly this only moves the unpredictability into a different, more fragile shape - this
// class doesn't ask IconMode to lay anything out at all. It computes every cell's
// row/column directly (row = index / columns, col = index % columns) and paints
// visible cells itself, so there is no second layout engine left that could ever
// disagree with the column count this class decided on - "computed" and "actually
// rendered" are the same number by construction, not something to separately verify
// (see debugComputedColumns()/debugRenderedColumnCount(), kept identical on purpose).
//
// Model access is through a plain QAbstractItemModel (ThumbGridModel, a flat
// QAbstractListModel) for the paint/selection/layout path - only DisplayRole/
// DecorationRole are read there, so that path stays model-agnostic even though
// ThumbGridModel now supports mid-lifetime row insert/remove (see rowsInserted/
// rowsRemoved handling below) in addition to the original modelReset/dataChanged
// paths. The hover-info tooltip is the one deliberate exception - it reads
// ThumbGridModel's own custom roles (format/size/dimensions/taken-at/duration)
// directly, the same way MainWindow already does, rather than inventing a parallel
// delegation mechanism to preserve agnosticism for a model this view is in practice
// never used with any other implementation of.
//
// Selection is multi-row (see currentRow()/selectedRows() below), not the single-row
// design this class started with - but there is still exactly one "lead" row at any
// time (currentRow()), which is what the preview pane/status bar/path bar follow.
class ThumbGridView : public QAbstractScrollArea {
    Q_OBJECT

public:
    explicit ThumbGridView(QWidget *parent = nullptr);
    ~ThumbGridView() override; // out-of-line: hoverInfoWorker_ is a unique_ptr to a type only forward-declared here

    void setModel(QAbstractItemModel *model);
    QAbstractItemModel *model() const { return model_; }

    // Needed to turn a hovered row's filename (all the model itself knows) into a
    // full path for the hover tooltip's on-demand EXIF read - see
    // HoverInfoWorker. Call whenever the displayed folder changes (MainWindow does,
    // right alongside setDirectory()); purely metadata plumbing, doesn't touch what's
    // displayed.
    void setCurrentFolderPath(const QString &path);

    // Filename/format/dimensions/size/taken-date/duration for `row` - everything
    // already in the model, no I/O needed. Same text the hover tooltip shows;
    // MainWindow's right-click context menu reuses this so the two "show me info
    // about this file" surfaces can't drift apart.
    QString cachedInfoText(int row) const;

    // Immediately hides any hover tooltip currently showing and cancels a pending
    // one - called by MainWindow right after the user unchecks "Show Hover Info" in
    // the View menu, since prefs::hoverInfoEnabled() is only checked lazily on the
    // next mouse move otherwise (a tooltip already on screen when the setting
    // changes would otherwise linger until the mouse next moves off the cell).
    void hideHoverTooltip();

    // Re-reads the configurable folder-navigation bindings (see KeyBindings.h). The
    // constructor does the initial load; MainWindow calls this again after the
    // Preferences dialog writes new ones, alongside re-applying its own QAction
    // shortcuts. Cached rather than read per key press because keyPressEvent runs for
    // every key including auto-repeating arrows, and that would be four QSettings
    // lookups an event to answer a question whose answer only changes in a dialog.
    void reloadKeyBindings();

    // Square icons only - matches prefs::thumbnailIconSize(), the only way this is
    // ever actually driven. Recomputes the grid layout immediately, unlike QListView's own
    // setIconSize() (a plain property with no side effect), so there is no separate
    // apply-the-change call for a caller to forget.
    void setIconSize(QSize size);
    QSize iconSize() const;

    // -1 = no selection. The "lead" row: the keyboard cursor, and the most-recently-
    // selected row - this is what the preview pane, the status bar's per-file detail
    // fields, and the path bar all follow, even when the selection has more than one
    // row. Invariant: currentRow() is always a selected row, or -1 (never a
    // deselected leftover) - that's what makes "preview shows the latest selection"
    // well defined.
    int currentRow() const { return currentRow_; }
    // Replaces the whole selection with just `row` (or clears it, for -1). This is
    // the pre-multi-select meaning of this method, kept as-is: every existing caller
    // (FullscreenViewer's grid sync, trySelectPendingFile) wants exactly "select this
    // one file", not an addition to whatever else might be selected.
    void setCurrentRow(int row);

    bool isRowSelected(int row) const;
    int selectionCount() const { return selectedCount_; }
    // Ascending row order, built on demand.
    QList<int> selectedRows() const;
    void selectAll();
    void clearSelection();
    // Restores a selection wholesale - used after a reload re-derives row indices
    // from file ids (see MainWindow::reloadGridPreservingSelection). Rows outside
    // [0, rowCount()) are ignored; if `currentRow` isn't itself one of `rows`, the
    // lead falls back to the first surviving selected row.
    void setSelection(const QList<int> &rows, int currentRow);

    // Ensures a full extra row stays visible above and below `row` (when there's
    // enough content for that to mean anything) rather than just barely bringing it
    // into view - so arrow-key browsing near the top/bottom edge always shows a
    // preview of what's coming next. `center` (jumping to a specific file from the
    // path bar) instead centers it directly, no nudging.
    void scrollToRow(int row, bool center = false);

    // First and last model row currently intersecting the viewport, inclusive, or
    // (-1, -1) when nothing is displayed. This is the same range paintEvent() draws -
    // it derives its own loop bounds from this call rather than recomputing them, so
    // "visible" can't come to mean two different things.
    //
    // Rows here are model rows, not grid rows: the first and last are the ends of a
    // partially-visible row of cells, so the range is a little generous at both edges,
    // which is what a caller prefetching against it wants anyway.
    QPair<int, int> visibleRowRange() const;

    // TODO: debug-build-only would be the natural gating; in release too for now
    // (2026-08-11), see MainWindow::onCopyGridDebugInfo(). The two are always equal by
    // construction (see the class comment). Kept as two separate calls anyway so the debug
    // dump can state that plainly: "computed" versus "actually rendered" is the exact
    // discrepancy the dump exists to catch, so collapsing them to one would hide it.
    int debugComputedColumns() const { return columns_; }
    int debugRenderedColumnCount() const { return columns_; }
    QSize debugCellSize() const { return QSize(cellWidth_, cellHeight_); }

signals:
    // The lead row changed - kept with its pre-multi-select meaning (preview/path
    // bar/status-bar-detail follow this) so none of that existing wiring needed to
    // change. A gesture that extends the selection without moving the lead (e.g. a
    // Ctrl+click that only toggles some other row) does not emit this.
    void currentRowChanged(int row);
    // The selected set changed - membership and/or count. Drives the status bar's
    // aggregate line and Cut/Copy's enabled state. Emitted independently of
    // currentRowChanged (a Ctrl+click that extends the selection without moving the
    // lead emits only this).
    void selectionChanged();
    // Enter/Return or double-click on a row.
    void activated(int row);
    // Ctrl+arrow is folder navigation (see MainWindow), not grid navigation - this
    // view just recognizes the chord and hands off the direction; it doesn't know
    // anything about folders/trees itself.
    void navigateFolderRequested(Qt::Key direction);
    // External files were dropped on the grid from Explorer/Finder. `move` is true
    // unless Ctrl was held at drop time - deliberately the opposite of Windows' own
    // OS-level default for a cross-application drag (a plain drag into a different
    // app normally proposes Copy there), overridden because dragging photos into a
    // library should move them in by default, not leave a duplicate behind; Ctrl
    // (matching the Explorer-native override key for the same reason) copies
    // instead. The view performs no I/O itself - MainWindow owns the FileOpsWorker
    // that actually copies/moves them into the current folder.
    void filesDropped(QStringList localPaths, bool move);
    // A press-and-drag past the OS drag threshold started on a selected row - the
    // view can't build the drag payload itself (it only knows filenames, not the
    // folder path), so MainWindow constructs and exec()s the actual QDrag.
    void dragOutRequested();
    // The set of rows visibleRowRange() would return may have changed - a scroll, a
    // resize, an icon-size change, or a model reset. Deliberately not de-duplicated
    // against the previous range: the receiver (MainWindow, telling the indexer which
    // thumbnails to generate first) does trivial work per emission, and suppressing
    // "same range" here would also suppress the case where the range is the same but
    // the *rows* in it are different files, which is exactly what a model reset is.
    void visibleRowsChanged();
    // Ctrl is held and the mouse moved onto a different cell than last reported (or
    // Ctrl was released, or the mouse left the grid, or moved over empty space) -
    // row is -1 for all of the latter cases, meaning "stop peeking, go back to
    // showing whatever's actually selected." Deliberately independent of
    // selected_/currentRow_ - this lets the preview pane show any thumbnail under
    // the cursor without disturbing an in-progress multi-select, which is the whole
    // point (see MainWindow::onGridCtrlHoverChanged).
    void ctrlHoverRowChanged(int row);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    // Small map-marker silhouette drawn before the filename for a geotagged file, sized from
    // `box` (font-height derived) so it scales with the UI font. See the implementation.
    void drawGeotagPin(class QPainter &painter, const QRect &box) const;

    static constexpr int kCellPadding = 8;    // margin around the image area
    static constexpr int kTextTopGap = 4;     // gap between image area and filename
    static constexpr int kTextRowHeight = 18; // reserved height for the filename line

    QAbstractItemModel *model_ = nullptr;
    int iconSize_ = 150;

    // The configured key for each folder-tree direction - see reloadKeyBindings(). The
    // direction is the Qt::Key handed to navigateFolderRequested(), which is the
    // *meaning* (parent/next/...) rather than the key actually pressed: they coincide
    // only for the arrow-key defaults.
    struct FolderNavBinding {
        QKeySequence sequence;
        Qt::Key direction;
    };
    QList<FolderNavBinding> folderNavBindings_;

    // selected_ is always sized == rowCount() - a bitset rather than e.g. QSet<int>
    // because paintCell() asks "is this row selected?" for every visible cell on
    // every repaint (a bit test beats a hash lookup), Select All over a huge folder
    // is one fill(true) rather than N hash inserts, and it costs only ~1 bit/row.
    // selectedCount_ is maintained incrementally alongside it so selectionCount() is
    // O(1) rather than a full scan.
    QBitArray selected_;
    int selectedCount_ = 0;
    int currentRow_ = -1; // the lead row - see the currentRow() doc comment above
    int anchorRow_ = -1;  // shift-click/shift-arrow range origin; -1 when nothing's selected

    // Set when the model resets, cleared by the next applySelectionResult().
    //
    // A reset forces currentRow_ to -1 without emitting anything (there is no
    // meaningful row to report yet, and the caller is about to restore a selection),
    // which makes the row number a useless identity across one: the caller's
    // "did it move?" comparison ends up between two post-reset values. It reads as
    // unchanged even though every row now refers to a different file, or to none.
    // applySelectionResult() consults this so the restore after a reset always
    // notifies, whatever the numbers happen to be.
    bool resetSinceNotify_ = false;

    // mousePressEvent/mouseReleaseEvent state for the deferred-collapse gesture: a
    // plain click on an *already*-selected row doesn't collapse the selection
    // immediately (see mousePressEvent) - only on release, and only if nothing else
    // (a drag) claimed the press first. pressRow_/pressPos_ are also what a future
    // drag-out gesture needs to know where the press started.
    int pressRow_ = -1;
    int pendingCollapseRow_ = -1;
    QPoint pressPos_;

    // True while an external drag (files from Explorer/Finder) is hovering the
    // viewport - drawn as a highlight border in paintEvent(), cleared on
    // dragLeaveEvent()/dropEvent(). Not selection state; unrelated to selected_.
    bool dragActive_ = false;
    // Live modifier state of the drag currently hovering, kept in sync on every
    // dragMoveEvent (Ctrl can be pressed/released mid-drag) - true means Ctrl is
    // held, i.e. this drop will Copy rather than the default Move. Drives
    // drawDropFeedback()'s border color, the same way the cursor's own OS-drawn
    // copy/move badge already reflects it natively.
    bool dragCopyMode_ = false;

    // Last row reported via ctrlHoverRowChanged() - -1 means either Ctrl isn't
    // currently held or the mouse isn't over a cell. Independent of hoverRow_ (the
    // info-tooltip's own hover tracking) - the two features share mouseMoveEvent but
    // don't otherwise interact.
    int ctrlHoverRow_ = -1;

    // Hover-info tooltip: deliberately a self-managed timer rather than Qt's own
    // QEvent::ToolTip/QToolTip auto-trigger - this class extends QAbstractScrollArea
    // directly, not QAbstractItemView, so it doesn't get QAbstractItemView's
    // automatic Qt::ToolTipRole handling, and relying on the OS/QPA-level "has the
    // mouse gone idle" heuristic that drives QEvent::ToolTip for a fully
    // custom-painted widget like this one is much less certain to fire reliably
    // than just tracking it explicitly. Also gives direct control over the delay
    // ("after a short delay" was the actual ask) rather than inheriting whatever
    // QApplication::toolTipWait() defaults to.
    QTimer *hoverDelayTimer_ = nullptr;
    QPoint hoverPos_;   // viewport-relative, where to anchor the tooltip once the timer fires
    int hoverRow_ = -1; // -1 = not currently hovering (or about to show a tooltip for) any cell
    QString hoverFolderPath_; // see setCurrentFolderPath()
    // Parentless like every other worker in this app - see MainWindow's own worker
    // members for why (moveToThread() silently fails on an object that already has
    // a parent).
    std::unique_ptr<HoverInfoWorker> hoverInfoWorker_;
    quint64 hoverInfoCounter_ = 0;
    quint64 hoverInfoRequestId_ = 0; // the *latest* request - an older reply arriving late is just discarded

    // Recomputed by relayout() (viewport resize, icon size change, or model reset -
    // row count affects the scrollbar range even though not the column count) - nothing
    // else ever writes these, and painting/hit-testing/scrolling all read them instead
    // of re-deriving independently, so there's exactly one source of truth for "where is
    // cell N" no matter which of those three triggered the last recompute.
    int columns_ = 1;
    int cellWidth_ = 0;
    int cellHeight_ = 0;
    // Height of the image area inside a cell - deliberately less than iconSize_, so a
    // landscape photo isn't letterboxed inside a square box. See prefs::kThumbnailTileAspect.
    int imageAreaHeight_ = 0;
    // Whatever viewport width doesn't divide evenly into whole columns is spread out as
    // equal gutters - between every pair of adjacent columns, and on the two outer edges
    // - rather than collected into one pair of big outer margins. Cells themselves stay a
    // fixed width (iconSize_ + 2*kCellPadding, matching the user's chosen icon size
    // exactly - see relayout()'s comment on why a *stretched* cell width was tried and
    // reverted before: it made the empty space *around each photo* grow unpredictably
    // with window width instead of just the gaps between photos). gridOffsetX_ is the
    // left edge of column 0 (== one gutter width, in this scheme); columnStride_ is the
    // distance from one column's left edge to the next (cellWidth_ + one gutter).
    // Painting and hit-testing both use these instead of cellWidth_ alone.
    int gridOffsetX_ = 0;
    int columnStride_ = 0;

    // High-resolution mice/trackpads deliver many small fractional wheel deltas
    // instead of one clean 120-unit notch per click - accumulate across events so a
    // full notch's worth of rotation still triggers exactly one discrete row-step,
    // rather than falling through to smooth per-pixel scrolling for every sub-notch
    // event.
    int accumulatedDelta_ = 0;

    int rowCount() const;
    // Recomputes columns_/cellWidth_/cellHeight_ from the current viewport width and
    // iconSize_, and the scrollbar range from those plus the current row count. No
    // debounce needed, unlike a QListView-based relayout: there's no
    // per-item relayout to force here, just arithmetic - the actual per-item cost
    // only happens in paintEvent(), which only ever touches on-screen cells and which
    // Qt already coalesces repaint requests for on its own.
    void relayout();
    // Model row -> its cell rect in *content* coordinates (before subtracting the
    // vertical scrollbar's current position) - i.e. as if there were no scrolling.
    QRect contentRect(int row) const;
    // pos (viewport-relative) -> model row, or -1 if pos isn't over any cell (empty
    // margin to the right of a partially-filled last row, or below the last item).
    int rowAt(const QPoint &pos) const;
    void paintCell(class QPainter &painter, int row, const QRect &rect) const;
    void drawDropFeedback(class QPainter &painter) const;
    // Moves the lead row by `delta`, clamped to the valid row range, and keeps it
    // visible - shared by every keyboard-navigation case (arrows, Page Up/Down).
    // `extendSelection` extends the range from anchorRow_ (Shift+<key>) instead of
    // replacing the selection outright.
    void moveCurrentRow(int delta, bool extendSelection);

    // Selection primitives - each leaves selected_/selectedCount_/currentRow_/
    // anchorRow_ consistent but does not itself emit any signal or repaint; callers
    // do that once via applySelectionResult() so a single gesture never produces more
    // than one of each signal.
    void setSelectedBit(int row, bool on);
    void replaceSelectionWith(int row);
    void clearSelectionInternal();
    void toggleRow(int row);
    // Selects [anchorRow_, row] (inclusive, either order). `unionMode` adds the range
    // into the existing selection (Ctrl+Shift+click) instead of replacing it
    // (plain Shift+click). Falls back to replaceSelectionWith(row) if there's no
    // anchor yet.
    void selectRange(int row, bool unionMode);
    // Repaints, and emits selectionChanged() plus currentRowChanged() - the latter if
    // the lead moved, or if a model reset made the comparison meaningless (see
    // resetSinceNotify_). Called once at the end of every gesture/mutator above.
    void applySelectionResult(int oldCurrentRow);

    // ThumbGridModel can insert/remove rows mid-lifetime now (a file op landing in,
    // or removing a file from, the currently-displayed folder) instead of only ever
    // a full reset - these keep selected_/currentRow_/anchorRow_ tracking the
    // surviving rows by shifting indices, rather than the model reset path's
    // "everything's different now, clear it all."
    void onRowsInserted(int first, int last);
    void onRowsRemoved(int first, int last);

    // Called on every mouse move where no button is held (see mouseMoveEvent) -
    // (re)starts hoverDelayTimer_ when the hovered cell actually changes, and hides
    // any tooltip already showing for a since-abandoned cell.
    void handleHoverMove(const QPoint &viewportPos);
    // hoverDelayTimer_'s timeout handler - shows the cached-info tooltip immediately
    // and kicks off the async EXIF-detail fetch for hoverRow_.
    void onHoverDelayElapsed();

private slots:
    // hoverInfoWorker_'s result for `id` - re-shows the tooltip with the EXIF text
    // appended, but only if the mouse is still over the same row the request was
    // for (a stale/superseded id, or the mouse having moved on since, means this is
    // simply discarded).
    void onHoverInfoReady(quint64 id, QString detailsText);
};
