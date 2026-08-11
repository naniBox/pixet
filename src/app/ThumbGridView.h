#pragma once

#include <QAbstractScrollArea>

class QAbstractItemModel;
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
// QAbstractListModel - only modelReset and dataChanged are actually used, no
// mid-lifetime row insert/remove), so ThumbGridModel itself needed no changes.
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

    // -1 = no selection.
    int currentRow() const { return currentRow_; }
    void setCurrentRow(int row);

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
    void currentRowChanged(int row);
    // Enter/Return or double-click on a row.
    void activated(int row);
    // Ctrl+arrow is folder navigation (see MainWindow), not grid navigation - this
    // view just recognizes the chord and hands off the direction; it doesn't know
    // anything about folders/trees itself.
    void navigateFolderRequested(Qt::Key direction);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    static constexpr int kCellPadding = 8;    // margin around the image area
    static constexpr int kTextTopGap = 4;     // gap between image area and filename
    static constexpr int kTextRowHeight = 18; // reserved height for the filename line

    QAbstractItemModel *model_ = nullptr;
    int iconSize_ = 150;
    int currentRow_ = -1;

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
    // Moves currentRow_ by `delta`, clamped to the valid row range, and keeps it
    // visible - shared by every keyboard-navigation case (arrows, Page Up/Down).
    void moveCurrentRow(int delta);
};
