#include "PreviewDecoder.h"

#include <QMetaObject>

#include "QtInterop.h"
#include "ThreadShutdown.h"
#include "db/Schema.h"
#include "decode/DisplayCodec.h"
#include "util/Profile.h"

PreviewDecoder::PreviewDecoder(QObject *parent) : QObject(parent) {
    moveToThread(&thread_);
    thread_.start();
}

PreviewDecoder::~PreviewDecoder() {
    // Bounded, and able to take the process down rather than hang it if this worker is
    // stuck inside a long decode - see ThreadShutdown.h for the failure this replaces.
    threadshutdown::stopWorker(thread_, "PreviewDecoder");
}

void PreviewDecoder::requestPreview(qint64 requestId, const QString &filePath, int fmt, int targetLongEdge) {
    latestRequestId_.store(requestId); // stamped synchronously, on the caller's (UI) thread
    QMetaObject::invokeMethod(this, "doDecode", Qt::QueuedConnection, Q_ARG(qint64, requestId),
                               Q_ARG(QString, filePath), Q_ARG(int, fmt), Q_ARG(int, targetLongEdge));
}

void PreviewDecoder::doDecode(qint64 requestId, QString filePath, int fmt, int targetLongEdge) {
    if (requestId != latestRequestId_.load()) return; // already superseded, don't bother

    QImage result;
    pixet::RgbImage img;
    bool ok;
    {
        PIXET_PROF_SCOPE("preview.decodeForDisplay");
        ok = pixet::decodeForDisplay(filePath.toStdString(), (pixet::Format)fmt, targetLongEdge, img);
    }
    if (ok) {
        PIXET_PROF_SCOPE("preview.decode.toQImage");
        result = rgbImageToQImage(img);
    }

    if (requestId != latestRequestId_.load()) return; // superseded while we were decoding
    emit previewReady(requestId, result);
}
