#include "FullscreenDecoder.h"

#include <QMetaObject>

#include "QtInterop.h"
#include "ThreadShutdown.h"
#include "db/Schema.h"
#include "decode/DisplayCodec.h"
#include "util/Profile.h"

FullscreenDecoder::FullscreenDecoder(QObject *parent) : QObject(parent) {
    moveToThread(&thread_);
    thread_.start();
}

FullscreenDecoder::~FullscreenDecoder() {
    // Bounded, and able to take the process down rather than hang it if this worker is
    // stuck inside a long decode - see ThreadShutdown.h for the failure this replaces.
    threadshutdown::stopWorker(thread_, "FullscreenDecoder");
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
    pixet::RgbImage img;
    bool ok;
    {
        PIXET_PROF_SCOPE("fullscreen.decodeForDisplay");
        ok = pixet::decodeForDisplay(req.filePath.toStdString(), (pixet::Format)req.fmt, req.targetLongEdge, img);
    }
    if (ok) {
        PIXET_PROF_SCOPE("fullscreen.decode.toQImage");
        result = rgbImageToQImage(img);
    }

    emit decoded(req.requestId, result);

    if (!stack_.isEmpty()) {
        QMetaObject::invokeMethod(this, "processOne", Qt::QueuedConnection);
    } else {
        processing_ = false;
    }
}
