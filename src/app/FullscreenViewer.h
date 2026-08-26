#pragma once

#include <QHash>
#include <QPixmap>
#include <QPointF>
#include <QString>
#include <QWidget>

#include <memory>

class QPainter;
class QTimer;
class ThumbGridModel;
class FullscreenDecoder;

// Fullscreen image viewer (P3). Opened via double-click/Enter on a thumbnail, and
// closed by Enter too - a toggle, not just an open action. Shows
// the current image scaled to fit the screen by default. Click zooms to 1:1 native
// pixels, centered on the clicked point; Z does the same fit<->1:1 toggle from the
// keyboard, centered on the image's middle instead (see toggleZoomKeyboard()); Ctrl+
// scroll zooms continuously, centered on the cursor; click-drag pans while zoomed;
// click again (or double-click) leaves zoom/closes. Left/Right/Up/Down or plain
// scroll move to the previous/next file in the same folder, keeping the main
// window's grid selection in sync (rowChanged) so closing lands back on whatever was
// last shown here. F toggles between borderless fullscreen and a maximized window
// with a title bar (showing the current image's full path); I toggles an info
// overlay; Escape, Enter/Return, or double-click closes.
//
// A small ring buffer keeps the current image's +/-2 neighbors' fit-scaled decodes
// prefetched, so next/prev is normally instant. Separately, the *current* row's full
// native-resolution decode is also prefetched in the background (on its own decoder/
// thread, see zoomDecoder_) after a short idle debounce - a real decode of a RAW file
// is expensive enough (~1s) that doing it only reactively on the zoom click itself
// meant a visible pause before the sharp image appeared. The debounce means rapid
// next/prev browsing never triggers it for images only passed through; only the row
// the user actually settles on gets the native decode started ahead of time, so by the
// time a zoom click/scroll actually happens it is often already there. Never blocks on
// decode either way: a cache miss (prefetch not caught up, or a fresh zoom request
// still in flight) falls back to instantly showing/upscaling whatever's already
// available - the grid's cached thumbnail, or the fit-scaled image in the zoom case -
// and swaps in the real decode once it lands. There is never a blank frame.
//
// Every format has a full decoder (see DisplayCodec) except Unknown, which just shows
// its already-cached grid thumbnail (there is none for Unknown in practice - unrecognized
// files aren't indexed at all), same graceful degradation as elsewhere in the app. Video
// "zooms" to its one extracted poster frame at native resolution, same as any still
// image - there's no live playback here (see decodeVideoPosterFrame), just a sharper
// look at the same frame the grid thumbnail was made from.
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

    // Follow a selection change the user made in the main window while this viewer is
    // still up - reachable because the viewer can be moved aside or switched to windowed
    // mode (F), leaving the grid clickable behind it. Without this the two drift apart
    // and the viewer keeps showing whatever it was opened on.
    //
    // Takes the directory as well as the row because the main window can change folder
    // underneath us, not just row: everything cached in here is keyed by row, and a row
    // means a different file in a different folder.
    //
    // A no-op when the viewer is hidden, so MainWindow can call it unconditionally, and
    // when the row is already the one on screen - that second guard is also what stops
    // this bouncing, since showRow() emits rowChanged() and MainWindow answers that by
    // moving the grid's current row back to the same place.
    void followGridSelection(const QString &directoryPath, int row);

signals:
    // Fired whenever the displayed row changes (navigation, or the initial open) -
    // lets MainWindow keep the grid's selection in sync so closing the viewer lands
    // back on the same image there.
    void rowChanged(int row);
    // Right-click. Carries the row and a global position so MainWindow can build and show
    // the same per-item menu the grid uses - the viewer deliberately doesn't build one
    // itself, since everything in it (cached info, the on-demand EXIF read, the Edit
    // actions) lives on that side.
    void contextMenuRequested(int row, QPoint globalPos);

protected:
    void paintEvent(QPaintEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
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
    // Separate instance/thread from decoder_ specifically so a slow native-resolution
    // decode (zoom, whether prefetched or click-triggered) can never sit in front of a
    // fit-scaled decode that's needed right now for next/prev to feel instant - see the
    // class comment. Both share requestCounter_'s id space and funnel into the same
    // onDecoded() - it dispatches by id, not by which decoder produced it.
    std::unique_ptr<FullscreenDecoder> zoomDecoder_;
    qint64 requestCounter_ = 0;
    // Debounced trigger for prefetching the current row's native decode - see the class
    // comment. Restarted (not just started) on every row change, so it only actually
    // fires once the user stops navigating for a moment.
    QTimer *zoomPrefetchTimer_ = nullptr;

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

    // vs. a maximized window with a title bar (F key). Initialized from
    // prefs::settingsStore() in the constructor and written back there on every
    // toggle, so this is a persisted preference (survives closing and relaunching
    // the app), not just a within-session one - see the .cpp.
    bool trueFullscreen_ = true;
    int infoOverlayLevel_ = 0;   // 0 = off, 1 = on (I key) - see the .cpp for why not full EXIF cycling

    QPoint dragStartMouse_;
    QPointF dragStartCenter_;
    bool dragging_ = false;
    bool dragMoved_ = false; // past the click-vs-drag movement threshold

    void showRow(int row, bool resetZoom);
    // Drops every cached/in-flight decode. Shared by openAt() and by
    // followGridSelection() when the folder changed - both are cases where the existing
    // row-keyed caches now describe different files entirely.
    void resetDecodeState();
    QString pathForRow(int row) const;
    int formatForRow(int row) const;
    QPixmap thumbnailForRow(int row) const;
    QSize nativeSizeForRow(int row) const;

    void requestFit(int row);
    void prefetchNeighbors();
    void trimFitCache();
    void requestZoom(int row);
    // zoomPrefetchTimer_'s timeout handler - requestZoom() for whichever row the user
    // has actually settled on.
    void prefetchZoom();

    void zoomAtCursor(const QPoint &cursorPos, int angleDeltaY);
    // Z key: same fit<->1:1 toggle as a plain click (see mouseReleaseEvent()), but
    // with no click position to derive a zoom target from - centers on the image's
    // middle instead, same as clicking dead-center of it would.
    void toggleZoomKeyboard();
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
    // The scale_ value that means "one image pixel per screen *device* pixel", i.e. what a
    // user means by 1:1. Not simply 1.0, because scale_ is in logical points - see the
    // implementation.
    qreal oneToOneScale() const;
};
