#include "FolderTreeView.h"

#include <QScrollBar>
#include <QTimer>

FolderTreeView::FolderTreeView(QWidget *parent) : QTreeView(parent) {}

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
