#pragma once

#include <QImage>
#include <QObject>
#include <QThread>

#include <atomic>

// Decodes the *original* file at preview resolution (not an upscale of the cached
// thumbnail) off the UI thread. Cancel-on-supersede: `latestRequestId_` is stamped
// synchronously on the calling (UI) thread the instant a new request comes in, so an
// in-flight decode for a since-superseded selection can bail early - both before
// starting the (possibly slow) decode and right after, before emitting.
class PreviewDecoder : public QObject {
    Q_OBJECT

public:
    explicit PreviewDecoder(QObject *parent = nullptr);
    ~PreviewDecoder() override;

    // Call from the UI thread. Stamps the request id immediately, then queues the
    // actual decode onto the worker thread.
    void requestPreview(qint64 requestId, const QString &filePath, int fmt, int targetLongEdge);

signals:
    void previewReady(qint64 requestId, QImage image);

private slots:
    void doDecode(qint64 requestId, QString filePath, int fmt, int targetLongEdge);

private:
    QThread thread_;
    std::atomic<qint64> latestRequestId_{0};
};
