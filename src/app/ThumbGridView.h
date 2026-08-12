#pragma once

#include <QAbstractScrollArea>
#include <QBitArray>
#include <QList>
#include <QPoint>

class QAbstractItemModel;
class QDragEnterEvent;
class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;
class QTimer;

// Fully custom-painted, virtualized thumbnail grid - replaces an earlier QListView-
// based implementation whose IconMode flow-layout (Qt's own internal algorithm for
// deciding how many same-size cells fit per row) turned out to sometimes disagree
// with a plain, exact, no-remainder division by a column count. Confirmed via a real
// side-by-side repro, not a hypothetical: a 748px-wide viewport with 187px cells
// rendered 4 columns (4*187=748, exact); an 8px-wider, 756px viewport with 189px
// cells - an equally exact division, 4*189=756 - rendered only 3, with
// devicePixelRatio()==1 in both cases, ruling out DPI-scaling rounding as the cause.
// There's no way to predict that kind of internal-to-Qt rounding from the outside, so
// rather than another attempt at guessing it correctly (a previous version tried a
// guess-then-verify-then-backoff loop against exactly this, which just moved the
// unpredictability into a different, more fragile shape - see git history), this
// class doesn't ask IconMode to lay anything out at all. It computes every cell's
// row/column directly (row = index / columns, col = index % columns) and paints
// visible cells itself, so there is no second layout engine left that could ever
// disagree with the column count this class decided on - "computed" and "actually
// rendered" are the same number by construction, not something to separately verify
// (see debugComputedColumns()/debugRenderedColumnCount(), kept identical on purpose).
//
// Model access is still through a plain QAbstractItemModel (ThumbGridModel, a flat
// QAbstractListModel) - only DisplayRole/DecorationRole are read here, so the view
// stays model-agnostic even though ThumbGridModel now supports mid-lifetime row
// insert/remove (see rowsInserted/rowsRemoved handling below) in addition to the
// original modelReset/dataChanged paths.
//
// Selection is multi-row (see currentRow()/selectedRows() below), not the single-row
// design this class started with - but there is still exactly one "lead" row at any
// time (currentRow()), which is what the preview pane/status bar/path bar follow.
class ThumbGridView : public QAbstractScrollArea {
    Q_OBJECT

public:
    explicit ThumbGridView(QWidget *parent = nullptr);

    void setModel(QAbstractItemModel *model);
    QAbstractItemModel *model() const { return model_; }

    // Square icons only - matches prefs::thumbnailIconSize(), the only way this is
    // ever actually driven. Recomputes the grid layout immediately (unlike the old
    // QListView-based setIconSize(), a plain property with no side effect of its
    // own) - there's no longer a separate applyIconSizeChange() to call afterward.
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

    // TODO: was debug-build-only; in release too for now (2026-08-11), see
    // MainWindow::onCopyGridDebugInfo(). Always equal now, by construction (see the
    // class comment). Kept as two separate calls anyway so the debug dump states
    // that plainly rather than silently dropping the "actually rendered" field that
    // used to be the whole point of this dump.
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

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    static constexpr int kCellPadding = 8;    // margin around the image area
    static constexpr int kTextTopGap = 4;     // gap between image area and filename
    static constexpr int kTextRowHeight = 18; // reserved height for the filename line

    QAbstractItemModel *model_ = nullptr;
    int iconSize_ = 150;

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

    // Recomputed by relayout() (viewport resize, icon size change, or model reset -
    // row count affects the scrollbar range even though not the column count) - nothing
    // else ever writes these, and painting/hit-testing/scrolling all read them instead
    // of re-deriving independently, so there's exactly one source of truth for "where is
    // cell N" no matter which of those three triggered the last recompute.
    int columns_ = 1;
    int cellWidth_ = 0;
    int cellHeight_ = 0;

    // High-resolution mice/trackpads deliver many small fractional wheel deltas
    // instead of one clean 120-unit notch per click - accumulate across events so a
    // full notch's worth of rotation still triggers exactly one discrete row-step,
    // rather than falling through to smooth per-pixel scrolling for every sub-notch
    // event.
    int accumulatedDelta_ = 0;

    int rowCount() const;
    // Recomputes columns_/cellWidth_/cellHeight_ from the current viewport width and
    // iconSize_, and the scrollbar range from those plus the current row count. No
    // debounce needed (unlike the old QListView-based updateGridSize()): there's no
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
    // Repaints, and emits selectionChanged() plus (if it moved) currentRowChanged() -
    // called once at the end of every gesture/mutator above.
    void applySelectionResult(int oldCurrentRow);

    // ThumbGridModel can insert/remove rows mid-lifetime now (a file op landing in,
    // or removing a file from, the currently-displayed folder) instead of only ever
    // a full reset - these keep selected_/currentRow_/anchorRow_ tracking the
    // surviving rows by shifting indices, rather than the model reset path's
    // "everything's different now, clear it all."
    void onRowsInserted(int first, int last);
    void onRowsRemoved(int first, int last);
};
