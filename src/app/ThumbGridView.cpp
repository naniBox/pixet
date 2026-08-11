#include "ThumbGridView.h"

#include <QAbstractItemModel>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QResizeEvent>
#include <QScrollBar>
#include <QWheelEvent>

ThumbGridView::ThumbGridView(QWidget *parent) : QAbstractScrollArea(parent) {
    // Always reserve the vertical scrollbar's width, even when nothing needs
    // scrolling, rather than the default show/hide-as-needed - see the class
    // comment on why a fluctuating viewport width is exactly the thing that made
    // the old implementation unpredictable. No horizontal scrolling at all: the
    // grid always stretches cells to fill the viewport width evenly (see relayout()).
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);

    // QAbstractScrollArea doesn't repaint automatically when the scrollbar value
    // changes (unlike QAbstractItemView, which this no longer is) - painting reads
    // verticalScrollBar()->value() directly, so a scroll is a repaint.
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() { viewport()->update(); });
}

void ThumbGridView::setModel(QAbstractItemModel *model) {
    if (model_) model_->disconnect(this);
    model_ = model;
    currentRow_ = -1;
    if (model_) {
        // ThumbGridModel only ever does a full reset (setDirectory) or in-place
        // dataChanged (refreshThumbStates/setThumbnail) - never row insert/remove
        // mid-lifetime, so those are the only two signals worth connecting.
        connect(model_, &QAbstractItemModel::modelReset, this, [this]() {
            currentRow_ = -1;
            relayout();
        });
        connect(model_, &QAbstractItemModel::dataChanged, this, [this](const QModelIndex &, const QModelIndex &) {
            viewport()->update();
        });
    }
    relayout();
}

void ThumbGridView::setIconSize(QSize size) {
    iconSize_ = size.width(); // square only - see header
    relayout();
}

QSize ThumbGridView::iconSize() const { return QSize(iconSize_, iconSize_); }

int ThumbGridView::rowCount() const { return model_ ? model_->rowCount() : 0; }

void ThumbGridView::relayout() {
    cellHeight_ = kCellPadding + iconSize_ + kTextTopGap + kTextRowHeight + kCellPadding;

    int vw = viewport()->width();
    if (vw > 0) {
        int baseCellWidth = iconSize_ + 2 * kCellPadding;
        columns_ = qMax(1, vw / baseCellWidth);
        cellWidth_ = vw / columns_; // stretched to fill the row evenly
    }

    int rc = rowCount();
    int totalRows = columns_ > 0 ? (rc + columns_ - 1) / columns_ : 0;
    int totalHeight = totalRows * cellHeight_;
    int vh = viewport()->height();
    verticalScrollBar()->setRange(0, qMax(0, totalHeight - vh));
    verticalScrollBar()->setPageStep(qMax(1, vh));
    verticalScrollBar()->setSingleStep(cellHeight_);

    viewport()->update();
}

QRect ThumbGridView::contentRect(int row) const {
    if (columns_ <= 0) return QRect();
    int gridRow = row / columns_;
    int gridCol = row % columns_;
    return QRect(gridCol * cellWidth_, gridRow * cellHeight_, cellWidth_, cellHeight_);
}

int ThumbGridView::rowAt(const QPoint &pos) const {
    if (columns_ <= 0 || cellWidth_ <= 0 || cellHeight_ <= 0) return -1;
    int contentY = pos.y() + verticalScrollBar()->value();
    if (contentY < 0 || pos.x() < 0) return -1;
    int gridRow = contentY / cellHeight_;
    int gridCol = pos.x() / cellWidth_;
    if (gridCol >= columns_) return -1; // clicked past the last column's cell (empty margin)
    int row = gridRow * columns_ + gridCol;
    if (row >= rowCount()) return -1;
    return row;
}

void ThumbGridView::setCurrentRow(int row) {
    int rc = rowCount();
    if (row < -1) row = -1;
    if (row >= rc) row = rc - 1;
    if (row == currentRow_) return;
    currentRow_ = row;
    viewport()->update();
    emit currentRowChanged(currentRow_);
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

void ThumbGridView::moveCurrentRow(int delta) {
    int rc = rowCount();
    if (rc == 0) return;
    int newRow = currentRow_ < 0 ? 0 : qBound(0, currentRow_ + delta, rc - 1);
    setCurrentRow(newRow);
    scrollToRow(newRow);
}

void ThumbGridView::paintEvent(QPaintEvent *event) {
    QPainter painter(viewport());
    painter.fillRect(event->rect(), palette().color(QPalette::Base));
    if (!model_ || columns_ <= 0 || cellHeight_ <= 0) return;

    int rc = rowCount();
    int scrollValue = verticalScrollBar()->value();
    int firstGridRow = qMax(0, scrollValue / cellHeight_);
    int lastGridRow = (scrollValue + viewport()->height()) / cellHeight_;

    for (int gridRow = firstGridRow; gridRow <= lastGridRow; ++gridRow) {
        for (int col = 0; col < columns_; ++col) {
            int row = gridRow * columns_ + col;
            if (row >= rc) break;
            QRect rect(col * cellWidth_, gridRow * cellHeight_ - scrollValue, cellWidth_, cellHeight_);
            paintCell(painter, row, rect);
        }
    }
}

void ThumbGridView::paintCell(QPainter &painter, int row, const QRect &cellRect) const {
    QModelIndex index = model_->index(row, 0);
    bool selected = (row == currentRow_);

    // Soft border around every cell, regardless of selection.
    painter.setPen(palette().color(QPalette::Mid));
    painter.drawRect(cellRect.adjusted(0, 0, -1, -1));

    // Thumbnail centered within a fixed-size image area at the top of the cell -
    // pixmaps are already scaled to fit within iconSize_ (aspect preserved, see
    // ThumbLoader), so they're frequently smaller than the box in one dimension.
    QRect imageArea(cellRect.left() + kCellPadding, cellRect.top() + kCellPadding,
                     cellRect.width() - 2 * kCellPadding, iconSize_);
    QVariant deco = model_->data(index, Qt::DecorationRole);
    if (deco.canConvert<QPixmap>()) {
        QPixmap pix = deco.value<QPixmap>();
        if (!pix.isNull()) {
            QRect pixRect(QPoint(0, 0), pix.size());
            pixRect.moveCenter(imageArea.center());
            painter.drawPixmap(pixRect.topLeft(), pix);
        }
    }

    // Filename, centered below the image area.
    QRect textRect(cellRect.left() + kCellPadding, imageArea.bottom() + kTextTopGap,
                    cellRect.width() - 2 * kCellPadding, kTextRowHeight);
    QString name = model_->data(index, Qt::DisplayRole).toString();
    QString elided = QFontMetrics(painter.font()).elidedText(name, Qt::ElideMiddle, textRect.width());
    painter.setPen(palette().color(QPalette::Text));
    painter.drawText(textRect, Qt::AlignHCenter | Qt::AlignVCenter, elided);

    // Selection border on top - not a recolor, so the thumbnail's own colors stay
    // readable.
    if (selected) {
        QPen pen(palette().color(QPalette::Highlight));
        pen.setWidth(2);
        painter.setPen(pen);
        painter.drawRect(cellRect.adjusted(1, 1, -2, -2));
    }
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

    switch (event->key()) {
        case Qt::Key_Left:
            moveCurrentRow(-1);
            event->accept();
            return;
        case Qt::Key_Right:
            moveCurrentRow(1);
            event->accept();
            return;
        case Qt::Key_Up:
            moveCurrentRow(-columns_);
            event->accept();
            return;
        case Qt::Key_Down:
            moveCurrentRow(columns_);
            event->accept();
            return;
        case Qt::Key_Home:
            moveCurrentRow(-rowCount());
            event->accept();
            return;
        case Qt::Key_End:
            moveCurrentRow(rowCount());
            event->accept();
            return;
        case Qt::Key_PageUp:
            moveCurrentRow(-qMax(1, viewport()->height() / qMax(1, cellHeight_)) * columns_);
            event->accept();
            return;
        case Qt::Key_PageDown:
            moveCurrentRow(qMax(1, viewport()->height() / qMax(1, cellHeight_)) * columns_);
            event->accept();
            return;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (currentRow_ >= 0) emit activated(currentRow_);
            event->accept();
            return;
        default:
            QAbstractScrollArea::keyPressEvent(event);
    }
}

void ThumbGridView::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) return;
    int row = rowAt(event->pos());
    if (row >= 0) setCurrentRow(row);
}

void ThumbGridView::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) return;
    int row = rowAt(event->pos());
    if (row >= 0) {
        setCurrentRow(row);
        emit activated(row);
    }
}
