#pragma once

#include <QElapsedTimer>
#include <QPersistentModelIndex>
#include <QTreeView>

class QDragEnterEvent;
class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;

// Qt's native click/keyboard-navigation handling in QTreeView resets horizontal
// scroll back to the left as part of selecting a new current item (it scrolls to
// reveal the row's start) - this happens *inside* QTreeView's own event handling,
// before any currentChanged signal fires, so there's no way to intercept it from
// outside. Save/restore horizontal scroll directly around the base class calls
// instead, regardless of what internal auto-scroll Qt performs during them.
//
// Also the window's second drop target (ThumbGridView being the first): thumbnails
// dragged out of the grid, or files dragged in from Explorer/Finder, can be dropped
// onto any folder row to move them there. None of QAbstractItemView's own drag/drop
// machinery is used for that - every handler below is a full override that never
// calls its base. That is deliberate: the base implementation would route the drop
// into QFileSystemModel::dropMimeData(), which does its own file copy/move with no
// collision dialog, no claim on the directories involved, and no idea that pixet's
// index has rows describing the files being moved. Everything pixet knows about a
// move (see FileOpsWorker) would be bypassed by the one line that looks most correct.
//
// Holding a drag near the top or bottom edge scrolls the tree, so a folder that isn't
// on screen when the drag starts is still reachable - and for as long as the pointer stays
// in that band a release **drops nothing, anywhere**, with the pane grey-washed to say so.
// That refusal is the point of the feature, not a limitation of it: releasing over a list
// that is moving under the cursor is how files end up in a folder nobody can name
// afterwards, and a photo filed into the wrong folder by one frame of scrolling is worse
// than a drag that has to be re-aimed. Move off the edge, the scroll stops, the row lights
// up, and the drop lands where it was aimed.
//
// The refusal is enforced at the drop and nowhere else, which is not where it started -
// see the long comment in updateDropTarget(). Refusing the *drag* is what a drop target
// normally does, and it makes the OS draw its own no-drop cursor, but on Windows it also
// takes this view out of the drag until the next event it is handed, and that can be most
// of a second while the tree is listing what a scroll revealed. A release in that window
// is simply lost. Given the choice between a cursor that warns and a drop that works, the
// drop wins; the wash carries the warning on its own.
class FolderTreeView : public QTreeView {
    Q_OBJECT

public:
    explicit FolderTreeView(QWidget *parent = nullptr);

    // True from the moment a drag carrying files enters this view until it leaves or
    // drops. MainWindow checks this before repositioning the tree (see
    // repositionTreeToTop()): its post-navigation settling scrolls the browsed folder
    // back to the top as each ancestor's listing lands, and doing that mid-drag moves
    // the row out from under the cursor - either fighting an edge scroll the user
    // asked for, or worse, sliding a different folder under a drop that is about to
    // happen. Whatever the tree is showing during a drag, the user put it there.
    bool dragInProgress() const { return dragActive_; }

signals:
    // Local files were dropped onto the folder row `target`, which is always a valid
    // index (a drop over empty space below the last row is refused outright, since
    // there is no folder there to name). `move` is true unless Ctrl was held at drop
    // time - see droppolicy::wantsCopy().
    //
    // This view performs no I/O and deliberately never resolves `target` to a
    // filesystem path itself: it is handed a QAbstractItemModel like any other view
    // and doesn't know it happens to be showing a QFileSystemModel. MainWindow owns
    // both that model and the FileOpsWorker, so it is where an index becomes a path
    // and a path becomes a move.
    void filesDroppedOnFolder(QModelIndex target, QStringList localPaths, bool move);
    // A release this view declined, with a sentence saying why. Needed because the drag is
    // accepted right up to the release now (see updateDropTarget in the .cpp): the cursor
    // no longer warns on the way down, so the moment of refusal is the only chance to say
    // anything, and a drop that quietly does nothing is indistinguishable from a bug.
    void dropRefused(QString reason);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void restoreHorizontalScroll(int value);

    // Shared by dragEnterEvent/dragMoveEvent, which differ only in their event type -
    // both have to re-answer the same two questions (which row is under the cursor,
    // and is Ctrl held) on every single event, since either can change without the
    // drag ever leaving the widget.
    void updateDropTarget(QDropEvent *event);
    // The row the highlight is currently drawn on, and the mode it's drawn in.
    // Paint-only state: dropEvent() re-derives the actual target from the drop
    // position rather than trusting this, so there is no way for the folder that gets
    // the files to differ from the row that was highlighted by being one event stale.
    //
    // Persistent rather than a plain QModelIndex because QFileSystemModel populates
    // directory contents asynchronously: a directoryLoaded landing mid-drag inserts
    // rows, which would leave a stored plain index quietly pointing at a different
    // folder than the one under the cursor - and, being paint state, it would move the
    // highlight rather than fail loudly.
    QPersistentModelIndex dropTarget_;
    bool dropCopyMode_ = false;
    bool dragActive_ = false; // see dragInProgress()

    // Edge auto-scroll during a drag. Deliberately not QAbstractItemView's own
    // startAutoScroll()/doAutoScroll(), which are protected and would be two lines to
    // call: theirs accelerates on every tick (autoScrollCount climbs towards a whole
    // pageStep per 50ms) and offers no way to ask for a constant rate, which is exactly
    // the runaway this is meant to be the opposite of. It also has no notion of a view
    // that wants to stop accepting drops while it scrolls.
    //
    // A timer rather than work done in dragMoveEvent(), for two reasons that both come
    // down to drag events being an unreliable clock. A drag held still at the edge
    // delivers no further events at all, so the scroll has to keep going on its own. And
    // - this one was a real bug, not a precaution - the events that do arrive can be
    // arbitrarily late: the scroll reveals rows, QFileSystemModel goes off to list the
    // directories they name, and on a busy tree the dragMoveEvent that should have ended
    // the wash showed up a few hundred milliseconds after the pointer had left the band.
    // The wash outliving the state it describes is exactly the lie this must not tell, so
    // the tick re-reads the live cursor position itself (see onEdgeScrollTick) and the
    // state is never more than one tick stale, whatever the event stream is doing.
    void updateEdgeScroll(const QPoint &viewportPos);
    // The whole widget in viewport coordinates - frame and scrollbars included. See the
    // .cpp: the difference between this and viewport()->rect() is the bottom strip of the
    // pane, which is exactly where a drag aimed at "the bottom edge" tends to stop.
    QRect paneRect() const;
    // Everything that has to stop when the drag is over, from wherever that is noticed.
    void endDragHover();
    // Which edge band the pointer is in - -1 (top), +1 (bottom), 0 (neither). Being in one
    // is not the same as scrolling in it: see edgeScrollActive_.
    void setEdgeBandDirection(int dir);
    // Whether the scrollbar has any room left in `dir`. Consulted to decide whether a run
    // *starts*, and re-asked every tick until it does; never asked whether a run should
    // end. All three of those are deliberate, and each one is a bug that was in here:
    //
    //  - Gating the start is what keeps a tree too short to scroll from having a dead strip
    //    at each end. Nothing can move there, so the band is just ordinary rows and drops
    //    land normally. Same for a tree already scrolled to the end when the drag arrives.
    //  - Asking it only *once*, on the drag event that entered the band, made the whole
    //    feature intermittent: QFileSystemModel streams rows in while the tree is being
    //    browsed, each batch has the view recompute its scroll range, and a pointer that
    //    arrived during one of those moments got "nothing to scroll here" and no retry,
    //    because a still pointer sends no further events. Hence the tick asking again
    //    every 50ms for as long as a drag is over this widget.
    //  - Gating the *continuation* on it was the same transient seen from the other side:
    //    it cleared the wash mid-hold and handed a drop back to whatever row was under a
    //    cursor parked at the very bottom edge - a misdrop offered by a blink, which is
    //    the exact failure this feature exists to prevent. A run ends when the pointer
    //    leaves the band, and only then.
    bool canEdgeScroll(int dir) const;
    void onEdgeScrollTick();
    void drawEdgeScrollFeedback(class QPainter &painter) const;

    // Started when a drag arrives over this view and stopped when it leaves or drops, not
    // scoped to the scrolling itself - see the comment in updateDropTarget() for why the
    // poll has to be running before the pointer reaches a band at all.
    QTimer *edgeScrollTimer_ = nullptr;
    // Two states, not one, because "the pointer is asking for a scroll" and "this view is
    // scrolling and therefore refusing drops" are different things and only the second one
    // may be shown or acted on. The gap between them is a band held over a tree with
    // nothing left to scroll: the tick keeps re-checking as the model grows, while the rows
    // underneath stay perfectly droppable.
    int edgeBandDir_ = 0;
    bool edgeScrollActive_ = false;
    // Time since this run of scrolling began, which is what the speed ramps against, and
    // the sub-row remainder carried between ticks so a rate slower than one row per tick
    // is still a rate rather than a rounding error.
    QElapsedTimer edgeScrollClock_;
    qreal edgeScrollCarry_ = 0;
};
