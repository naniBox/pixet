#pragma once

#include <QTreeView>

// Qt's native click/keyboard-navigation handling in QTreeView resets horizontal
// scroll back to the left as part of selecting a new current item (it scrolls to
// reveal the row's start) - this happens *inside* QTreeView's own event handling,
// before any currentChanged signal fires, so there's no way to intercept it from
// outside. Save/restore horizontal scroll directly around the base class calls
// instead, regardless of what internal auto-scroll Qt performs during them.
class FolderTreeView : public QTreeView {
    Q_OBJECT

public:
    explicit FolderTreeView(QWidget *parent = nullptr);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void restoreHorizontalScroll(int value);
};
