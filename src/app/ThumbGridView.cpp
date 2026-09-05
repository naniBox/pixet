#include "ThumbGridView.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QCursor>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLocale>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTimer>
#include <QToolTip>
#include <QUrl>
#include <QWheelEvent>

#include "DropPolicy.h"
#include "HoverInfoWorker.h"
#include "KeyBindings.h"
#include "PathQ.h"
#include "Preferences.h"
#include "ThumbGridModel.h"
#include "db/Schema.h"

ThumbGridView::ThumbGridView(QWidget *parent) : QAbstractScrollArea(parent) {
    // Always reserve the vertical scrollbar's width, even when nothing needs
    // scrolling, rather than the default show/hide-as-needed - see the class
    // comment on why a fluctuating viewport width is exactly the thing that makes
    // column-fit arithmetic unpredictable. No horizontal scrolling at all: cells are a
    // fixed width and the block of columns is centred, with any leftover width becoming an
    // outer margin (see relayout()).
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    // Both calls are required - drag/drop events on a QAbstractScrollArea are
    // delivered to whichever widget is actually under the cursor, which is the
    // viewport (the same mechanism that already makes rowAt()'s viewport-relative
    // mouse coordinates correct). setAcceptDrops(true) on `this` alone is a silent,
    // warning-free no-op.
    setAcceptDrops(true);
    viewport()->setAcceptDrops(true);

    // QAbstractScrollArea doesn't repaint automatically when the scrollbar value
    // changes (unlike QAbstractItemView, which this deliberately isn't) - painting reads
    // verticalScrollBar()->value() directly, so a scroll is a repaint.
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() {
        viewport()->update();
        emit visibleRowsChanged();
    });

    // mouseMoveEvent only fires without a button held if mouse tracking is on - see
    // handleHoverMove(), the no-button branch of mouseMoveEvent().
    viewport()->setMouseTracking(true);

    hoverDelayTimer_ = new QTimer(this);
    hoverDelayTimer_->setSingleShot(true);
    hoverDelayTimer_->setInterval(600); // "after a short delay" - deliberately not QApplication::toolTipWait()
    connect(hoverDelayTimer_, &QTimer::timeout, this, &ThumbGridView::onHoverDelayElapsed);

    hoverInfoWorker_ = std::make_unique<HoverInfoWorker>();
    connect(hoverInfoWorker_.get(), &HoverInfoWorker::ready, this, &ThumbGridView::onHoverInfoReady);

    reloadKeyBindings();
}

void ThumbGridView::reloadKeyBindings() {
    folderNavBindings_ = {
        {keybindings::binding(keybindings::Action::FolderPrevious), Qt::Key_Up},
        {keybindings::binding(keybindings::Action::FolderNext), Qt::Key_Down},
        {keybindings::binding(keybindings::Action::FolderParent), Qt::Key_Left},
        {keybindings::binding(keybindings::Action::FolderFirstChild), Qt::Key_Right},
    };
}

ThumbGridView::~ThumbGridView() = default;

void ThumbGridView::setCurrentFolderPath(const QString &path) {
    hoverFolderPath_ = path;
    hoverRow_ = -1; // stale row index from whatever folder was showing before
    hoverDelayTimer_->stop();
}

void ThumbGridView::setModel(QAbstractItemModel *model) {
    if (model_) model_->disconnect(this);
    model_ = model;
    selected_.clear();
    selectedCount_ = 0;
    currentRow_ = -1;
    anchorRow_ = -1;
    pendingCollapseRow_ = -1;
    resetSinceNotify_ = true;
    if (model_) {
        connect(model_, &QAbstractItemModel::modelReset, this, [this]() {
            selected_.clear();
            selectedCount_ = 0;
            currentRow_ = -1;
            anchorRow_ = -1;
            pressRow_ = -1;
            pendingCollapseRow_ = -1;
            // Deliberately silent: the caller (MainWindow::reloadGridPreservingSelection)
            // restores the selection by file id immediately afterwards, and announcing an
            // empty selection in between would blank the preview pane for the length of a
            // decode on every folder refresh. resetSinceNotify_ is how that restore still
            // gets announced even when it lands on the same row number - see
            // applySelectionResult().
            resetSinceNotify_ = true;
            relayout();
        });
        connect(model_, &QAbstractItemModel::dataChanged, this, [this](const QModelIndex &, const QModelIndex &) {
            viewport()->update();
        });
        // ThumbGridModel can now insert/remove rows mid-lifetime (a file op landing
        // in, or removing a file from, the currently-displayed folder) instead of
        // only ever a full reset - shift the selection to track the surviving rows
        // rather than let it silently point at the wrong file.
        connect(model_, &QAbstractItemModel::rowsInserted, this,
                [this](const QModelIndex &, int first, int last) { onRowsInserted(first, last); });
        connect(model_, &QAbstractItemModel::rowsRemoved, this,
                [this](const QModelIndex &, int first, int last) { onRowsRemoved(first, last); });
    }
    relayout();
}

void ThumbGridView::setIconSize(QSize size) {
    iconSize_ = size.width(); // square only - see header
    relayout();
}

QSize ThumbGridView::iconSize() const { return QSize(iconSize_, iconSize_); }

int ThumbGridView::rowCount() const { return model_ ? model_->rowCount() : 0; }

QPair<int, int> ThumbGridView::visibleRowRange() const {
    int rc = rowCount();
    if (rc <= 0 || columns_ <= 0 || cellHeight_ <= 0) return {-1, -1};

    int scrollValue = verticalScrollBar()->value();
    int firstGridRow = qMax(0, scrollValue / cellHeight_);
    int lastGridRow = (scrollValue + viewport()->height()) / cellHeight_;

    int first = firstGridRow * columns_;
    // The last cell of lastGridRow, clamped to the last row that actually exists - the
    // final grid row is usually partly empty, and scrolled to the bottom lastGridRow can
    // be past the end entirely.
    int last = qMin(rc - 1, lastGridRow * columns_ + columns_ - 1);
    if (first > last) return {-1, -1};
    return {first, last};
}

void ThumbGridView::relayout() {
    imageAreaHeight_ = prefs::thumbnailImageAreaHeightFor(iconSize_);
    cellHeight_ = kCellPadding + imageAreaHeight_ + kTextTopGap + kTextRowHeight + kCellPadding;

    int vw = viewport()->width();
    if (vw > 0) {
        // Cells keep their natural width (iconSize_ + padding) - only the *gutters*
        // between/around them absorb the leftover. The tempting `cellWidth_ = vw / columns_`
        // - stretching every cell to fill the row evenly - sounds harmless and isn't: the
        // thumbnail inside is still only iconSize_ wide, so all of the leftover becomes
        // empty space *around each photo*, and the amount jumps around as the window
        // resizes (at a 240px icon size: 2px per side at a 1300px viewport, 22px at
        // 1500px). Spreading it across
        // (columns_+1) equal gutters instead keeps every photo exactly the size the
        // user picked, and reads as intentional grid spacing rather than a wide dead
        // margin down each side - see the class comment on gridOffsetX_/columnStride_.
        cellWidth_ = iconSize_ + 2 * kCellPadding;
        columns_ = qMax(1, vw / cellWidth_);
        int leftover = qMax(0, vw - columns_ * cellWidth_);
        int gutter = leftover / (columns_ + 1);
        gridOffsetX_ = gutter;
        columnStride_ = cellWidth_ + gutter;
    }

    int rc = rowCount();
    // Preserves existing bits when growing/shrinking (QBitArray::resize() zero-fills
    // any newly-added bits) - relayout() also runs on plain viewport resize/icon-size
    // change, where the selection must survive untouched. A row-count change here
    // only ever means a model reset (which already cleared selected_ to empty above)
    // or an incremental insert/remove (which shifts selected_ itself before calling
    // relayout() - see the rowsInserted/rowsRemoved handlers in setModel()).
    if (selected_.size() != rc) selected_.resize(rc);

    int totalRows = columns_ > 0 ? (rc + columns_ - 1) / columns_ : 0;
    int totalHeight = totalRows * cellHeight_;
    int vh = viewport()->height();
    verticalScrollBar()->setRange(0, qMax(0, totalHeight - vh));
    verticalScrollBar()->setPageStep(qMax(1, vh));
    verticalScrollBar()->setSingleStep(cellHeight_);

    viewport()->update();
    // Covers every trigger of relayout() that isn't a scroll: viewport resize, icon-size
    // change, model reset, and incremental row insert/remove - all of which change which
    // files are on screen even when the scroll position doesn't move.
    emit visibleRowsChanged();
}

QRect ThumbGridView::contentRect(int row) const {
    if (columns_ <= 0) return QRect();
    int gridRow = row / columns_;
    int gridCol = row % columns_;
    return QRect(gridOffsetX_ + gridCol * columnStride_, gridRow * cellHeight_, cellWidth_, cellHeight_);
}

int ThumbGridView::rowAt(const QPoint &pos) const {
    if (columns_ <= 0 || columnStride_ <= 0 || cellHeight_ <= 0) return -1;
    int contentY = pos.y() + verticalScrollBar()->value();
    // gridOffsetX_ is the left gutter (see relayout()); a click to the left of the
    // first column lands negative here and is correctly "no row", same as one past the
    // last column.
    int gridX = pos.x() - gridOffsetX_;
    if (contentY < 0 || gridX < 0) return -1;
    int gridRow = contentY / cellHeight_;
    int gridCol = gridX / columnStride_;
    if (gridCol >= columns_) return -1; // clicked past the last column's cell (trailing gutter)
    // Within a column's stride, but past its cell into the gutter before the next one -
    // still empty space, not an early hit on the next column.
    if (gridX % columnStride_ >= cellWidth_) return -1;
    int row = gridRow * columns_ + gridCol;
    if (row >= rowCount()) return -1;
    return row;
}

bool ThumbGridView::isRowSelected(int row) const {
    return row >= 0 && row < selected_.size() && selected_.testBit(row);
}

QList<int> ThumbGridView::selectedRows() const {
    QList<int> rows;
    rows.reserve(selectedCount_);
    for (int r = 0; r < selected_.size(); ++r) {
        if (selected_.testBit(r)) rows.push_back(r);
    }
    return rows;
}

void ThumbGridView::setSelectedBit(int row, bool on) {
    if (row < 0 || row >= selected_.size()) return;
    if (selected_.testBit(row) == on) return;
    selected_.setBit(row, on);
    selectedCount_ += on ? 1 : -1;
}

void ThumbGridView::replaceSelectionWith(int row) {
    selected_.fill(false);
    selectedCount_ = 0;
    setSelectedBit(row, true);
    currentRow_ = row;
    anchorRow_ = row;
}

void ThumbGridView::clearSelectionInternal() {
    selected_.fill(false);
    selectedCount_ = 0;
    currentRow_ = -1;
    anchorRow_ = -1;
}

void ThumbGridView::toggleRow(int row) {
    if (isRowSelected(row)) {
        setSelectedBit(row, false);
        anchorRow_ = row;
        if (currentRow_ == row) {
            // The lead was just deselected - fall back to the highest remaining
            // selected row (arbitrary but deterministic) rather than leaving the
            // preview pointed at a file that's no longer selected.
            currentRow_ = -1;
            for (int r = selected_.size() - 1; r >= 0; --r) {
                if (selected_.testBit(r)) {
                    currentRow_ = r;
                    break;
                }
            }
        }
    } else {
        setSelectedBit(row, true);
        currentRow_ = row;
        anchorRow_ = row;
    }
}

void ThumbGridView::selectRange(int row, bool unionMode) {
    if (anchorRow_ < 0) {
        replaceSelectionWith(row);
        return;
    }
    if (!unionMode) {
        selected_.fill(false);
        selectedCount_ = 0;
    }
    int lo = qMin(anchorRow_, row);
    int hi = qMax(anchorRow_, row);
    for (int r = lo; r <= hi; ++r) setSelectedBit(r, true);
    currentRow_ = row; // anchor stays put - only the lead moves
}

void ThumbGridView::applySelectionResult(int oldCurrentRow) {
    viewport()->update();
    emit selectionChanged();
    // The reset case is not the same question as "did the number change". After one,
    // oldCurrentRow is the -1 the reset itself stored, so a restore that also lands on
    // -1 - every file in the selection moved or vanished from the folder - compares
    // equal and would say nothing, leaving listeners describing a file that is no longer
    // there. The number matching is exactly when it means least.
    if (currentRow_ != oldCurrentRow || resetSinceNotify_) emit currentRowChanged(currentRow_);
    resetSinceNotify_ = false;
}

void ThumbGridView::setCurrentRow(int row) {
    int rc = rowCount();
    if (row < -1) row = -1;
    if (row >= rc) row = rc - 1;

    if (row < 0) {
        if (currentRow_ == -1 && selectedCount_ == 0) return;
        int oldCurrent = currentRow_;
        clearSelectionInternal();
        applySelectionResult(oldCurrent);
        return;
    }

    if (currentRow_ == row && selectedCount_ == 1 && isRowSelected(row)) return; // already exactly this
    int oldCurrent = currentRow_;
    replaceSelectionWith(row);
    applySelectionResult(oldCurrent);
}

void ThumbGridView::selectAll() {
    int rc = rowCount();
    if (rc == 0) return;
    if (selectedCount_ == rc) return; // already all selected
    int oldCurrent = currentRow_;
    selected_.fill(true);
    selectedCount_ = rc;
    // Leave the lead/preview alone when a selection already exists - Select All is a
    // precursor to Cut/Copy, not a navigation gesture. Jumping the preview to an
    // arbitrary (possibly huge RAW) file and scrolling there would throw away the scroll
    // position the user is working from. Only pick a lead when nothing was selected
    // before.
    if (currentRow_ < 0) currentRow_ = 0;
    anchorRow_ = currentRow_;
    applySelectionResult(oldCurrent);
}

void ThumbGridView::clearSelection() {
    if (selectedCount_ == 0 && currentRow_ == -1) return;
    int oldCurrent = currentRow_;
    clearSelectionInternal();
    applySelectionResult(oldCurrent);
}

void ThumbGridView::setSelection(const QList<int> &rows, int currentRow) {
    int oldCurrent = currentRow_;
    selected_.fill(false);
    selectedCount_ = 0;
    for (int r : rows) setSelectedBit(r, true);

    currentRow_ = (currentRow >= 0 && currentRow < selected_.size() && selected_.testBit(currentRow)) ? currentRow : -1;
    if (currentRow_ < 0 && selectedCount_ > 0) {
        for (int r = 0; r < selected_.size(); ++r) {
            if (selected_.testBit(r)) {
                currentRow_ = r;
                break;
            }
        }
    }
    anchorRow_ = currentRow_;
    applySelectionResult(oldCurrent);
}

void ThumbGridView::onRowsInserted(int first, int last) {
    int n = last - first + 1;
    int oldSize = selected_.size();
    // Grow, then shift existing bits at/after `first` up by n (inserted rows start
    // unselected) - processed from the top down so a bit's destination is always
    // written before its own slot is read as someone else's source.
    selected_.resize(oldSize + n);
    for (int r = oldSize - 1; r >= first; --r) selected_.setBit(r + n, selected_.testBit(r));
    for (int r = first; r < first + n; ++r) selected_.setBit(r, false);

    // Index-only shifts - the file each of these refers to hasn't changed identity,
    // just its row number, so no signal is emitted here (contrast onRowsRemoved()).
    if (currentRow_ >= first) currentRow_ += n;
    if (anchorRow_ >= first) anchorRow_ += n;
    if (pressRow_ >= first) pressRow_ += n;
    if (pendingCollapseRow_ >= first) pendingCollapseRow_ += n;

    relayout();
}

void ThumbGridView::onRowsRemoved(int first, int last) {
    int n = last - first + 1;
    int oldSize = selected_.size();
    int oldCurrent = currentRow_;
    int oldCount = selectedCount_;

    int removedSelected = 0;
    for (int r = first; r <= last && r < oldSize; ++r) {
        if (selected_.testBit(r)) removedSelected++;
    }
    selectedCount_ -= removedSelected;

    for (int r = last + 1; r < oldSize; ++r) selected_.setBit(r - n, selected_.testBit(r));
    selected_.resize(oldSize - n);

    if (currentRow_ >= first && currentRow_ <= last) {
        // The lead was itself removed - fall back to the highest remaining selected
        // row, same convention as toggleRow()'s "lead deselected" case.
        currentRow_ = -1;
        for (int r = selected_.size() - 1; r >= 0; --r) {
            if (selected_.testBit(r)) {
                currentRow_ = r;
                break;
            }
        }
    } else if (currentRow_ > last) {
        currentRow_ -= n;
    }

    if (anchorRow_ >= first && anchorRow_ <= last) anchorRow_ = currentRow_;
    else if (anchorRow_ > last) anchorRow_ -= n;

    // Stale mid-gesture state after a row shift out from under a press - safest to
    // just drop it rather than try to remap it.
    pressRow_ = -1;
    pendingCollapseRow_ = -1;

    relayout();

    // Unlike onRowsInserted(), a removal can genuinely change what's selected or
    // which file is the lead (a file moved out from under the current selection) -
    // notify listeners (MainWindow: status bar/preview) so they don't keep
    // describing a file that's no longer there.
    if (selectedCount_ != oldCount) emit selectionChanged();
    if (currentRow_ != oldCurrent) emit currentRowChanged(currentRow_);
}

void ThumbGridView::scrollToRow(int row, bool center) {
    if (!model_ || row < 0 || row >= rowCount() || columns_ <= 0) return;
    QRect content = contentRect(row);

    if (center) {
        int target = content.top() - (viewport()->height() - cellHeight_) / 2;
        verticalScrollBar()->setValue(qBound(0, target, verticalScrollBar()->maximum()));
        return;
    }

    int scrollValue = verticalScrollBar()->value();

    // Baseline "ensure visible" - scroll the minimum needed if the row isn't already
    // fully on screen.
    if (content.top() < scrollValue) {
        scrollValue = content.top();
    } else if (content.bottom() > scrollValue + viewport()->height()) {
        scrollValue = content.bottom() - viewport()->height();
    }

    // Then nudge further so a full extra row stays visible above/below - keyboard
    // browsing near an edge always shows a preview of what's coming next. Clamping
    // in setValue() below naturally handles "there's no more content to nudge into"
    // at either end of the grid, so no separate space check is needed here.
    int topGap = content.top() - scrollValue;
    int bottomGap = (scrollValue + viewport()->height()) - content.bottom();
    if (topGap < cellHeight_) {
        scrollValue -= (cellHeight_ - topGap);
    } else if (bottomGap < cellHeight_) {
        scrollValue += (cellHeight_ - bottomGap);
    }

    verticalScrollBar()->setValue(qBound(0, scrollValue, verticalScrollBar()->maximum()));
}

void ThumbGridView::moveCurrentRow(int delta, bool extendSelection) {
    int rc = rowCount();
    if (rc == 0) return;
    int newRow = currentRow_ < 0 ? 0 : qBound(0, currentRow_ + delta, rc - 1);

    int oldCurrent = currentRow_;
    // Ctrl+Shift+<key> isn't given a distinct meaning here (Explorer's "move focus
    // without selecting" is Ctrl+Arrow, already spent on folder navigation in
    // keyPressEvent) - it just extends the same as plain Shift+<key>, a reasonable
    // fallback rather than a dead combination.
    if (extendSelection && anchorRow_ >= 0) {
        selectRange(newRow, /*unionMode=*/false);
    } else {
        replaceSelectionWith(newRow);
    }
    applySelectionResult(oldCurrent);
    scrollToRow(newRow);
}

void ThumbGridView::paintEvent(QPaintEvent *event) {
    QPainter painter(viewport());
    painter.fillRect(event->rect(), palette().color(QPalette::Base));
    if (!model_ || columns_ <= 0 || cellHeight_ <= 0) {
        if (dragActive_) drawDropFeedback(painter);
        return;
    }

    int rc = rowCount();
    int scrollValue = verticalScrollBar()->value();
    const QPair<int, int> visible = visibleRowRange();
    if (visible.first < 0) {
        if (dragActive_) drawDropFeedback(painter);
        return;
    }
    int firstGridRow = visible.first / columns_;
    int lastGridRow = visible.second / columns_;

    for (int gridRow = firstGridRow; gridRow <= lastGridRow; ++gridRow) {
        for (int col = 0; col < columns_; ++col) {
            int row = gridRow * columns_ + col;
            if (row >= rc) break;
            QRect rect(gridOffsetX_ + col * columnStride_, gridRow * cellHeight_ - scrollValue, cellWidth_,
                        cellHeight_);
            paintCell(painter, row, rect);
        }
    }

    if (dragActive_) drawDropFeedback(painter);
}

void ThumbGridView::drawDropFeedback(QPainter &painter) const {
    // Move (the default) uses the same accent color as selection; Copy (Ctrl held)
    // gets a visually distinct amber - the OS's own drag cursor already shows the
    // standard copy/move badge too (a side effect of actually setting
    // QDropEvent::setDropAction() instead of blindly accepting the OS's proposal -
    // see dragEnterEvent/dragMoveEvent), so this is corroborating, not the only cue.
    QColor color = dragCopyMode_ ? QColor(230, 160, 30) : palette().color(QPalette::Highlight);
    QPen pen(color);
    pen.setWidth(3);
    painter.setPen(pen);
    painter.drawRect(viewport()->rect().adjusted(1, 1, -2, -2));

    QString label = dragCopyMode_ ? QStringLiteral("Copy") : QStringLiteral("Move");
    QRect labelRect = viewport()->rect().adjusted(8, 8, -8, -8);
    painter.setPen(color);
    painter.drawText(labelRect, Qt::AlignTop | Qt::AlignLeft, label);
}

void ThumbGridView::paintCell(QPainter &painter, int row, const QRect &cellRect) const {
    QModelIndex index = model_->index(row, 0);
    bool selected = isRowSelected(row);
    bool isLead = (row == currentRow_);

    // Soft border around every cell, regardless of selection.
    painter.setPen(palette().color(QPalette::Mid));
    painter.drawRect(cellRect.adjusted(0, 0, -1, -1));

    // Faint fill under the lead row specifically (drawn before the thumbnail so it
    // reads as a background tint, not an overlay) - in a multi-selection every
    // selected cell gets the highlight border below, but only one of them is what the
    // preview pane is actually showing, and this is what keeps that one visually
    // distinguishable from the rest.
    if (isLead && selected) {
        QColor tint = palette().color(QPalette::Highlight);
        tint.setAlpha(48);
        painter.fillRect(cellRect.adjusted(1, 1, -1, -1), tint);
    }

    // Thumbnail centered within a fixed-size image area at the top of the cell. ThumbLoader
    // already scaled the pixmap to fit iconSize_ x imageAreaHeight_ with aspect preserved, so
    // it typically fills one dimension and falls short in the other: a landscape shot fills
    // the width, a portrait one fills the height and is narrower than the cell.
    QRect imageArea(cellRect.left() + kCellPadding, cellRect.top() + kCellPadding,
                     cellRect.width() - 2 * kCellPadding, imageAreaHeight_);
    QVariant deco = model_->data(index, Qt::DecorationRole);
    if (deco.canConvert<QPixmap>()) {
        QPixmap pix = deco.value<QPixmap>();
        if (!pix.isNull()) {
            // deviceIndependentSize(), not size(): ThumbLoader decodes at iconSize *
            // devicePixelRatio and stamps the ratio on the pixmap, so size() is in device
            // pixels and on a Retina screen would be twice the box being centred in - which
            // would offset every thumbnail up and left by half its own size. drawPixmap()
            // itself honours the ratio, so only the positioning needs converting.
            QRect pixRect(QPoint(0, 0), pix.deviceIndependentSize().toSize());
            pixRect.moveCenter(imageArea.center());
            painter.drawPixmap(pixRect.topLeft(), pix);
        }
    }

    // Filename, centered below the image area.
    QRect textRect(cellRect.left() + kCellPadding, imageArea.bottom() + kTextTopGap,
                    cellRect.width() - 2 * kCellPadding, kTextRowHeight);
    QString name = model_->data(index, Qt::DisplayRole).toString();
    const bool hasGps = model_->data(index, ThumbGridModel::HasGpsRole).toBool();

    // Pin and name are centred together as one group rather than the pin being parked at the
    // left edge - otherwise adding a marker would visibly shift the filename off-centre, and
    // a grid where geotagged rows sit differently from the rest reads as misalignment rather
    // than as information. The elide width shrinks by exactly what the pin occupies, so a
    // long name still truncates cleanly instead of colliding with it.
    QFontMetrics fm(painter.font());
    const int pinWidth = hasGps ? qMax(6, fm.height() * 3 / 5) : 0;
    const int pinGap = hasGps ? 3 : 0;
    QString elided = fm.elidedText(name, Qt::ElideMiddle, textRect.width() - pinWidth - pinGap);
    const int textWidth = fm.horizontalAdvance(elided);
    int x = textRect.left() + (textRect.width() - (pinWidth + pinGap + textWidth)) / 2;
    if (hasGps) {
        drawGeotagPin(painter, QRect(x, textRect.top(), pinWidth, textRect.height()));
        x += pinWidth + pinGap;
    }
    painter.setPen(palette().color(QPalette::Text));
    painter.drawText(QRect(x, textRect.top(), textWidth, textRect.height()),
                      Qt::AlignLeft | Qt::AlignVCenter, elided);

    // Selection border on top - not a recolor, so the thumbnail's own colors stay
    // readable.
    if (selected) {
        QPen pen(palette().color(QPalette::Highlight));
        pen.setWidth(2);
        painter.setPen(pen);
        painter.drawRect(cellRect.adjusted(1, 1, -2, -2));
    }
}

void ThumbGridView::drawGeotagPin(QPainter &painter, const QRect &box) const {
    // The classic map-marker silhouette - a disc with a tapered tail - built as one path so
    // the join between the two stays clean when it's filled. Drawn in the muted
    // PlaceholderText role, the same role the section headings use for
    // legible-but-not-shouting text, so it reads as metadata next to the filename rather
    // than competing with it.
    //
    // Everything is proportional to the box (which is font-height derived), so it scales with
    // the UI font instead of needing an asset per device pixel ratio - it renders at roughly
    // 10px, where a bitmap would be the wrong size on half the machines this runs on.
    const qreal d = qMin<qreal>(box.width(), box.height() * 0.75);
    const QPointF centre(box.center().x(), box.top() + box.height() / 2.0 - d * 0.12);
    const qreal r = d * 0.34;

    QPainterPath path;
    path.addEllipse(centre, r, r);
    QPolygonF tail;
    tail << QPointF(centre.x() - r * 0.70, centre.y() + r * 0.60)
          << QPointF(centre.x() + r * 0.70, centre.y() + r * 0.60)
          << QPointF(centre.x(), centre.y() + d * 0.72);
    path.addPolygon(tail);

    // Saved/restored because paintCell() carries on drawing the filename with this same
    // painter straight afterwards, and would otherwise inherit the brush and pen set here.
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(palette().color(QPalette::PlaceholderText));
    painter.drawPath(path.simplified());
    painter.restore();
}

void ThumbGridView::resizeEvent(QResizeEvent *event) {
    QAbstractScrollArea::resizeEvent(event);
    relayout();
}

void ThumbGridView::wheelEvent(QWheelEvent *event) {
    accumulatedDelta_ += event->angleDelta().y();
    int notches = accumulatedDelta_ / 120; // 120 = one standard wheel notch

    if (notches != 0) {
        accumulatedDelta_ -= notches * 120;
        verticalScrollBar()->setValue(verticalScrollBar()->value() - notches * cellHeight_);
    }
    event->accept();
}

void ThumbGridView::keyPressEvent(QKeyEvent *event) {
    // Folder navigation moves the whole view to another folder, so it is checked before
    // the grid's own navigation below and, like ActivateFullscreen, has to go through
    // keybindings::matches() rather than a case label - once rebound the key isn't
    // necessarily an arrow at all. Defaults are Ctrl+arrow, which Qt renders and matches
    // as Cmd+arrow on macOS; see KeyBindings.h on why that difference is deliberate and
    // why physical Ctrl+arrow can't be the Mac default.
    for (const FolderNavBinding &nav : folderNavBindings_) {
        if (keybindings::matches(event, nav.sequence)) {
            emit navigateFolderRequested(nav.direction);
            event->accept();
            return;
        }
    }

    // Checked before the fixed-navigation switch below since the configured key
    // could be anything - it isn't necessarily Return/Enter anymore once rebound,
    // so it can't just be a case label keyed on event->key() the way the others are.
    if (keybindings::matches(event, keybindings::binding(keybindings::Action::ActivateFullscreen))) {
        if (currentRow_ >= 0) emit activated(currentRow_);
        event->accept();
        return;
    }

    // Shift extends the selection from anchorRow_ instead of replacing it - covers
    // Shift+Arrow/Home/End/PageUp/PageDown range-select in one change. See
    // moveCurrentRow() for why Ctrl+Shift+<key> isn't given its own distinct meaning.
    const bool extend = event->modifiers().testFlag(Qt::ShiftModifier);

    switch (event->key()) {
        case Qt::Key_Left:
            moveCurrentRow(-1, extend);
            event->accept();
            return;
        case Qt::Key_Right:
            moveCurrentRow(1, extend);
            event->accept();
            return;
        case Qt::Key_Up:
            moveCurrentRow(-columns_, extend);
            event->accept();
            return;
        case Qt::Key_Down:
            moveCurrentRow(columns_, extend);
            event->accept();
            return;
        case Qt::Key_Home:
            moveCurrentRow(-rowCount(), extend);
            event->accept();
            return;
        case Qt::Key_End:
            moveCurrentRow(rowCount(), extend);
            event->accept();
            return;
        case Qt::Key_PageUp:
            moveCurrentRow(-qMax(1, viewport()->height() / qMax(1, cellHeight_)) * columns_, extend);
            event->accept();
            return;
        case Qt::Key_PageDown:
            moveCurrentRow(qMax(1, viewport()->height() / qMax(1, cellHeight_)) * columns_, extend);
            event->accept();
            return;
        default:
            QAbstractScrollArea::keyPressEvent(event);
    }
}

void ThumbGridView::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::RightButton) {
        // Selects the row under the cursor first, matching Explorer - but only if
        // it isn't already part of the current selection, so right-clicking inside
        // an existing multi-selection to Cut/Copy the whole thing doesn't collapse
        // it down to just the one row that happened to be under the cursor. Right-
        // click on empty space leaves the selection untouched (MainWindow's context
        // menu still shows Paste, just with Cut/Copy/View Fullscreen disabled).
        int row = rowAt(event->pos());
        if (row >= 0 && !isRowSelected(row)) {
            int oldCurrent = currentRow_;
            replaceSelectionWith(row);
            applySelectionResult(oldCurrent);
        }
        return;
    }

    if (event->button() != Qt::LeftButton) return;
    int row = rowAt(event->pos());
    pressPos_ = event->pos();
    pressRow_ = row;
    pendingCollapseRow_ = -1;

    if (row < 0) {
        // Only a plain click on empty space deselects - a modified click there (e.g.
        // an accidental Ctrl+click past the last row) has no target row to act on,
        // so it's simplest and safest to leave the selection alone.
        if (event->modifiers() == Qt::NoModifier) clearSelection();
        return;
    }

    const Qt::KeyboardModifiers mods = event->modifiers();
    const int oldCurrent = currentRow_;

    if (mods & Qt::ShiftModifier) {
        selectRange(row, mods & Qt::ControlModifier);
        applySelectionResult(oldCurrent);
    } else if (mods & Qt::ControlModifier) {
        toggleRow(row);
        applySelectionResult(oldCurrent);
    } else if (isRowSelected(row)) {
        // Defer collapsing to just this row until mouseReleaseEvent (or until a drag
        // starts) rather than collapsing immediately - otherwise pressing on an
        // already-multi-selected thumbnail to start a drag-out would collapse the
        // selection to one item before the drag even began, the same way Explorer
        // avoids doing.
        pendingCollapseRow_ = row;
    } else {
        replaceSelectionWith(row);
        applySelectionResult(oldCurrent);
    }
}

void ThumbGridView::mouseMoveEvent(QMouseEvent *event) {
    if (!(event->buttons() & Qt::LeftButton) || pressRow_ < 0) {
        handleHoverMove(event->pos());

        int ctrlRow = (event->modifiers() & Qt::ControlModifier) ? rowAt(event->pos()) : -1;
        if (ctrlRow != ctrlHoverRow_) {
            ctrlHoverRow_ = ctrlRow;
            emit ctrlHoverRowChanged(ctrlRow);
        }

        QAbstractScrollArea::mouseMoveEvent(event);
        return;
    }
    // A real drag is in progress - the hover tooltip has no business showing up
    // mid-drag (over whatever cell the drag happens to pass over).
    hoverDelayTimer_->stop();
    if (hoverRow_ >= 0) {
        hoverRow_ = -1;
        QToolTip::hideText();
    }
    if (ctrlHoverRow_ >= 0) {
        ctrlHoverRow_ = -1;
        emit ctrlHoverRowChanged(-1);
    }
    if ((event->pos() - pressPos_).manhattanLength() < QApplication::startDragDistance()) return;

    // A drag consumes the deferred collapse - the multi-selection must survive into
    // the drag (see mousePressEvent's comment: pressing on an already-selected row
    // doesn't collapse the selection until release, specifically so a drag-out of a
    // multi-selection is still possible).
    pendingCollapseRow_ = -1;
    emit dragOutRequested();
}

void ThumbGridView::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && pendingCollapseRow_ >= 0) {
        int row = pendingCollapseRow_;
        pendingCollapseRow_ = -1;
        int oldCurrent = currentRow_;
        replaceSelectionWith(row);
        applySelectionResult(oldCurrent);
    }
    QAbstractScrollArea::mouseReleaseEvent(event);
}

void ThumbGridView::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) return;
    int row = rowAt(event->pos());
    if (row >= 0) {
        pendingCollapseRow_ = -1; // the second press already set this - resolve now instead
        int oldCurrent = currentRow_;
        replaceSelectionWith(row);
        applySelectionResult(oldCurrent);
        emit activated(row);
    }
}

void ThumbGridView::leaveEvent(QEvent *event) {
    hoverDelayTimer_->stop();
    if (hoverRow_ >= 0) {
        hoverRow_ = -1;
        QToolTip::hideText();
    }
    if (ctrlHoverRow_ >= 0) {
        ctrlHoverRow_ = -1;
        emit ctrlHoverRowChanged(-1);
    }
    QAbstractScrollArea::leaveEvent(event);
}

void ThumbGridView::keyReleaseEvent(QKeyEvent *event) {
    // Catches releasing Ctrl while the mouse sits still over a cell (no
    // mouseMoveEvent to notice it otherwise) - only reachable while this widget has
    // focus, which covers the common case (clicking into the grid before Ctrl-
    // hovering around it), not e.g. Ctrl released while some other widget has focus.
    if (event->key() == Qt::Key_Control && ctrlHoverRow_ >= 0) {
        ctrlHoverRow_ = -1;
        emit ctrlHoverRowChanged(-1);
    }
    QAbstractScrollArea::keyReleaseEvent(event);
}

void ThumbGridView::handleHoverMove(const QPoint &viewportPos) {
    if (!prefs::hoverInfoEnabled()) return;

    int row = rowAt(viewportPos);
    if (row == hoverRow_) return; // still the same cell (or still off-grid) - nothing to do

    hoverRow_ = row;
    hoverDelayTimer_->stop();
    QToolTip::hideText(); // the old cell's tooltip (if any) doesn't apply to the new one

    if (row >= 0) {
        hoverPos_ = viewportPos;
        hoverDelayTimer_->start();
    }
}

void ThumbGridView::hideHoverTooltip() {
    hoverDelayTimer_->stop();
    hoverRow_ = -1;
    QToolTip::hideText();
}

void ThumbGridView::onHoverDelayElapsed() {
    if (hoverRow_ < 0 || !model_) return;

    QToolTip::showText(viewport()->mapToGlobal(hoverPos_), cachedInfoText(hoverRow_), this);

    QModelIndex idx = model_->index(hoverRow_, 0);
    QString name = idx.data(Qt::DisplayRole).toString();
    if (hoverFolderPath_.isEmpty() || name.isEmpty()) return;

    int fmt = idx.data(ThumbGridModel::FormatRole).toInt();
    quint64 id = ++hoverInfoCounter_;
    hoverInfoRequestId_ = id;
    QMetaObject::invokeMethod(hoverInfoWorker_.get(), "request", Qt::QueuedConnection, Q_ARG(quint64, id),
                               Q_ARG(QString, joinPathQ(hoverFolderPath_, name)), Q_ARG(int, fmt));
}

void ThumbGridView::onHoverInfoReady(quint64 id, QString detailsText) {
    if (id != hoverInfoRequestId_ || detailsText.isEmpty()) return;
    // The reply arrived after the mouse already moved on to a different cell (or off
    // the grid entirely) - nothing to update.
    if (hoverRow_ < 0 || rowAt(viewport()->mapFromGlobal(QCursor::pos())) != hoverRow_) return;

    QString text = cachedInfoText(hoverRow_) + QStringLiteral("\n\n") + detailsText;
    QToolTip::showText(QCursor::pos(), text, this);
}

QString ThumbGridView::cachedInfoText(int row) const {
    QModelIndex idx = model_->index(row, 0);
    QString name = idx.data(Qt::DisplayRole).toString();
    int fmt = idx.data(ThumbGridModel::FormatRole).toInt();
    int w = idx.data(ThumbGridModel::WidthRole).toInt();
    int h = idx.data(ThumbGridModel::HeightRole).toInt();
    qint64 size = idx.data(ThumbGridModel::SizeRole).toLongLong();
    qint64 takenAt = idx.data(ThumbGridModel::TakenAtRole).toLongLong();
    qint64 durationMs = idx.data(ThumbGridModel::DurationMsRole).toLongLong();

    QStringList lines;
    lines << name;

    QStringList meta;
    meta << QString::fromUtf8(pixet::formatName((pixet::Format)fmt));
    if (w > 0 && h > 0) meta << QStringLiteral("%1×%2").arg(w).arg(h);
    if (size > 0) meta << QLocale().formattedDataSize(size);
    lines << meta.join(QStringLiteral(" · "));

    if (takenAt > 0) {
        lines << QStringLiteral("Taken: %1").arg(
            QDateTime::fromSecsSinceEpoch(takenAt).toString(QStringLiteral("yyyy-MM-dd hh:mm")));
    }
    if (durationMs > 0) {
        qint64 totalSec = durationMs / 1000;
        lines << QStringLiteral("Duration: %1:%2").arg(totalSec / 60).arg(totalSec % 60, 2, 10, QChar('0'));
    }

    return lines.join(QStringLiteral("\n"));
}

void ThumbGridView::dragEnterEvent(QDragEnterEvent *event) {
    // A drop of pixet's own drag-out (see MainWindow's drag-out handling) landing
    // back on this same view must be a no-op, not a self-import.
    if (event->source() == this) return;
    if (!droppolicy::hasLocalFileUrl(event->mimeData())) return;
    dragActive_ = true;
    dragCopyMode_ = droppolicy::wantsCopy(event->modifiers());
    event->setDropAction(dragCopyMode_ ? Qt::CopyAction : Qt::MoveAction);
    event->accept();
    viewport()->update();
}

void ThumbGridView::dragMoveEvent(QDragMoveEvent *event) {
    if (event->source() == this || !droppolicy::hasLocalFileUrl(event->mimeData())) return;
    // Modifiers can change mid-drag (Ctrl pressed/released while still hovering) -
    // re-evaluate every move, not just on entry, and repaint if the mode actually
    // flipped so drawDropFeedback()'s border color stays live.
    bool copyMode = droppolicy::wantsCopy(event->modifiers());
    if (copyMode != dragCopyMode_) {
        dragCopyMode_ = copyMode;
        viewport()->update();
    }
    event->setDropAction(dragCopyMode_ ? Qt::CopyAction : Qt::MoveAction);
    event->accept();
}

void ThumbGridView::dragLeaveEvent(QDragLeaveEvent *) {
    dragActive_ = false;
    viewport()->update();
}

void ThumbGridView::dropEvent(QDropEvent *event) {
    dragActive_ = false;
    viewport()->update();

    if (event->source() == this || !droppolicy::hasLocalFileUrl(event->mimeData())) return;

    QStringList paths;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile()) paths << url.toLocalFile();
    }
    if (paths.isEmpty()) return;

    bool copyMode = droppolicy::wantsCopy(event->modifiers());
    event->setDropAction(copyMode ? Qt::CopyAction : Qt::MoveAction);
    event->accept();
    emit filesDropped(paths, /*move=*/!copyMode);
}
