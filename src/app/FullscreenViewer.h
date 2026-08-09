#pragma once

#include <QHash>
#include <QPixmap>
#include <QPointF>
#include <QString>
#include <QWidget>

#include <memory>

class QPainter;
class ThumbGridModel;
class FullscreenDecoder;

// Fullscreen image viewer (P3). Opened via double-click/Enter on a thumbnail. Shows
// the current image scaled to fit the screen by default. Click zooms to 1:1 native
// pixels, centered on the clicked point; Ctrl+scroll zooms continuously, centered on
// the cursor; click-drag pans while zoomed; click again (or double-click) leaves
// zoom/closes. Left/Right/Up/Down or plain scroll move to the previous/next file in
// the same folder, keeping the main window's grid selection in sync (rowChanged) so
// closing lands back on whatever was last shown here. F toggles between borderless
// fullscreen and a maximized window with a title bar (showing the current image's
// full path); I toggles an info overlay; Escape or double-click closes.
//
// A small ring buffer keeps the current image's +/-2 neighbors' fit-scaled decodes
// prefetched, so next/prev is normally instant. Never blocks on decode: a cache miss
// (prefetch not caught up, or a fresh zoom request still in flight) falls back to
// instantly showing/upscaling whatever's already available - the grid's cached
// thumbnail, or the fit-scaled image in the zoom case - and swaps in the real decode
// once it lands. There is never a blank frame.
//
// Only JPEG has a full decoder right now (see JpegCodec) - other formats just show
// their already-cached grid thumbnail, same graceful degradation as elsewhere in the
// app; zoom/pan is unavailable for them since it needs the image's real dimensions.
class FullscreenViewer : public QWidget {
    Q_OBJECT

public:
    explicit FullscreenViewer(QWidget *parent = nullptr);
    ~FullscreenViewer() override;

    // Opens fullscreen showing row `startRow` of `model` (files live in
    // `directoryPath`). `model` is not owned - must outlive this call (MainWindow's
    // gridModel_, whose lifetime already exceeds any fullscreen session opened from
    // it).
    void openAt(ThumbGridModel *model, const QString &directoryPath, int startRow);

signals:
    // Fired whenever the displayed row changes (navigation, or the initial open) -
    // lets MainWindow keep the grid's selection in sync so closing the viewer lands
    // back on the same image there.
    void rowChanged(int row);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onDecoded(qint64 requestId, QImage image);

private:
    ThumbGridModel *model_ = nullptr; // not owned
    QString directoryPath_;
    int currentRow_ = -1;

    std::unique_ptr<FullscreenDecoder> decoder_;
    qint64 requestCounter_ = 0;

    struct FitEntry {
        QPixmap pixmap;
        bool requested = false;
    };
    QHash<int, FitEntry> fitCache_;    // row -> fit-scaled (display resolution) decode
    QHash<qint64, int> fitRequestRow_; // in-flight fit request id -> row

    QPixmap zoomPixmap_; // full-resolution decode, once loaded
    int zoomPixmapRow_ = -1;
    qint64 zoomRequestId_ = -1;
    int zoomRequestRow_ = -1;

    // Zoom/pan state. fitMode_ means "ignore scale_/centerImagePoint_, recompute a
    // scale-to-fit every paint" - the default view. Leaving it (click, or
    // Ctrl+scroll) switches to an explicit scale_ (1.0 == the image's native
    // resolution) and centerImagePoint_ (which native-resolution pixel is centered
    // in the viewport) - both in the *original image's* pixel coordinate space,
    // rather than any particular decoded pixmap's, so they stay meaningful across
    // whichever source paintEvent ends up sampling from (fit-scaled placeholder vs.
    // full-resolution once it lands) - that unification is also what makes the
    // "never blank, upscale a placeholder meanwhile" behavior fall out for free
    // instead of needing a separate code path.
    bool fitMode_ = true;
    qreal scale_ = 1.0;
    QPointF centerImagePoint_;

    bool trueFullscreen_ = true; // vs. a maximized window with a title bar (F key) - persists across opens
    int infoOverlayLevel_ = 0;   // 0 = off, 1 = on (I key) - see the .cpp for why not full EXIF cycling

    QPoint dragStartMouse_;
    QPointF dragStartCenter_;
    bool dragging_ = false;
    bool dragMoved_ = false; // past the click-vs-drag movement threshold

    void showRow(int row, bool resetZoom);
    QString pathForRow(int row) const;
    int formatForRow(int row) const;
    QPixmap thumbnailForRow(int row) const;
    QSize nativeSizeForRow(int row) const;

    void requestFit(int row);
    void prefetchNeighbors();
    void trimFitCache();
    void requestZoom(int row);

    void zoomAtCursor(const QPoint &cursorPos, int angleDeltaY);
    void clampCenterImagePoint();
    void updateCursor();
    void updateWindowTitle();
    void drawInfoOverlay(QPainter &painter) const;
    // Screen-space rect the fit-scaled `pixmap` is drawn into (letterboxed, centered) -
    // used by the click handler to translate a click into a fraction across the image.
    QRect fitTargetRect(const QPixmap &pixmap) const;
    // Current effective scale (native-resolution pixels -> screen pixels) and the
    // native-resolution point centered in the viewport - fitMode_-aware.
    qreal effectiveScale(const QSize &nativeSize) const;
    QPointF effectiveCenter(const QSize &nativeSize) const;
};
