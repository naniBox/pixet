#include "FolderTreeView.h"

#include <QCursor>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QGuiApplication>
#include <QMimeData>
#include <QPainter>
#include <QScrollBar>
#include <QTimer>
#include <QUrl>

#include "DropPolicy.h"

namespace {

// The tick is deliberately short and fixed, and the *speed* is what varies: it doubles as
// the poll that keeps the scrolling state honest when drag events are late or absent (see
// the header comment on updateEdgeScroll), so it has to be quick enough that a wash never
// visibly outlives the pointer leaving the band.
constexpr int kEdgeScrollTickMs = 50;

// Rows per second, ramped linearly from the first to the second over kEdgeScrollRampMs.
// The slow start is what makes a brief clip of the edge harmless - overshooting into the
// band on the way to a row near the bottom should cost a row or two, not a screenful -
// and the ceiling is what keeps a long hold from turning into a blur nobody can aim at.
// The ramp only matters while the pointer stays in the band; leaving resets it, so speed
// is always a statement about the current hold rather than accumulated history.
constexpr qreal kEdgeScrollStartRowsPerSec = 4.0;
constexpr qreal kEdgeScrollMaxRowsPerSec = 24.0;
constexpr int kEdgeScrollRampMs = 1200;

} // namespace

FolderTreeView::FolderTreeView(QWidget *parent) : QTreeView(parent) {
    // Both calls are required, and for the same reason ThumbGridView's constructor
    // needs both: drag/drop events on a QAbstractScrollArea are delivered to the
    // *viewport*, and setAcceptDrops(true) on `this` alone is a silent no-op that
    // simply never produces a single dragEnterEvent.
    //
    // Deliberately not setDragDropMode(DropOnly), which would look like the idiomatic
    // way to say this: that also arms QAbstractItemView's own drop path, and this
    // class overrides every handler that path runs through anyway (see the class
    // comment). Saying it with the two properties that actually matter keeps the
    // enabled machinery down to what is really used - notably leaving dragEnabled()
    // false, so a folder row can never be picked up and dragged somewhere itself.
    setAcceptDrops(true);
    viewport()->setAcceptDrops(true);

    edgeScrollTimer_ = new QTimer(this);
    edgeScrollTimer_->setInterval(kEdgeScrollTickMs);
    connect(edgeScrollTimer_, &QTimer::timeout, this, &FolderTreeView::onEdgeScrollTick);
}

void FolderTreeView::mousePressEvent(QMouseEvent *event) {
    int hScroll = horizontalScrollBar()->value();
    QTreeView::mousePressEvent(event);
    restoreHorizontalScroll(hScroll);
}

void FolderTreeView::mouseReleaseEvent(QMouseEvent *event) {
    int hScroll = horizontalScrollBar()->value();
    QTreeView::mouseReleaseEvent(event);
    restoreHorizontalScroll(hScroll);
}

void FolderTreeView::keyPressEvent(QKeyEvent *event) {
    int hScroll = horizontalScrollBar()->value();
    QTreeView::keyPressEvent(event);
    restoreHorizontalScroll(hScroll);
}

void FolderTreeView::restoreHorizontalScroll(int value) {
    horizontalScrollBar()->setValue(value);
    // Some of Qt's selection-driven auto-scroll is apparently deferred rather than
    // happening synchronously within the base class call above (a synchronous
    // restore right after wasn't enough on its own) - reassert once more after the
    // event loop has had a chance to process whatever that is.
    QTimer::singleShot(0, this, [this, value]() { horizontalScrollBar()->setValue(value); });
}

void FolderTreeView::updateDropTarget(QDropEvent *event) {
    if (!droppolicy::hasLocalFileUrl(event->mimeData())) return; // unaccepted: the OS shows "no drop" and we hear no more

    dragActive_ = true; // see dragInProgress() - MainWindow must stop moving the tree now
    // Runs for the whole hover, not just while scrolling: the tick is what actually reads
    // where the pointer is (see onEdgeScrollTick), and it has to be running *before* the
    // pointer reaches a band, because the places a drag is most likely to stop - the
    // scrollbar strip along the bottom of the pane, the frame - deliver no drag events to
    // this widget at all. Aiming at the visible bottom edge of the folder pane and getting
    // nothing was the whole of the "sometimes it just does not scroll" report.
    if (!edgeScrollTimer_->isActive()) edgeScrollTimer_->start();

    // Viewport-relative, which is what indexAt() wants - drag events are delivered to
    // the viewport (unlike customContextMenuRequested's position, which MainWindow has
    // to map, see onTreeContextMenu).
    const QPoint pos = event->position().toPoint();
    updateEdgeScroll(pos);

    // No highlight while the tree is scrolling - there is no target then, and the wash is
    // what says so.
    QModelIndex index = edgeScrollActive_ ? QModelIndex() : indexAt(pos);
    bool copyMode = droppolicy::wantsCopy(event->modifiers());

    if (index != dropTarget_ || copyMode != dropCopyMode_) {
        dropTarget_ = index;
        dropCopyMode_ = copyMode;
        // Modifiers can change mid-drag (Ctrl pressed or released without the mouse
        // moving at all), so the mode is re-read on every event and not just on entry.
        viewport()->update();
    }

    // Accepted for as long as the drag is over this pane at all - including mid-scroll,
    // and including the scrollbar strip where there is no row under the cursor. Refusing
    // here is what used to lock this view out of the rest of the drag, and it is worth
    // being precise about why, because the fix looks like the wrong one:
    //
    // On Windows a refusal leaves the OS holding DROPEFFECT_NONE for this target, and the
    // only thing that can change that answer is the *next drag event this widget is given*.
    // No timer can do it, no amount of internal state can do it. While the tree is busy
    // listing the directories a scroll just revealed, that next event can be the better
    // part of a second away - measured. By then the pointer is back over an ordinary row,
    // there is no highlight, and the release does nothing at all: the drop is never even
    // delivered, because as far as the OS is concerned this target said no and meant it.
    // That is the "it stops scrolling but I cannot drop any more" report, and it cannot be
    // fixed while the refusal lives here.
    //
    // So the refusal moves to the one place that is always evaluated at exactly the right
    // moment against exactly the right position: dropEvent(). What must not happen - files
    // landing in a list that is moving under the cursor - is enforced there, from the drop's
    // own coordinates. The cost is the OS no-drop cursor during a scroll, which the wash
    // now carries alone.
    event->setDropAction(copyMode ? Qt::CopyAction : Qt::MoveAction);
    event->accept();
}

void FolderTreeView::updateEdgeScroll(const QPoint &viewportPos) {
    // About one row tall, derived from the font rather than hardcoded in pixels so it
    // keeps meaning the same thing at a different UI scale. Qt's own autoScrollMargin
    // default is a flat 16.
    const int band = qMax(16, fontMetrics().height() + 4);
    const QRect area = viewport()->rect();

    int dir = 0;
    if (paneRect().contains(viewportPos)) {
        if (viewportPos.y() < area.top() + band) dir = -1;
        else if (viewportPos.y() > area.bottom() - band) dir = 1;
    }
    setEdgeBandDirection(dir);

    // Starting the run here rather than in the tick, so that entering a band with room to
    // scroll begins immediately on the drag event that entered it. Leaving it to the tick
    // made activation depend on a timer that is not reliably serviced inside the OLE drag
    // loop our own drags run in - measured: a hold that ticked eleven times in 700ms in one
    // place ticked once in another - which turned "scroll at the edge" into a coin flip.
    if (dir == 0 || edgeScrollActive_) return;
    // No room yet is not the same as no. The tick keeps asking (see canEdgeScroll), because
    // the answer changes on its own as QFileSystemModel lists more of the tree.
    if (!canEdgeScroll(dir)) return;

    edgeScrollActive_ = true;
    // Per run, not per drag: the ramp describes how long the pointer has been asking for
    // *this* scroll, so leaving the band and coming back starts slow again rather than
    // resuming at whatever speed it had worked up to.
    edgeScrollClock_.start();
    edgeScrollCarry_ = 0;
    dropTarget_ = QModelIndex(); // the run refuses drops; the highlight goes with it
    viewport()->update();
}

QRect FolderTreeView::paneRect() const {
    // The pane as a person aims at it, in viewport coordinates: the viewport *plus* the
    // frame and scrollbars wrapped around it. Testing against the viewport alone left the
    // bottom few pixels of the widget - the horizontal scrollbar strip and the frame under
    // it - outside the band, so a drag taken to the visible bottom edge of the folder pane,
    // which is the obvious place to take it, did nothing at all. That is the whole of the
    // "sometimes it just does not scroll" report: coming in from below worked because that
    // path crosses the viewport on the way in, coming down from the middle did not because
    // it stops in the strip.
    return QRect(viewport()->mapFrom(const_cast<FolderTreeView *>(this), QPoint(0, 0)), size());
}

void FolderTreeView::endDragHover() {
    edgeScrollTimer_->stop();
    dragActive_ = false;
    setEdgeBandDirection(0);
    dropTarget_ = QModelIndex();
    viewport()->update();
}

bool FolderTreeView::canEdgeScroll(int dir) const {
    const QScrollBar *sb = verticalScrollBar();
    return dir < 0 ? sb->value() > sb->minimum() : sb->value() < sb->maximum();
}

void FolderTreeView::setEdgeBandDirection(int dir) {
    if (dir == edgeBandDir_) return;
    edgeBandDir_ = dir;
    if (dir != 0) return; // entering a band changes nothing visible until a run starts

    // Leaving the band is the one and only thing that ends a run.
    if (!edgeScrollActive_) return; // in a band but never scrolling: nothing was painted
    edgeScrollActive_ = false;
    viewport()->update(); // the wash goes away with the run
}

void FolderTreeView::onEdgeScrollTick() {
    // The live cursor, not the last drag event, and this is the only reading of it that can
    // be trusted: drag events stop arriving over the scrollbar strip and the frame, they
    // can be hundreds of milliseconds late while the tree lists the directories a scroll
    // just revealed, and dragLeaveEvent fires merely for crossing from the viewport onto
    // this widget's own scrollbar. Everything about which band the pointer is in, and
    // therefore whether this view is refusing drops, is decided from here.
    const QPoint pos = viewport()->mapFromGlobal(QCursor::pos());

    // ...which means this poll, not dragLeaveEvent, is what decides a drag is over. Two
    // conditions, because neither covers the other: the pointer having left the pane is the
    // ordinary case, and the mouse button being up catches a drag that ended somewhere this
    // view never heard about (dropped on another widget, or cancelled with Escape while the
    // pointer sat right here). QGuiApplication::mouseButtons() is live inside a drag -
    // measured, not assumed, because the drag runs its own modal loop.
    if (!(QGuiApplication::mouseButtons() & Qt::LeftButton) || !paneRect().contains(pos)) {
        endDragHover();
        return;
    }

    updateEdgeScroll(pos);
    if (!edgeScrollActive_) return; // in no band, or in one with nothing left to scroll

    // Linear ramp from the start rate to the ceiling, then flat. Time-based rather than
    // keyed to how deep into the band the pointer is: with a band one row tall, depth is
    // a couple of pixels of hand tremor, which would read as the speed wobbling on its
    // own. How long you have held there is something the user is actually choosing.
    const qreal t = qMin(1.0, qreal(edgeScrollClock_.elapsed()) / kEdgeScrollRampMs);
    const qreal rowsPerSec = kEdgeScrollStartRowsPerSec + (kEdgeScrollMaxRowsPerSec - kEdgeScrollStartRowsPerSec) * t;

    // Out of range: the run stays alive and the wash stays up (see canEdgeScroll()), there
    // is simply nothing to move. The carry is dropped so that rows arriving later - the
    // model is often still listing - resume at the ramp's current speed instead of
    // discharging a saved-up jump all at once.
    if (!canEdgeScroll(edgeBandDir_)) {
        edgeScrollCarry_ = 0;
        return;
    }

    // Fractional rows accumulate rather than round away, so the slow end of the ramp is a
    // real speed (a row every few ticks) instead of one row per tick, which is all an int
    // step could express and would make the start ~4x too fast.
    edgeScrollCarry_ += rowsPerSec * kEdgeScrollTickMs / 1000.0;
    const int rows = int(edgeScrollCarry_);
    if (rows <= 0) return;
    edgeScrollCarry_ -= rows;

    QScrollBar *sb = verticalScrollBar();
    // singleStep() is one row in either vertical scroll mode - 1 in the ScrollPerItem
    // default QTreeView uses, and a row height in pixels if anything ever switches this
    // view to ScrollPerPixel - so the rate stays rows-per-second without this having to
    // ask which mode is in effect.
    sb->setValue(sb->value() + edgeBandDir_ * rows * qMax(1, sb->singleStep()));
    // Deliberately not re-deriving dropTarget_ from the cursor position here. Until the
    // next drag event this view has told the OS it refuses the drop, and highlighting a
    // row the drop wouldn't honour would be the one lie this whole feature exists to
    // avoid. The first pixel of mouse movement brings the two back together.
}

void FolderTreeView::dragEnterEvent(QDragEnterEvent *event) {
    // A drag arriving here is arriving *fresh*, so nothing about the last time one was
    // over this widget may survive into it. Without this, a drag that left the pane while
    // a scroll was running could come back carrying that run - refusing drops over rows
    // that are not moving and are nowhere near an edge, with no way back except an event
    // or a tick that may be a long time coming (see onEdgeScrollTick). updateDropTarget()
    // immediately re-derives the band from this event's own position, so clearing first
    // costs nothing when the pointer really has arrived in a band.
    setEdgeBandDirection(0);
    updateDropTarget(event);
}

void FolderTreeView::dragMoveEvent(QDragMoveEvent *event) { updateDropTarget(event); }

void FolderTreeView::dragLeaveEvent(QDragLeaveEvent *) {
    // Deliberately does *not* end the hover. Qt sends this whenever the drop target widget
    // changes, and crossing from the viewport onto this view's own horizontal scrollbar -
    // twelve pixels that are visually the bottom of the folder pane, and a natural place to
    // hold a drag - counts as a change. Tearing down here is what made that strip dead, and
    // tearing down and rebuilding across it would reset the speed ramp on a stray pixel.
    // The tick decides when the drag is really gone; all this needs to do is stop pointing
    // at a row.
    dropTarget_ = QModelIndex();
    viewport()->update();
}

void FolderTreeView::dropEvent(QDropEvent *event) {
    const QPoint pos = event->position().toPoint();
    // Re-derive from where the pointer actually is, rather than trusting the run flag.
    // Those can disagree: the flag is only as fresh as the last drag event or tick, and
    // both can lag by a long way while the tree is busy listing what a scroll revealed.
    // A release over a still row, well away from any edge, is a drop the user meant, and
    // refusing it because the view still thinks it is scrolling is the worse mistake -
    // the file silently stays put and there is nothing on screen saying why.
    updateEdgeScroll(pos);
    const bool refusing = edgeScrollActive_;
    endDragHover();

    // A release while the tree really is scrolling drops nothing, anywhere. This is now the
    // *only* thing standing between a moving list and a misfiled photo - the drag itself is
    // accepted throughout (see updateDropTarget) - so it is stated outright rather than
    // inferred from the target lookup below.
    //
    // Both refusals are announced for the same reason: with the cursor no longer saying "no"
    // on the way down, the release is the only moment left to say anything, and a drop that
    // quietly does nothing is indistinguishable from the app having lost it - which is
    // exactly the complaint this change is answering.
    if (refusing) {
        emit dropRefused(QStringLiteral("Not dropped - the folder tree was scrolling"));
        return;
    }

    QModelIndex target = indexAt(pos);
    if (!droppolicy::hasLocalFileUrl(event->mimeData())) return;
    if (!target.isValid()) {
        // The scrollbar strip, or empty space past the last row: no folder to name.
        emit dropRefused(QStringLiteral("Not dropped - no folder under the cursor"));
        return;
    }

    QStringList paths;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile()) paths << url.toLocalFile();
    }
    if (paths.isEmpty()) return;

    bool copyMode = droppolicy::wantsCopy(event->modifiers());
    event->setDropAction(copyMode ? Qt::CopyAction : Qt::MoveAction);
    event->accept();
    emit filesDroppedOnFolder(target, paths, /*move=*/!copyMode);
}

void FolderTreeView::paintEvent(QPaintEvent *event) {
    QTreeView::paintEvent(event);

    if (edgeScrollActive_) {
        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing);
        drawEdgeScrollFeedback(painter);
        return; // never both: the two states are mutually exclusive by construction
    }

    if (!dropTarget_.isValid()) return;

    QRect rect = visualRect(dropTarget_);
    if (rect.isEmpty()) return; // the row scrolled out from under the drag

    // Same color language as the grid's own drop feedback (ThumbGridView::
    // drawDropFeedback): the accent color for a move, a distinct amber for a Ctrl-held
    // copy. Drawn *on top of* the painted row rather than by fiddling with the item
    // delegate's state, so it can't be confused with - or overwritten by - the tree's
    // own current-item highlight, which stays where it is: a drop doesn't navigate.
    QColor color = dropCopyMode_ ? QColor(230, 160, 30) : palette().color(QPalette::Highlight);
    QColor fill = color;
    fill.setAlpha(60);

    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing);
    QRect box = rect.adjusted(0, 0, -1, -1);
    painter.fillRect(box, fill);
    painter.setPen(QPen(color, 2));
    painter.drawRect(box);
}

void FolderTreeView::drawEdgeScrollFeedback(QPainter &painter) const {
    const QRect area = viewport()->rect();

    // Pointedly *not* the move/copy accent colors the row highlight uses: this state is
    // the absence of a drop target, and anything that reads as one of those would say the
    // opposite of what is true. A flat desaturated wash over the whole pane reads the way
    // a disabled control does, which is exactly the message - the tree is moving, and it
    // is not taking anything right now.
    // Strong enough to be unmistakable rather than tasteful: against this app's near-black
    // tree background (45,45,45) it lands around (100,105,110), which reads as a pane that
    // has been greyed out, not as a subtle tint someone might take for a rendering artifact.
    // A wash that has to be looked for would defeat the point of having one.
    QColor wash(150, 158, 168);
    wash.setAlpha(130);
    painter.fillRect(area, wash);

    // Border in the same grey, so the boundary of the state is explicit even where the pane
    // meets a similarly dark neighbour.
    QColor edge(150, 158, 168);
    painter.setPen(QPen(edge, 2));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(area.adjusted(1, 1, -1, -1));

    // No label. There was one ("Scrolling - move off the edge to drop") and it was cut on
    // the grounds that the cursor already carries the message: the OS draws its own no-drop
    // badge for as long as this view refuses, so the wash and the badge together say it,
    // and a sentence of prose in the middle of a moving tree was one channel too many.
}
