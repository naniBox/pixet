#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <QThread>
#include <QVector>

// Decodes original image files off the UI thread for the fullscreen viewer.
// PreviewDecoder (the side-panel's decoder) is "latest request wins" - right for a
// single preview slot that should cancel-on-supersede, wrong here: the fullscreen
// viewer needs several different requests serviced at once (ring-buffer neighbor
// prefetch across multiple rows, plus an on-demand full-resolution zoom decode for
// whichever row is current). Modeled on ThumbLoader's LIFO queue instead, decoding
// via the same file-based path PreviewDecoder uses rather than ThumbLoader's
// cached-blob path.
class FullscreenDecoder : public QObject {
    Q_OBJECT

public:
    explicit FullscreenDecoder(QObject *parent = nullptr);
    ~FullscreenDecoder() override;

public slots:
    // requestId is caller-assigned and returned verbatim in decoded() - matching it
    // back to what it was for (which row, fit vs. zoom) and ignoring anything
    // superseded by the time it arrives is the caller's job. targetLongEdge <= 0
    // decodes at full native resolution (no scaled-DCT downscale) - see
    // JpegCodec::decodeJpeg.
    void request(qint64 requestId, QString filePath, int fmt, int targetLongEdge);
    // Drops every not-yet-started queued request (e.g. the viewer closing, or a
    // prefetch neighborhood being abandoned after jumping several images at once).
    // Anything already mid-decode still finishes and emits normally - the caller
    // ignores a requestId it no longer cares about.
    void cancelPending();

signals:
    void decoded(qint64 requestId, QImage image);

private slots:
    void processOne();

private:
    struct Req {
        qint64 requestId;
        QString filePath;
        int fmt;
        int targetLongEdge;
    };

    QThread thread_;
    QVector<Req> stack_;
    bool processing_ = false;
};
