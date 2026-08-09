#include "ThumbGridView.h"

#include <QFontMetrics>
#include <QKeyEvent>
#include <QPainter>
#include <QPixmap>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTimer>
#include <QWheelEvent>

#include "ThumbLoader.h"

void ThumbGridDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    painter->save();

    QRect cellRect = option.rect;
    bool selected = option.state & QStyle::State_Selected;

    // Soft border around every cell, regardless of selection.
    painter->setPen(option.palette.color(QPalette::Mid));
    painter->drawRect(cellRect.adjusted(0, 0, -1, -1));

    // Thumbnail centered within a fixed-size image area at the top of the cell -
    // pixmaps are already scaled to fit within kThumbIconSize (aspect preserved, see
    // ThumbLoader), so they're frequently smaller than the box in one dimension.
    QRect imageArea(cellRect.left() + kCellPadding, cellRect.top() + kCellPadding,
                     cellRect.width() - 2 * kCellPadding, ThumbLoader::kThumbIconSize);
    QVariant deco = index.data(Qt::DecorationRole);
    if (deco.canConvert<QPixmap>()) {
        QPixmap pix = deco.value<QPixmap>();
        if (!pix.isNull()) {
            QRect pixRect(QPoint(0, 0), pix.size());
            pixRect.moveCenter(imageArea.center());
            painter->drawPixmap(pixRect.topLeft(), pix);
        }
    }

    // Filename, centered below the image area.
    QRect textRect(cellRect.left() + kCellPadding, imageArea.bottom() + kTextTopGap,
                    cellRect.width() - 2 * kCellPadding, kTextRowHeight);
    QString name = index.data(Qt::DisplayRole).toString();
    QString elided = QFontMetrics(option.font).elidedText(name, Qt::ElideMiddle, textRect.width());
    painter->setPen(option.palette.color(QPalette::Text));
    painter->drawText(textRect, Qt::AlignHCenter | Qt::AlignVCenter, elided);

    // Selection border on top - not a recolor, so the thumbnail's own colors stay
    // readable (see class comment).
    if (selected) {
        QPen pen(option.palette.color(QPalette::Highlight));
        pen.setWidth(2);
        painter->setPen(pen);
        painter->drawRect(cellRect.adjusted(1, 1, -2, -2));
    }

    painter->restore();
}

QSize ThumbGridDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const {
    Q_UNUSED(option);
    Q_UNUSED(index);
    if (auto *view = qobject_cast<const QListView *>(parent())) {
        QSize grid = view->gridSize();
        if (grid.isValid()) return grid;
    }
    // Fallback for the (practically never hit) case of no parent view yet - a
    // reasonable single-column-width guess using the same layout constants.
    int height = kCellPadding + ThumbLoader::kThumbIconSize + kTextTopGap + kTextRowHeight + kCellPadding;
    return QSize(ThumbLoader::kThumbIconSize + 2 * kCellPadding, height);
}

ThumbGridView::ThumbGridView(QWidget *parent) : QListView(parent) {
    // Keyboard (Page Up/Down) and scrollbar-arrow clicks also step by one row now,
    // consistent with the wheel behavior below.
    setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerItem);
    setItemDelegate(new ThumbGridDelegate(this));

    resizeDebounce_ = new QTimer(this);
    resizeDebounce_->setSingleShot(true);
    resizeDebounce_->setInterval(50);
    connect(resizeDebounce_, &QTimer::timeout, this, &ThumbGridView::updateGridSize);
}

void ThumbGridView::resizeEvent(QResizeEvent *event) {
    QListView::resizeEvent(event);
    resizeDebounce_->start(); // (re)starts - only fires once resizing actually pauses
}

void ThumbGridView::updateGridSize() {
    if (fitInProgress_) {
        // Don't just drop this - the width that prompted it (e.g. the final settled
        // size of a maximize, arriving while an earlier intermediate-size fit is
        // still converging) needs to be checked once that fit is done, or the grid
        // stays sized for a stale, since-superseded width.
        pendingRecheck_ = true;
        return;
    }

    int vw = viewport()->width();
    // Width close to one we already found a working fit for - most likely our own
    // scrollbar-visibility side effect bouncing back, not a real resize. Re-deriving
    // here would just repeat the same failing guess (see the class comment on why).
    if (lastFitWidth_ >= 0 && qAbs(vw - lastFitWidth_) <= kWidthJitterTolerance) return;

    fitInProgress_ = true;
    applyColumns(idealColumns());
}

int ThumbGridView::idealColumns() const {
    int viewportWidth = viewport()->width();
    if (viewportWidth <= 0) return 1;
    int baseCellWidth = ThumbLoader::kThumbIconSize + 2 * ThumbGridDelegate::kCellPadding;
    return qMax(1, viewportWidth / baseCellWidth);
}

void ThumbGridView::applyColumns(int columns, int attemptsLeft) {
    int viewportWidth = viewport()->width();
    if (viewportWidth <= 0 || columns <= 0) {
        finishFit();
        return;
    }

    int cellWidth = viewportWidth / columns; // stretched to fill the row evenly
    int cellHeight = ThumbGridDelegate::kCellPadding + ThumbLoader::kThumbIconSize + ThumbGridDelegate::kTextTopGap +
                      ThumbGridDelegate::kTextRowHeight + ThumbGridDelegate::kCellPadding;

    QSize newSize(cellWidth, cellHeight);
    if (newSize != gridSize()) setGridSize(newSize);

    if (attemptsLeft <= 0) {
        finishFit();
        return;
    }
    QTimer::singleShot(0, this, [this, columns, attemptsLeft]() {
        int actual = renderedColumnCount();
        // Only back off when short - overshooting isn't the reported problem and
        // "more, slightly smaller-than-target columns" isn't actually wrong, just a
        // different density than the baseline guess.
        if (actual > 0 && actual < columns) {
            applyColumns(columns - 1, attemptsLeft - 1); // continuation - still in progress
        } else {
            finishFit(); // converged (or nothing to verify yet) - done
        }
    });
}

void ThumbGridView::finishFit() {
    fitInProgress_ = false;
    lastFitWidth_ = viewport()->width();
    if (pendingRecheck_) {
        pendingRecheck_ = false;
        updateGridSize(); // re-evaluate against whatever the width actually is now
    }
}

int ThumbGridView::renderedColumnCount() const {
    if (!model() || model()->rowCount() == 0) return 0;
    QRect firstRect = visualRect(model()->index(0, 0));
    int cols = 1;
    for (int i = 1; i < model()->rowCount(); ++i) {
        QRect r = visualRect(model()->index(i, 0));
        if (r.top() != firstRect.top()) break;
        cols++;
    }
    return cols;
}

void ThumbGridView::scrollTo(const QModelIndex &index, ScrollHint hint) {
    QListView::scrollTo(index, hint);
    if (hint != EnsureVisible || !index.isValid()) return;

    int rowHeight = gridSize().height();
    if (rowHeight <= 0) rowHeight = iconSize().height();
    if (rowHeight <= 0) return;

    // Re-queried after the base scroll above, so this reflects where the item
    // actually ended up - if that's within one row of either edge, nudge further so
    // a full extra row remains visible (scrollbar range clamps this naturally at
    // either end of the grid, so no separate "is there space" check is needed).
    QRect itemRect = visualRect(index);
    if (!itemRect.isValid()) return;
    QRect viewportRect = viewport()->rect();

    int topGap = itemRect.top() - viewportRect.top();
    int bottomGap = viewportRect.bottom() - itemRect.bottom();
    if (topGap < rowHeight) {
        verticalScrollBar()->setValue(verticalScrollBar()->value() - (rowHeight - topGap));
    } else if (bottomGap < rowHeight) {
        verticalScrollBar()->setValue(verticalScrollBar()->value() + (rowHeight - bottomGap));
    }
}

void ThumbGridView::wheelEvent(QWheelEvent *event) {
    accumulatedDelta_ += event->angleDelta().y();
    int notches = accumulatedDelta_ / 120; // 120 = one standard wheel notch

    if (notches != 0) {
        accumulatedDelta_ -= notches * 120;

        int rowHeight = gridSize().height();
        if (rowHeight <= 0) rowHeight = iconSize().height();

        verticalScrollBar()->setValue(verticalScrollBar()->value() - notches * rowHeight);
    }

    // Never fall through to QListView::wheelEvent - that's the default smooth/pixel
    // scroller this class exists to replace.
    event->accept();
}

void ThumbGridView::keyPressEvent(QKeyEvent *event) {
    if (event->modifiers() == Qt::ControlModifier) {
        switch (event->key()) {
            case Qt::Key_Up:
            case Qt::Key_Down:
            case Qt::Key_Left:
            case Qt::Key_Right:
                emit navigateFolderRequested((Qt::Key)event->key());
                event->accept();
                return;
            default:
                break;
        }
    }
    QListView::keyPressEvent(event);
}
