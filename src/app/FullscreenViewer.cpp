#include "FullscreenViewer.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QWheelEvent>

#include <cmath>

#include "FullscreenDecoder.h"
#include "KeyBindings.h"
#include "PathQ.h"
#include "Preferences.h"
#include "ThumbGridModel.h"
#include "db/Schema.h"
#include "version.h"

namespace {
// prefs::settingsStore() key trueFullscreen_ is persisted under - see the header's
// member comment and the constructor/keyPressEvent's F-key handler below. Deliberately
// not "fullscreenTrueFullscreen" - KeyBindings.cpp already uses that exact string as
// the *keybinding's* settings key (under a separate "keybindings" INI group, for what
// key triggers the toggle - default F). Different group so there's no actual
// collision, but the same string meaning two different things (one a QKeySequence,
// this one a bool) is exactly the kind of thing worth not doing anyway.
const QString kUseTrueFullscreenSettingsKey = QStringLiteral("fullscreenUseTrueFullscreen");

// Every format ThumbGenerator supports also has a decodeForDisplay() path (see
// DisplayCodec.h) - Video included, via its poster frame (see decodeVideoPosterFrame) -
// only Unknown has none, and falls back to the grid's cached thumbnail like before.
bool hasFullscreenDecoder(pixet::Format fmt) { return fmt != pixet::Format::Unknown; }

// Movement beyond this many pixels between press and release means "drag", not
// "click" - below it, small hand tremor while clicking shouldn't be read as a pan
// attempt and swallow the zoom toggle.
constexpr int kDragThreshold = 4;
// Ctrl+scroll zoom bounds and per-notch step. 0.1 keeps a runaway scroll from
// shrinking the image to nothing; 8.0 is a generous ceiling past 1:1 for detail
// inspection. 15% per notch feels continuous without being twitchy.
constexpr qreal kMinScale = 0.1;
constexpr qreal kMaxScale = 8.0;
constexpr qreal kScaleStepPerNotch = 1.15;

// How long the user has to stay on one image before its native-resolution decode
// starts prefetching in the background - long enough that holding next/prev to skim
// through a folder doesn't fire it for every image only briefly passed through, short
// enough that a real pause-to-look almost always has it ready before an actual zoom
// click/scroll follows.
constexpr int kZoomPrefetchDelayMs = 400;
} // namespace

FullscreenViewer::FullscreenViewer(QWidget *parent) : QWidget(parent) {
    setWindowFlag(Qt::Window);
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("background-color: black;"));
    setFocusPolicy(Qt::StrongFocus);

    // No parent - see the moveToThread/parent gotcha documented on MainWindow's
    // worker members; the same applies here.
    decoder_ = std::make_unique<FullscreenDecoder>();
    connect(decoder_.get(), &FullscreenDecoder::decoded, this, &FullscreenViewer::onDecoded);
    zoomDecoder_ = std::make_unique<FullscreenDecoder>();
    connect(zoomDecoder_.get(), &FullscreenDecoder::decoded, this, &FullscreenViewer::onDecoded);

    zoomPrefetchTimer_ = new QTimer(this);
    zoomPrefetchTimer_->setSingleShot(true);
    zoomPrefetchTimer_->setInterval(kZoomPrefetchDelayMs);
    connect(zoomPrefetchTimer_, &QTimer::timeout, this, &FullscreenViewer::prefetchZoom);

    trueFullscreen_ = prefs::settingsStore().value(kUseTrueFullscreenSettingsKey, trueFullscreen_).toBool();
}

FullscreenViewer::~FullscreenViewer() = default;

void FullscreenViewer::resetDecodeState() {
    fitCache_.clear();
    fitRequestRow_.clear();
    zoomPixmap_ = QPixmap();
    zoomPixmapRow_ = -1;
    zoomRequestId_ = -1;
    zoomRequestRow_ = -1;
    decoder_->cancelPending();
    zoomDecoder_->cancelPending();
    zoomPrefetchTimer_->stop();
}

void FullscreenViewer::openAt(QAbstractListModel *model, const QString &directoryPath, int startRow) {
    model_ = model;
    directoryPath_ = directoryPath;

    resetDecodeState();
    infoOverlayLevel_ = 0;

    // trueFullscreen_ deliberately isn't reset here - it's a persisted display
    // preference (F key, saved to prefs::settingsStore()), so re-opening on another
    // image - in this session or the next one - keeps whichever mode the user last
    // chose rather than jumping back to true fullscreen every time.
    if (trueFullscreen_) showFullScreen(); else showMaximized();
    setFocus();
    showRow(startRow, /*resetZoom=*/true);
}

void FullscreenViewer::followGridSelection(const QString &directoryPath, int row) {
    if (!isVisible() || !model_ || row < 0) return;

    const bool sameFolder = (directoryPath == directoryPath_);
    // Already showing it - the case that matters most, since this is what breaks the
    // cycle with MainWindow: showRow() below emits rowChanged(), MainWindow answers by
    // setting the grid's current row, and that comes straight back here. (The grid's own
    // setCurrentRow() also declines to re-emit for a row it is already on, so this is
    // belt and braces - but this side is the one that must hold, because it is the side
    // that would re-enter decoding.)
    if (sameFolder && row == currentRow_) return;

    if (!sameFolder) {
        // Folder changed under us (tree click, bookmark, path bar). The fit cache, the
        // zoom pixmap and every in-flight request are keyed by row against the *old*
        // folder, so they describe the wrong files now - same situation openAt() is in.
        directoryPath_ = directoryPath;
        resetDecodeState();
    }
    // resetZoom because this is a jump to an unrelated image rather than a step along a
    // sequence: keeping a previous image's zoom factor and centre point would land on an
    // arbitrary crop of the new one. Matches what a fresh openAt() does.
    showRow(row, /*resetZoom=*/true);
}

void FullscreenViewer::showRow(int row, bool resetZoom) {
    if (!model_ || row < 0 || row >= model_->rowCount()) return;

    currentRow_ = row;
    if (resetZoom) {
        fitMode_ = true;
        scale_ = 1.0;
        centerImagePoint_ = QPointF();
        updateCursor();
    }

    // Order matters here and reads backwards: FullscreenDecoder is a LIFO stack, so the
    // request pushed *last* is serviced *first*. Neighbours are therefore queued before the
    // row actually on screen, not after it.
    prefetchNeighbors();
    requestFit(row);
    // Restarting an already-running singleshot timer replaces whatever was left of its
    // wait with a fresh one, so this naturally debounces - only the row the user is
    // still on kZoomPrefetchDelayMs later actually triggers prefetchZoom().
    zoomPrefetchTimer_->start();
    updateWindowTitle();
    emit rowChanged(row);
    update();
}

QString FullscreenViewer::pathForRow(int row) const {
    if (!model_) return QString();
    QString name = model_->data(model_->index(row), Qt::DisplayRole).toString();
    if (name.isEmpty()) return QString();
    return joinPathQ(directoryPath_, name);
}

int FullscreenViewer::formatForRow(int row) const {
    if (!model_) return (int)pixet::Format::Unknown;
    return model_->data(model_->index(row), ThumbGridModel::FormatRole).toInt();
}

QPixmap FullscreenViewer::thumbnailForRow(int row) const {
    if (!model_) return QPixmap();
    return model_->data(model_->index(row), Qt::DecorationRole).value<QPixmap>();
}

QSize FullscreenViewer::nativeSizeForRow(int row) const {
    if (!model_ || row < 0) return QSize();
    QModelIndex idx = model_->index(row);
    int w = model_->data(idx, ThumbGridModel::WidthRole).toInt();
    int h = model_->data(idx, ThumbGridModel::HeightRole).toInt();
    return QSize(w, h);
}

void FullscreenViewer::requestFit(int row) {
    if (!model_ || row < 0 || row >= model_->rowCount()) return;
    if (!hasFullscreenDecoder((pixet::Format)formatForRow(row))) return; // thumbnail fallback only

    auto it = fitCache_.find(row);
    if (it != fitCache_.end() && (!it->pixmap.isNull() || it->requested)) return; // already have it or already asked

    fitCache_[row].requested = true;

    qint64 reqId = ++requestCounter_;
    fitRequestRow_[reqId] = row;
    // Display resolution, not full native size - see JpegCodec's scaled-DCT path. In *device*
    // pixels: width()/height() are logical points, so on a Retina screen decoding to them
    // yields half the pixels the display can actually show and the compositor upscales the
    // difference. The paint path below needs no matching change - it derives everything from
    // nativeSize and draws into logical rects, so handing it a higher-resolution source just
    // means more pixels to sample from at the same on-screen size.
    int targetLongEdge = qRound(qMax(width(), height()) * devicePixelRatioF());
    decoder_->request(reqId, pathForRow(row), formatForRow(row), targetLongEdge);
}

void FullscreenViewer::prefetchNeighbors() {
    if (!model_) return;
    // Neighbours only - the current row is requested by showRow() *after* this, so that it
    // sits on top of the LIFO stack and decodes first.
    //
    // Listed farthest-first for the same reason: FullscreenDecoder::processOne() takes from
    // the back, so this list is in reverse order of when things actually decode.
    //
    // Getting this order backwards is expensive rather than cosmetic, and the natural-
    // looking spelling is the wrong one: {0, -1, 1, -2, 2}, reading as "current row first,
    // then outward", is the precise opposite once the LIFO stack is applied to it - the row
    // two *ahead* decodes first and the visible image decodes fifth. A 4032x3024 JPEG
    // fit-decode measures ~65ms in a release build and ~520ms in a debug build on this
    // machine, so being five decodes late is a third of a second of black screen at best,
    // and over two seconds at worst, every time the viewer opens.
    for (int d : {2, -2, 1, -1}) requestFit(currentRow_ + d);
    trimFitCache();
}

void FullscreenViewer::trimFitCache() {
    QList<int> stale;
    for (auto it = fitCache_.constBegin(); it != fitCache_.constEnd(); ++it) {
        if (qAbs(it.key() - currentRow_) > 2) stale.append(it.key());
    }
    for (int row : stale) fitCache_.remove(row);
}

void FullscreenViewer::requestZoom(int row) {
    if (!model_ || row < 0 || row >= model_->rowCount()) return;
    if (!hasFullscreenDecoder((pixet::Format)formatForRow(row))) return;
    if (zoomPixmapRow_ == row && !zoomPixmap_.isNull()) return; // already have it
    if (zoomRequestRow_ == row) return;                                 // already in flight

    zoomRequestId_ = ++requestCounter_;
    zoomRequestRow_ = row;
    zoomDecoder_->request(zoomRequestId_, pathForRow(row), formatForRow(row), /*targetLongEdge=*/0);
}

void FullscreenViewer::prefetchZoom() {
    if (!model_ || currentRow_ < 0) return;
    requestZoom(currentRow_); // no-op if already cached or already in flight
}

void FullscreenViewer::onDecoded(qint64 requestId, QImage image) {
    auto fitIt = fitRequestRow_.find(requestId);
    if (fitIt != fitRequestRow_.end()) {
        int row = fitIt.value();
        fitRequestRow_.erase(fitIt);
        auto cacheIt = fitCache_.find(row);
        if (cacheIt != fitCache_.end()) {
            cacheIt->pixmap = QPixmap::fromImage(image);
            cacheIt->requested = false;
        }
        if (row == currentRow_) update();
        return;
    }

    if (requestId == zoomRequestId_) {
        zoomPixmap_ = QPixmap::fromImage(image);
        zoomPixmapRow_ = zoomRequestRow_;
        zoomRequestRow_ = -1;
        // This decode was asked for at targetLongEdge=0, so what came back *is* the file at
        // its native pixel size - the one measurement a model with no indexer behind it has
        // no other way to obtain. Emitted before the repaint below so a model that answers
        // this by filling in WidthRole/HeightRole (FolderListModel) has done so by the time
        // paintEvent() reads them, and the frame that shows the sharp pixels is also the
        // first frame that can zoom.
        if (!zoomPixmap_.isNull()) emit nativeSizeDiscovered(zoomPixmapRow_, zoomPixmap_.size());
        if (zoomPixmapRow_ == currentRow_) update();
        return;
    }

    // Superseded by a since-closed viewer, an abandoned prefetch neighborhood, or a
    // stale generation - nothing to do.
}

QRect FullscreenViewer::fitTargetRect(const QPixmap &pixmap) const {
    if (pixmap.isNull()) return QRect();
    QSize scaled = pixmap.size().scaled(size(), Qt::KeepAspectRatio);
    QPoint topLeft((width() - scaled.width()) / 2, (height() - scaled.height()) / 2);
    return QRect(topLeft, scaled);
}

qreal FullscreenViewer::oneToOneScale() const {
    // scale_ is measured in logical points per native image pixel, so a literal 1.0 means one
    // image pixel per *point* - which on a Retina display is two device pixels, i.e. the
    // "1:1" view was actually showing everything at 2x. Dividing by the ratio makes it one
    // image pixel per device pixel, which is what pixel-for-pixel is supposed to mean and
    // what you get in Preview.app.
    const qreal dpr = devicePixelRatioF();
    return dpr > 0.0 ? 1.0 / dpr : 1.0;
}

qreal FullscreenViewer::effectiveScale(const QSize &nativeSize) const {
    if (fitMode_) {
        if (nativeSize.width() <= 0 || nativeSize.height() <= 0) return 1.0;
        return qMin(qreal(width()) / nativeSize.width(), qreal(height()) / nativeSize.height());
    }
    return scale_;
}

QPointF FullscreenViewer::effectiveCenter(const QSize &nativeSize) const {
    if (fitMode_) return QPointF(nativeSize.width() / 2.0, nativeSize.height() / 2.0);
    return centerImagePoint_;
}

void FullscreenViewer::clampCenterImagePoint() {
    QSize nativeSize = nativeSizeForRow(currentRow_);
    if (nativeSize.width() <= 0 || nativeSize.height() <= 0) return;
    centerImagePoint_.setX(qBound(0.0, centerImagePoint_.x(), qreal(nativeSize.width())));
    centerImagePoint_.setY(qBound(0.0, centerImagePoint_.y(), qreal(nativeSize.height())));
}

void FullscreenViewer::updateCursor() {
    if (fitMode_) {
        setCursor(Qt::ArrowCursor);
    } else {
        setCursor(dragging_ ? Qt::ClosedHandCursor : Qt::OpenHandCursor);
    }
}

void FullscreenViewer::updateWindowTitle() {
    // Only ever seen in windowed mode (F) - true fullscreen has no title bar to put this in.
    // Path first, then version, then "pixet" last - the same shape as
    // MainWindow::updateWindowTitle(), and last for the same non-obvious reason: Qt appends
    // the application display name to any title that doesn't already end with it, so a title
    // arranged the natural way arrives on screen with "- pixet" stuck on the end. See that
    // function for the full explanation.
    //
    // Before this carried a version the title was the bare path, which likewise didn't end in
    // the display name - so what was actually on screen was Qt's "<path> - pixet", not the
    // path alone this line appeared to set.
    const QString path = pathForRow(currentRow_);
    const QString version = QString::fromLatin1(pixet::version());
    setWindowTitle(path.isEmpty() ? QStringLiteral("%1 - pixet").arg(version)
                                  : QStringLiteral("%1 - %2 - pixet").arg(path, version));
}

void FullscreenViewer::drawInfoOverlay(QPainter &painter) const {
    if (infoOverlayLevel_ <= 0 || !model_ || currentRow_ < 0) return;

    QModelIndex idx = model_->index(currentRow_);
    QStringList parts;
    parts << idx.data(Qt::DisplayRole).toString();
    parts << QString::fromUtf8(pixet::formatName((pixet::Format)idx.data(ThumbGridModel::FormatRole).toInt()));

    int w = idx.data(ThumbGridModel::WidthRole).toInt();
    int h = idx.data(ThumbGridModel::HeightRole).toInt();
    if (w > 0 && h > 0) parts << QStringLiteral("%1×%2").arg(w).arg(h);

    qint64 fileSize = idx.data(ThumbGridModel::SizeRole).toLongLong();
    if (fileSize > 0) parts << QLocale().formattedDataSize(fileSize);

    qint64 takenAt = idx.data(ThumbGridModel::TakenAtRole).toLongLong();
    if (takenAt > 0) {
        parts << QDateTime::fromSecsSinceEpoch(takenAt).toString(QStringLiteral("yyyy-MM-dd hh:mm"));
    }

    qint64 durationMs = idx.data(ThumbGridModel::DurationMsRole).toLongLong();
    if (durationMs > 0) {
        qint64 totalSec = durationMs / 1000;
        parts << QStringLiteral("%1:%2").arg(totalSec / 60).arg(totalSec % 60, 2, 10, QChar('0'));
    }

    QString text = parts.join(QStringLiteral("     "));

    QFont font = painter.font();
    font.setPointSize(12);
    painter.setFont(font);
    QFontMetrics fm(font);
    QRect textRect = fm.boundingRect(text).adjusted(-14, -8, 14, 8);
    textRect.moveBottomLeft(QPoint(24, height() - 24));

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 170));
    painter.drawRoundedRect(textRect, 6, 6);

    painter.setPen(Qt::white);
    painter.drawText(textRect, Qt::AlignCenter, text);
}

void FullscreenViewer::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    // Fit-scaled decodes only land on a power-of-2 scaled-DCT step (see
    // JpegCodec::decodeJpeg), so they're usually somewhat larger than the exact
    // on-screen size - drawPixmap's default (nearest-neighbor-ish) transform made that
    // visible as aliasing even in the plain default view. Smooth (bilinear) filtering
    // fixes both that and any zoom-level scaling.
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.fillRect(rect(), Qt::black);

    if (!model_ || currentRow_ < 0) return;

    QPixmap fitPixmap = fitCache_.value(currentRow_).pixmap;
    if (fitPixmap.isNull()) fitPixmap = thumbnailForRow(currentRow_);

    QSize nativeSize = nativeSizeForRow(currentRow_);
    if (nativeSize.width() <= 0 || nativeSize.height() <= 0) {
        // No known native size (metadata not indexed yet, or an unsupported format) -
        // show whatever's available, fit to screen; zoom/pan needs a real size to
        // compute against and is unavailable here.
        if (!fitPixmap.isNull()) painter.drawPixmap(fitTargetRect(fitPixmap), fitPixmap);
        drawInfoOverlay(painter);
        return;
    }

    // Prefer the full-resolution decode once available; otherwise render from
    // whatever's already on screen (fit-scaled decode or grid thumbnail). The same
    // math below handles both, so a cache miss at any zoom level is just "draw a
    // lower-resolution source at the same on-screen scale" - blurrier, never blank -
    // and self-corrects the moment onDecoded() lands the real thing.
    QPixmap source = (zoomPixmapRow_ == currentRow_ && !zoomPixmap_.isNull()) ? zoomPixmap_ : fitPixmap;
    if (source.isNull()) {
        drawInfoOverlay(painter);
        return;
    }

    qreal scale = effectiveScale(nativeSize);
    QPointF center = effectiveCenter(nativeSize);
    qreal sourceNativeScale = source.width() / qreal(nativeSize.width()); // source px per native px
    qreal screenPerSourcePx = scale / sourceNativeScale;

    QPointF centerSourcePoint = center * sourceNativeScale;
    QSizeF scaledSize(source.width() * screenPerSourcePx, source.height() * screenPerSourcePx);
    QPointF topLeft(width() / 2.0 - centerSourcePoint.x() * screenPerSourcePx,
                     height() / 2.0 - centerSourcePoint.y() * screenPerSourcePx);

    painter.drawPixmap(QRectF(topLeft, scaledSize), source, QRectF(QPointF(0, 0), source.size()));

    drawInfoOverlay(painter);
}

void FullscreenViewer::contextMenuEvent(QContextMenuEvent *event) {
    if (!model_ || currentRow_ < 0) return;
    // Cancel any drag-in-progress state first: the right-click arrives while a left button
    // may still be logically held (a press that never got its release because the menu
    // grabbed the mouse), and leaving dragging_ set would make the next move pan the image.
    dragging_ = false;
    updateCursor();
    emit contextMenuRequested(currentRow_, event->globalPos());
    event->accept();
}

void FullscreenViewer::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) return;
    dragStartMouse_ = event->pos();
    dragStartCenter_ = centerImagePoint_;
    dragging_ = true;
    dragMoved_ = false;
}

void FullscreenViewer::mouseMoveEvent(QMouseEvent *event) {
    if (!dragging_) return;
    QPoint screenDelta = event->pos() - dragStartMouse_;
    if (!dragMoved_ && screenDelta.manhattanLength() > kDragThreshold) {
        dragMoved_ = true;
        updateCursor();
    }
    if (!fitMode_ && dragMoved_) {
        // Dragging the mouse right/down should move the *view* left/up - grabbing
        // and pulling the image, not pushing the viewport - hence subtract. Divide
        // by scale_ to convert screen-space drag distance into image-space distance.
        QPointF imageDelta = QPointF(screenDelta) / scale_;
        centerImagePoint_ = dragStartCenter_ - imageDelta;
        clampCenterImagePoint();
        update();
    }
}

void FullscreenViewer::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton || !dragging_) return;
    dragging_ = false;
    updateCursor();
    if (dragMoved_) return; // was a pan, not a click - don't also toggle zoom

    if (!fitMode_) {
        fitMode_ = true;
        updateCursor();
        update();
        return;
    }

    QSize nativeSize = nativeSizeForRow(currentRow_);
    if (nativeSize.width() <= 0 || nativeSize.height() <= 0) return; // no known size - can't compute a zoom target

    QPixmap pixmap = fitCache_.value(currentRow_).pixmap;
    if (pixmap.isNull()) pixmap = thumbnailForRow(currentRow_);
    if (pixmap.isNull()) return;

    QRect target = fitTargetRect(pixmap);
    if (!target.contains(event->pos())) return; // clicked the letterboxed margin, not the image

    QPointF fraction((event->pos().x() - target.left()) / qreal(target.width()),
                      (event->pos().y() - target.top()) / qreal(target.height()));
    centerImagePoint_ = QPointF(fraction.x() * nativeSize.width(), fraction.y() * nativeSize.height());
    scale_ = oneToOneScale();
    fitMode_ = false;
    updateCursor();
    requestZoom(currentRow_);
    update();
}

void FullscreenViewer::toggleZoomKeyboard() {
    if (!fitMode_) {
        fitMode_ = true;
        updateCursor();
        update();
        return;
    }

    QSize nativeSize = nativeSizeForRow(currentRow_);
    if (nativeSize.width() <= 0 || nativeSize.height() <= 0) return; // no known size - can't compute a zoom target

    centerImagePoint_ = QPointF(nativeSize.width() / 2.0, nativeSize.height() / 2.0);
    scale_ = oneToOneScale();
    fitMode_ = false;
    updateCursor();
    requestZoom(currentRow_);
    update();
}

void FullscreenViewer::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) return;
    // The first click of the double-click already ran through mouseReleaseEvent (Qt
    // delivers press/release for the first click, then this event, then a second
    // release) and may have toggled zoom - harmless, since the window closes right
    // behind it and that transient state is never seen.
    close();
}

void FullscreenViewer::zoomAtCursor(const QPoint &cursorPos, int angleDeltaY) {
    if (!model_ || currentRow_ < 0 || angleDeltaY == 0) return;
    QSize nativeSize = nativeSizeForRow(currentRow_);
    if (nativeSize.width() <= 0 || nativeSize.height() <= 0) return; // no known size - nothing to zoom against

    qreal oldScale = effectiveScale(nativeSize);
    QPointF oldCenter = effectiveCenter(nativeSize);
    QPointF viewportCenter(width() / 2.0, height() / 2.0);

    // Image-space point currently under the cursor, at the current scale/center.
    QPointF cursorImagePoint = oldCenter + (QPointF(cursorPos) - viewportCenter) / oldScale;

    qreal notches = angleDeltaY / 120.0;
    qreal factor = std::pow(kScaleStepPerNotch, notches);
    qreal newScale = qBound(kMinScale, oldScale * factor, kMaxScale);

    // Re-center so that same image point stays under the cursor after rescaling -
    // standard zoom-to-cursor behavior.
    QPointF newCenter = cursorImagePoint - (QPointF(cursorPos) - viewportCenter) / newScale;

    scale_ = newScale;
    centerImagePoint_ = newCenter;
    fitMode_ = false;
    clampCenterImagePoint();
    updateCursor();
    requestZoom(currentRow_);
    update();
}

void FullscreenViewer::wheelEvent(QWheelEvent *event) {
    if (event->modifiers() & Qt::ControlModifier) {
        zoomAtCursor(event->position().toPoint(), event->angleDelta().y());
        event->accept();
        return;
    }

    // Plain scroll: browse, same as arrow keys. Scroll down/forward = next, matching
    // the "scrolling forward through a feed" convention.
    if (event->angleDelta().y() < 0) showRow(currentRow_ + 1, /*resetZoom=*/true);
    else if (event->angleDelta().y() > 0) showRow(currentRow_ - 1, /*resetZoom=*/true);
    event->accept();
}

void FullscreenViewer::keyPressEvent(QKeyEvent *event) {
    // Checked before the fixed-navigation switch below since these are all
    // user-configurable (see KeyBindings.h) and so can't just be case labels keyed
    // on event->key() the way fixed navigation is. Escape always closes regardless
    // of what ActivateFullscreen is currently bound to - guarantees there's always
    // a way out of the viewer even if that binding gets reassigned to something
    // unreachable from here.
    if (event->key() == Qt::Key_Escape) {
        close();
        return;
    }
    if (keybindings::matches(event, keybindings::binding(keybindings::Action::ActivateFullscreen))) {
        // Enter/Return (or whatever ActivateFullscreen is rebound to) is what opened
        // this (grid's activated() fires on it too, see MainWindow::
        // onGridItemActivated) - closing on it too makes it a toggle rather than an
        // open-only action.
        //
        // Standalone there was no grid to open from and so nothing to toggle back to;
        // the same key means "now show me the folder this is in" instead, which is the
        // one thing the mode deliberately hasn't loaded yet. Deliberately does not close
        // here - main.cpp closes this only once the main window is actually up, so the
        // last-window-closed quit can't fire in the gap between the two.
        if (standalone_) emit browseRequested(currentRow_);
        else close();
        return;
    }
    if (keybindings::matches(event, keybindings::binding(keybindings::Action::FullscreenToggleTrueFullscreen))) {
        trueFullscreen_ = !trueFullscreen_;
        prefs::settingsStore().setValue(kUseTrueFullscreenSettingsKey, trueFullscreen_);
        if (trueFullscreen_) showFullScreen(); else showMaximized();
        updateWindowTitle();
        return;
    }
    if (keybindings::matches(event, keybindings::binding(keybindings::Action::FullscreenToggleZoom))) {
        toggleZoomKeyboard();
        return;
    }
    if (keybindings::matches(event, keybindings::binding(keybindings::Action::FullscreenToggleInfoOverlay))) {
        // Only two states for now (off/on) rather than a real per-field EXIF cycle -
        // the app doesn't parse EXIF tags beyond orientation yet (see ExifInfo), so
        // there's no richer data to cycle through today. The overlay itself reuses
        // the same fields the main window's status bar shows.
        infoOverlayLevel_ = (infoOverlayLevel_ + 1) % 2;
        update();
        return;
    }

    switch (event->key()) {
        case Qt::Key_Left:
        case Qt::Key_Up:
            showRow(currentRow_ - 1, /*resetZoom=*/true);
            return;
        case Qt::Key_Right:
        case Qt::Key_Down:
        case Qt::Key_Space:
            showRow(currentRow_ + 1, /*resetZoom=*/true);
            return;
        default:
            QWidget::keyPressEvent(event);
    }
}

void FullscreenViewer::closeEvent(QCloseEvent *event) {
    decoder_->cancelPending();
    zoomDecoder_->cancelPending();
    zoomPrefetchTimer_->stop();
    if (parentWidget()) parentWidget()->activateWindow();
    QWidget::closeEvent(event);
}
