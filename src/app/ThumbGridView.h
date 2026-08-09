#pragma once

#include <QListView>
#include <QStyledItemDelegate>

class QTimer;

// Fully custom cell painting (not just a tweak to the default QStyledItemDelegate
// look): a soft border around every cell, the thumbnail centered in a fixed-size
// image area, and the filename centered below it. Also still responsible for the
// selection border (not a recolor - see below) so the selected/soft borders share one
// paint pass instead of fighting each other.
class ThumbGridDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    using QStyledItemDelegate::QStyledItemDelegate;

    // Shared with ThumbGridView::updateGridSize(), which needs the exact same numbers
    // to size cells correctly - single source of truth for the cell layout.
    static constexpr int kCellPadding = 8;    // margin around the image area
    static constexpr int kTextTopGap = 4;     // gap between image area and filename
    static constexpr int kTextRowHeight = 18; // reserved height for the filename line

protected:
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    // Without this, Qt falls back to the default icon+text size hint (its own margin
    // assumptions, unrelated to this delegate's layout) to decide how big `option.rect`
    // is - smaller than what paint() actually draws into, so both the soft and
    // selection borders end up too short and cut through the image/text instead of
    // surrounding the full cell. Returning the view's actual gridSize() keeps
    // option.rect exactly matched to what's painted.
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

// QListView's built-in wheel handling scrolls by a pixel amount scaled by the OS's
// "lines per wheel notch" setting (usually 3), which reads as smooth/continuous
// scrolling over a grid of thumbnails. This subclass makes one wheel notch move by
// exactly one row, so scrolling feels like discrete pagination instead.
class ThumbGridView : public QListView {
    Q_OBJECT

public:
    explicit ThumbGridView(QWidget *parent = nullptr);

    // Ensures a full extra row stays visible above and below `index` (when there's
    // enough content for that to mean anything) rather than just barely bringing it
    // into view - so arrow-key browsing near the top/bottom edge always shows a
    // preview of what's coming next. Only applies to the default EnsureVisible hint
    // (what keyboard navigation uses); explicit hints like PositionAtCenter (used for
    // jumping to a specific file) are left untouched.
    void scrollTo(const QModelIndex &index, ScrollHint hint = EnsureVisible) override;

signals:
    // Ctrl+arrow is folder navigation (see MainWindow), not grid navigation - this
    // view just recognizes the chord and hands off the direction; it doesn't know
    // anything about folders/trees itself.
    void navigateFolderRequested(Qt::Key direction);

protected:
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    // High-resolution mice/trackpads deliver many small fractional wheel deltas
    // instead of one clean 120-unit notch per click - accumulate across events so a
    // full notch's worth of rotation still triggers exactly one discrete row-step,
    // rather than falling through to smooth per-pixel scrolling for every sub-notch
    // event (which is what silently happened before this field existed).
    int accumulatedDelta_ = 0;

    // Cell width is recomputed once resizing settles so as many columns as fit evenly
    // split the viewport's actual width - a fixed cell width almost never divides the
    // viewport evenly, leaving a gap on the right. Height stays fixed; only width
    // stretches to fill the row. Debounced (see resizeEvent()) rather than run
    // straight off every resizeEvent: a single window-manager resize (e.g. a
    // maximize) can fire a burst of intermediate WM_SIZE events.
    //
    // Getting this to actually converge took three layers, each fixing a real,
    // logged failure of the previous one:
    //  1. Debounce alone: a resize burst still computed-and-verified against each
    //     intermediate size, so the *final* size's verification never got to finish.
    //  2. fitInProgress_ (block re-entry while a fit is running): setGridSize()
    //     inside applyColumns() turned out to itself trigger further resizeEvents
    //     mid-backoff, re-entering at a fresh ideal guess before the prior backoff
    //     could converge.
    //  3. lastFitWidth_ (skip re-deriving for a width close to one already handled):
    //     even serialized, the *sequence itself* oscillates forever on its own -
    //     trying N columns can fail, backing off to N-1 (wider) cells changes the row
    //     count, which can toggle the vertical scrollbar's visibility, which changes
    //     viewport width by the scrollbar's ~12-17px - and idealColumns() computes
    //     the *same* N again for a width that close, repeating indefinitely. Since
    //     idealColumns() is a stateless function of only the current width, it has no
    //     way to remember "N already failed here" on its own.
    void updateGridSize();
    QTimer *resizeDebounce_;
    bool fitInProgress_ = false;
    // Set when updateGridSize() is called again while a fit is already running (see
    // fitInProgress_) - without this, that trigger was simply dropped, so a resize
    // landing mid-fit (e.g. the final settled size arriving right as an intermediate
    // animation frame's fit was still converging) got silently lost, leaving the grid
    // stuck sized for a stale, since-superseded width. Checked once the current fit
    // finishes to run one more pass against whatever the width actually is by then.
    bool pendingRecheck_ = false;
    int lastFitWidth_ = -1;
    static constexpr int kWidthJitterTolerance = 20; // comfortably more than one scrollbar's width
    // Target column count for the current viewport width, before any verification -
    // see applyColumns() for why this is only a starting guess, not a guarantee.
    int idealColumns() const;
    // Sets gridSize() for `columns` evenly filling the viewport, then verifies what
    // QListView actually rendered and backs off by one column (wider cells) if it
    // came up short. QListView's own column-fitting arithmetic isn't something this
    // can reliably predict up front - tried both a strict inequality and a 1px safety
    // margin on the width calculation and neither was always enough (likely
    // DPI-scaling-related rounding inside Qt's layout code) - directly checking what
    // rendered and correcting is more robust than continuing to guess at the formula.
    // attemptsLeft bounds the backoff so it can't loop forever - generous on purpose:
    // each step is a cheap 0ms check (the normal case converges in exactly one), and
    // too small a bound was observed to cut the sequence off before it actually
    // reached a fitting count (landed on idealColumns()-3 columns, well under what
    // the width could actually hold, precisely because attemptsLeft was 3).
    void applyColumns(int columns, int attemptsLeft = 15);
    // Clears fitInProgress_/records lastFitWidth_, then re-triggers if a resize came
    // in while this fit was running (see pendingRecheck_) - the single place every
    // terminal path through applyColumns() routes through, so that re-trigger can't
    // be missed from any of them.
    void finishFit();
    // How many items actually share the first row's top y-coordinate right now.
    int renderedColumnCount() const;
};
