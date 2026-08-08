#include "ThumbLoader.h"

#include <QMetaObject>

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
        // Stored thumbs are up to 320px; decode straight to display size (cheap
        // scaled-DCT path) rather than decoding full-size and scaling after.
        if (pixet::decodeJpeg(bytes.data(), bytes.size(), kThumbIconSize, img)) {
            pixmap = QPixmap::fromImage(rgbImageToQImage(img));
            // decodeJpeg only lands *close* to the target via coarse DCT scale steps -
            // the grid needs an exact fit or oversized decorations bleed into
            // neighboring cells.
            if (pixmap.width() > kThumbIconSize || pixmap.height() > kThumbIconSize) {
                pixmap = pixmap.scaled(kThumbIconSize, kThumbIconSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
        }
    }

    emit thumbReady(req.fileId, pixmap);

    if (!stack_.isEmpty()) {
        QMetaObject::invokeMethod(this, "processOne", Qt::QueuedConnection);
    } else {
        processing_ = false;
    }
}
