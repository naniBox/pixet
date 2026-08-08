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

private:
    // High-resolution mice/trackpads deliver many small fractional wheel deltas
    // instead of one clean 120-unit notch per click - accumulate across events so a
    // full notch's worth of rotation still triggers exactly one discrete row-step,
    // rather than falling through to smooth per-pixel scrolling for every sub-notch
    // event (which is what silently happened before this field existed).
    int accumulatedDelta_ = 0;
};
