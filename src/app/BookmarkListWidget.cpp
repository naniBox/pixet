#include "BookmarkListWidget.h"

BookmarkListWidget::BookmarkListWidget(QWidget *parent) : QListWidget(parent) {
    // InternalMove is what confines this to reordering: QAbstractItemView drops any drag
    // whose source isn't this widget, so a folder dragged in from Explorer/Finder (or from
    // pixet's own thumbnail grid, which drags out real file URLs) lands on the bookmarks
    // list as a no-op rather than as something half-defined.
    setDragDropMode(QAbstractItemView::InternalMove);
    // InternalMove alone leaves the drag's default action platform-dependent; naming Move
    // explicitly is what makes QAbstractItemView::startDrag() remove the source row, which
    // is the difference between reordering the list and duplicating a row into it.
    setDefaultDropAction(Qt::MoveAction);
    setDropIndicatorShown(true);
}

QList<qint64> BookmarkListWidget::rowIds() const {
    QList<qint64> ids;
    ids.reserve(count());
    for (int row = 0; row < count(); ++row) ids << item(row)->data(Qt::UserRole + 1).toLongLong();
    return ids;
}

void BookmarkListWidget::startDrag(Qt::DropActions supportedActions) {
    const QList<qint64> before = rowIds();
    QListWidget::startDrag(supportedActions); // returns only once the move is fully applied
    if (rowIds() != before) emit orderChanged();
}
