#pragma once

#include <QListWidget>

// The bookmarks list, with drag-to-reorder.
//
// The drag itself is stock QListWidget (InternalMove); the only thing this class adds is a
// reliable "the rows are now in a new order" signal, which Qt does not otherwise offer for
// this widget.
//
// Wrapping startDrag() is what makes that reliable, because Qt has two different
// internal-move implementations and they finish at different moments. QListView (this
// widget's base, in list mode) does the move itself inside dropEvent() via
// QListModel::moveRows, relocating the QListWidgetItem and setting
// QAbstractItemViewPrivate::dropEventMoved so the generic path below is skipped. Every
// other case falls back to QAbstractItemView, where an internal move is two separate
// edits: dropEvent() inserts a decoded copy at the new position, and the *source* row is
// removed afterwards in startDrag(), once the nested QDrag::exec() loop that delivered the
// drop has returned. Reading the row order from dropEvent() would therefore be correct on
// the first path and see the list mid-move - with the dragged bookmark present twice - on
// the second. startDrag() returning is the first moment both are settled.
//
// Connecting to the model's rowsMoved would be shorter, but only the first path emits it,
// so it would quietly stop persisting anything if Qt ever changed which one runs.
class BookmarkListWidget : public QListWidget {
    Q_OBJECT

public:
    explicit BookmarkListWidget(QWidget *parent = nullptr);

signals:
    // Emitted once per drag that actually rearranged the list. A drag the user aborted, or
    // one that dropped a row back where it started, emits nothing - so a connected slot can
    // treat this as "the user reordered the bookmarks", not merely "a drag ended".
    void orderChanged();

protected:
    void startDrag(Qt::DropActions supportedActions) override;

private:
    // The per-row bookmark ids (Qt::UserRole + 1), top to bottom - what startDrag()
    // compares before and after to decide whether anything actually moved.
    QList<qint64> rowIds() const;
};
