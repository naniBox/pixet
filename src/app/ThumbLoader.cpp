#include "ThumbLoader.h"

#include <QMetaObject>

#include "Preferences.h"
#include "QtInterop.h"
#include "db/Database.h"
#include "decode/JpegCodec.h"
#include "util/AppPaths.h"

ThumbLoader::ThumbLoader(QObject *parent) : QObject(parent) {
    moveToThread(&thread_);
    thread_.start();
}

ThumbLoader::~ThumbLoader() {
    thread_.quit();
    thread_.wait();
}

void ThumbLoader::setDevicePixelRatio(qreal ratio) {
    devicePixelRatio_.store(ratio > 0.0 ? ratio : 1.0, std::memory_order_relaxed);
}

void ThumbLoader::request(qint64 fileId, qint64 thumbId) {
    if (thumbId == 0 || pending_.contains(fileId)) return;
    pending_.insert(fileId);
    stack_.push_back({fileId, thumbId});
    if (!processing_) {
        processing_ = true;
        QMetaObject::invokeMethod(this, "processOne", Qt::QueuedConnection);
    }
}

void ThumbLoader::processOne() {
    if (stack_.isEmpty()) {
        processing_ = false;
        return;
    }
    Req req = stack_.takeLast();
    pending_.remove(req.fileId);

    if (!db_) db_ = std::make_unique<pixet::Database>(pixet::indexDbPath(), pixet::thumbsDbPath(), true);

    QPixmap pixmap;
    auto sel = db_->prepare("SELECT bytes FROM thumbs.thumbs WHERE id=?");
    sel.bind(1, req.thumbId);
    if (sel.step()) {
        std::vector<uint8_t> bytes = sel.columnBlob(0);
        pixet::RgbImage img;
        const qreal dpr = devicePixelRatio_.load(std::memory_order_relaxed);
        // Device pixels, not logical points. prefs::thumbnailIconSize() is the on-screen
        // (logical) cell size, so on a Retina display the grid can show twice that many real
        // pixels - and prefs::thumbnailTargetLongEdge() already stores blobs at 2x the icon
        // size precisely so this headroom exists to be used.
        const int deviceSize = qMax(1, qRound(prefs::thumbnailIconSize() * dpr));
        // Stored thumbs are generated at prefs::thumbnailTargetLongEdge() (always >=
        // this); decode straight to display size (cheap scaled-DCT path) rather than
        // decoding full-size and scaling after.
        if (pixet::decodeJpeg(bytes.data(), bytes.size(), deviceSize, img)) {
            pixmap = QPixmap::fromImage(rgbImageToQImage(img));
            // decodeJpeg only lands *close* to the target via coarse DCT scale steps -
            // the grid needs an exact fit or oversized decorations bleed into
            // neighboring cells.
            if (pixmap.width() > deviceSize || pixmap.height() > deviceSize) {
                pixmap = pixmap.scaled(deviceSize, deviceSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
            // Stamping the ratio is what keeps the grid's geometry in logical units: the
            // pixmap is now physically 2x on Retina, and ThumbGridView centres it using
            // deviceIndependentSize() so the cell layout is unchanged.
            pixmap.setDevicePixelRatio(dpr);
        }
    }

    emit thumbReady(req.fileId, pixmap);

    if (!stack_.isEmpty()) {
        QMetaObject::invokeMethod(this, "processOne", Qt::QueuedConnection);
    } else {
        processing_ = false;
    }
}
