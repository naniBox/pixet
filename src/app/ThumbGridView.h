#pragma once

#include <QListView>

// QListView's built-in wheel handling scrolls by a pixel amount scaled by the OS's
// "lines per wheel notch" setting (usually 3), which reads as smooth/continuous
// scrolling over a grid of thumbnails. This subclass makes one wheel notch move by
// exactly one row, so scrolling feels like discrete pagination instead.
class ThumbGridView : public QListView {
    Q_OBJECT

public:
    explicit ThumbGridView(QWidget *parent = nullptr);

protected:
    void wheelEvent(QWheelEvent *event) override;
};
