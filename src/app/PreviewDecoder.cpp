#include "PreviewDecoder.h"

#include <QMetaObject>

#include "QtInterop.h"
#include "db/Schema.h"
#include "decode/JpegCodec.h"
#include "meta/JpegExif.h"
#include "util/FileIO.h"
#include "util/StringUtil.h"

PreviewDecoder::PreviewDecoder(QObject *parent) : QObject(parent) {
    moveToThread(&thread_);
    thread_.start();
}

PreviewDecoder::~PreviewDecoder() {
    thread_.quit();
    thread_.wait();
}

void PreviewDecoder::requestPreview(qint64 requestId, const QString &filePath, int fmt, int targetLongEdge) {
    latestRequestId_.store(requestId); // stamped synchronously, on the caller's (UI) thread
    QMetaObject::invokeMethod(this, "doDecode", Qt::QueuedConnection, Q_ARG(qint64, requestId),
                               Q_ARG(QString, filePath), Q_ARG(int, fmt), Q_ARG(int, targetLongEdge));
}

void PreviewDecoder::doDecode(qint64 requestId, QString filePath, int fmt, int targetLongEdge) {
    if (requestId != latestRequestId_.load()) return; // already superseded, don't bother

    QImage result;
    if ((pixet::Format)fmt == pixet::Format::Jpeg) {
        std::vector<uint8_t> fileBytes;
        if (pixet::readWholeFile(filePath.toStdWString(), fileBytes)) {
            pixet::ExifInfo exif = pixet::parseJpegExif(fileBytes.data(), fileBytes.size());
            pixet::RgbImage img;
            if (pixet::decodeJpeg(fileBytes.data(), fileBytes.size(), targetLongEdge, img)) {
                pixet::applyOrientation(img, exif.orientation);
                result = rgbImageToQImage(img);
            }
        }
    }

    if (requestId != latestRequestId_.load()) return; // superseded while we were decoding
    emit previewReady(requestId, result);
}
