#include "FullscreenDecoder.h"

#include <QMetaObject>

#include "QtInterop.h"
#include "db/Schema.h"
#include "decode/JpegCodec.h"
#include "meta/JpegExif.h"
#include "util/FileIO.h"

FullscreenDecoder::FullscreenDecoder(QObject *parent) : QObject(parent) {
    moveToThread(&thread_);
    thread_.start();
}

FullscreenDecoder::~FullscreenDecoder() {
    thread_.quit();
    thread_.wait();
}

void FullscreenDecoder::request(qint64 requestId, QString filePath, int fmt, int targetLongEdge) {
    stack_.push_back({requestId, std::move(filePath), fmt, targetLongEdge});
    if (!processing_) {
        processing_ = true;
        QMetaObject::invokeMethod(this, "processOne", Qt::QueuedConnection);
    }
}

void FullscreenDecoder::cancelPending() { stack_.clear(); }

void FullscreenDecoder::processOne() {
    if (stack_.isEmpty()) {
        processing_ = false;
        return;
    }
    Req req = stack_.takeLast(); // LIFO - most recently requested (nearest to the user) wins ties

    QImage result;
    if ((pixet::Format)req.fmt == pixet::Format::Jpeg) {
        std::vector<uint8_t> fileBytes;
        if (pixet::readWholeFile(req.filePath.toStdWString(), fileBytes)) {
            pixet::ExifInfo exif = pixet::parseJpegExif(fileBytes.data(), fileBytes.size());
            pixet::RgbImage img;
            if (pixet::decodeJpeg(fileBytes.data(), fileBytes.size(), req.targetLongEdge, img)) {
                pixet::applyOrientation(img, exif.orientation);
                result = rgbImageToQImage(img);
            }
        }
    }

    emit decoded(req.requestId, result);

    if (!stack_.isEmpty()) {
        QMetaObject::invokeMethod(this, "processOne", Qt::QueuedConnection);
    } else {
        processing_ = false;
    }
}
