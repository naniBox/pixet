#include "FolderTreeView.h"

#include <QScrollBar>

FolderTreeView::FolderTreeView(QWidget *parent) : QTreeView(parent) {}

void FolderTreeView::mousePressEvent(QMouseEvent *event) {
    int hScroll = horizontalScrollBar()->value();
    QTreeView::mousePressEvent(event);
    horizontalScrollBar()->setValue(hScroll);
}

void FolderTreeView::keyPressEvent(QKeyEvent *event) {
    int hScroll = horizontalScrollBar()->value();
    QTreeView::keyPressEvent(event);
    horizontalScrollBar()->setValue(hScroll);
}
