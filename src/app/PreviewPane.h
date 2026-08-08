#pragma once

#include <QImage>
#include <QPixmap>
#include <QWidget>

class QLabel;

// Simple side-preview widget: shows whatever PreviewDecoder last produced, scaled to
// fit while preserving aspect ratio. Keeps the undecoded-resolution QPixmap around so
// a window resize just rescales in place rather than triggering a redecode.
class PreviewPane : public QWidget {
    Q_OBJECT

public:
    explicit PreviewPane(QWidget *parent = nullptr);

    // Long edge to request the next decode at, sized to how big this pane currently is.
    int preferredTargetLongEdge() const;

public slots:
    void setImage(const QImage &image);
    void clear();

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    QLabel *label_;
    QPixmap original_;

    void updateScaledPixmap();
};
