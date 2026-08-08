#include "ThumbGridView.h"

#include <QScrollBar>
#include <QWheelEvent>

ThumbGridView::ThumbGridView(QWidget *parent) : QListView(parent) {
    // Keyboard (Page Up/Down) and scrollbar-arrow clicks also step by one row now,
    // consistent with the wheel behavior below.
    setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerItem);
}

void ThumbGridView::wheelEvent(QWheelEvent *event) {
    int notches = event->angleDelta().y() / 120; // 120 = one standard wheel notch
    if (notches == 0) {
        QListView::wheelEvent(event);
        return;
    }

    int rowHeight = gridSize().height();
    if (rowHeight <= 0) rowHeight = iconSize().height();

    verticalScrollBar()->setValue(verticalScrollBar()->value() - notches * rowHeight);
    event->accept();
}
