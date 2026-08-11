#pragma once

#include <QList>
#include <QWidget>

class StatusLabel;

// A row of StatusLabel cells, each with a "nominal" (ideal) width, separated by
// thin divider lines - positions and sizes every cell itself in resizeEvent()
// rather than delegating to a QHBoxLayout. QHBoxLayout turned out to have
// unpredictable, unusable behavior here once the row's total nominal width
// exceeded what's actually available (the normal case once the window gets
// reasonably narrow): with Qt::Fixed-policy cells (setFixedWidth()) it couldn't
// shrink them at all, so it packed them at overlapping X offsets instead of
// respecting their real rendered width; switching to a shrinkable maximumWidth
// instead just made it crush every cell down to a tiny (~6px), near-identical
// floor regardless of their very different nominal widths, rather than shrinking
// them proportionally. Neither is what "stay in their spot, truncate the text"
// (the actual goal) needs, so this widget does the arithmetic directly: when
// there's enough room, every cell gets its full nominal width; when there isn't,
// every cell shrinks by the same proportion, keeping their relative sizes (and so
// their relative screen positions) intact instead of any one of them collapsing
// disproportionately.
class StatusBarRow : public QWidget {
    Q_OBJECT

public:
    explicit StatusBarRow(QWidget *parent = nullptr);

    // Appends a new cell (with a divider before it, except the first) at the given
    // nominal width and returns it for the caller to setStatusText() on.
    StatusLabel *addLabel(int nominalWidth);

    QSize sizeHint() const override;

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    struct Entry {
        StatusLabel *label;
        QWidget *divider; // nullptr for the first entry
        int nominalWidth;
    };
    QList<Entry> entries_;

    void relayout();
};
