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

    // Call after prefs::setThumbnailIconSize() changes - re-derives the grid layout
    // (cell size, column count) for the new size. Resets lastFitWidth_ first: without
    // that, updateGridSize()'s jitter guard would see an unchanged viewport width and
    // skip recomputing entirely, since it has no way to know the *cell* size (not
    // just the width) is what actually changed.
    void applyIconSizeChange();

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
    // maximize, or a manual drag) can fire a burst of intermediate WM_SIZE events.
    //
    // Getting this to actually converge went through several rounds, each fixing a
    // real, logged failure of the previous one - worth recording since the failure
    // mode was different every time and easy to reintroduce:
    //  1. Debounce alone: a resize burst still computed-and-verified against each
    //     intermediate size, so the *final* size's verification never got to finish.
    //  2. Verifying what actually rendered (via renderedColumnCount()) and backing off
    //     by one column when short, rather than trusting idealColumns()'s arithmetic
    //     outright - QListView's own column-fitting isn't reliably predictable up
    //     front (tried both a strict inequality and a 1px safety margin on the width
    //     calculation and neither was always enough, likely DPI-scaling-related
    //     rounding inside Qt's layout code).
    //  3. The verify step was originally deferred a tick (QTimer::singleShot(0)) to
    //     give Qt a chance to actually lay out the new gridSize before checking it -
    //     which turned out to be the root problem, not a detail: that tick is a gap
    //     the OS can deliver another resize event into (setGridSize() itself can
    //     synchronously toggle the vertical scrollbar's visibility as row count
    //     changes, which is itself a resize; a real user drag-resizing the window
    //     delivers a steady stream of these regardless). A resize landing in that gap
    //     meant the *next* verify was checking a gridSize computed for an
    //     already-superseded width, and whatever it converged to then got permanently
    //     stamped as "fits this width" even though it didn't - visible as the grid
    //     getting stuck at far fewer columns than the space available (small folders,
    //     where backing off enough columns to need more rows was itself enough to
    //     toggle the scrollbar mid-verify) or as leftover unfilled space at the right
    //     edge once resizing actually stopped (a mid-drag width's fit outliving the
    //     drag). Fixed at the root by dropping the deferred verify entirely -
    //     doItemsLayout() forces Qt to recompute item positions synchronously, so the
    //     whole try/verify/back-off loop now runs in one call with no event-loop gap
    //     for a resize to land in at all, rather than reactively detecting drift after
    //     the fact (which was tried first and still wasn't watertight against a
    //     continuous drag re-landing in the *next* gap).
    //  5. Turns out even that gap wasn't fully closeable: a resize landing while
    //     updateGridSize() is mid-loop was observed to change the viewport's width out
    //     from under a *later* iteration of the same synchronous loop, not just
    //     between separate calls to this method - a real window drag comes from
    //     explorer.exe/dwm.exe, delivered as sent messages Qt can end up dispatching
    //     between iterations regardless of how synchronous this method's own code is.
    //     Retrying inline against that (an "outer" retry loop, tried next) doesn't
    //     reliably win either: fitting a *narrower* just-seen width can converge on
    //     one column count while fitting a *wider* one converges on a very different
    //     one (the empirical renderedColumnCount() check is only meaningful for a
    //     width that actually held still while being measured), so retrying inline
    //     against a target whose scrollbar is itself toggling in response to each
    //     guess can genuinely tick-tock between two states forever rather than settle.
    //     Deferring to the debounce timer on a detected mismatch - rather than
    //     retrying inline - fixes the *transient* version of this (a drag still
    //     actually in progress), since the timer only fires once real quiet has
    //     elapsed. It doesn't fix a *sustained* one, though: consecutiveDefers_ bounds
    //     how many times in a row this can happen before just accepting whatever the
    //     current state is rather than deferring indefinitely - guaranteeing
    //     termination even if the width and the scrollbar it toggles are, for
    //     whatever reason, never going to hold still relative to each other.
    // Being synchronous end-to-end (layers 3-4) does still mean there's no window for
    // updateGridSize() to be re-entered by its *own* side effects (as opposed to an
    // external resize) mid-fit, so there's no separate re-entrancy guard needed here.
    void updateGridSize();
    QTimer *resizeDebounce_;
    // Skip re-deriving for a width close to one already fit - not primarily about
    // performance, but about breaking a genuine oscillation: fitting N columns can
    // itself change row count enough to toggle the vertical scrollbar, which changes
    // viewport width by the scrollbar's ~12-17px, which would otherwise trigger
    // *another* fit for a width that close, which can toggle the scrollbar back, and
    // so on indefinitely. idealColumns() is a stateless function of only the current
    // width, so without this it has no way to remember "already fit here." Only ever
    // set from a fit that was actually verified against a width that held still (or
    // after consecutiveDefers_ forces acceptance) - see updateGridSize().
    int lastFitWidth_ = -1;
    static constexpr int kWidthJitterTolerance = 20; // comfortably more than one scrollbar's width
    // How many updateGridSize() calls in a row have bailed out via the debounce-defer
    // path (see updateGridSize()) without ever reaching a verified fit. Bounds that
    // defer loop so a width that's genuinely oscillating (its own scrollbar toggling
    // in response to each fit attempt, rather than settling) can't defer forever -
    // past kMaxConsecutiveDefers, the next call accepts whatever it converges to
    // without requiring one more round of verification. Reset to 0 on any verified fit.
    int consecutiveDefers_ = 0;
    static constexpr int kMaxConsecutiveDefers = 4;
    // Target column count for the current viewport width, before any verification -
    // see updateGridSize() for why this is only a starting guess, not a guarantee.
    int idealColumns() const;
    // How many items actually share the first row's top y-coordinate right now.
    int renderedColumnCount() const;
};
