#pragma once

#include <QObject>
#include <QPixmap>
#include <QSet>
#include <QThread>
#include <QVector>

#include <atomic>
#include <memory>

namespace pixet {
class Database;
}

// Decodes cached thumbnail blobs off the UI thread. Requests are a LIFO stack
// (deduped by file id) so the most recently requested cell - typically wherever the
// user just scrolled to - is served first, per the plan's "distance from viewport,
// newest wins" intent. No explicit cancellation of off-screen requests in P2: the
// grid only re-requests currently-painted cells, so stale entries just stop being
// re-issued and drain naturally; revisit if scrolling perf ever demands it.
class ThumbLoader : public QObject {
    Q_OBJECT

public:
    explicit ThumbLoader(QObject *parent = nullptr);
    ~ThumbLoader() override;

    // Screen pixel ratio to decode for. Must be set from the UI thread (the worker can't ask
    // a widget or QScreen anything safely), and is read atomically by the decode thread.
    //
    // Without it thumbnails decode to the icon size in *logical* points and the compositor
    // doubles them on a Retina display, which is the difference between a crisp grid and a
    // soft one. Known limitation: a mid-session move to a display with a different ratio
    // isn't tracked - already-decoded pixmaps keep the old ratio until something reloads the
    // folder. Not worth watching screenChanged for, given the grid re-requests on any
    // icon-size change anyway.
    void setDevicePixelRatio(qreal ratio);

public slots:
    void request(qint64 fileId, qint64 thumbId);

signals:
    void thumbReady(qint64 fileId, QPixmap pixmap);

private slots:
    void processOne();

private:
    struct Req {
        qint64 fileId;
        qint64 thumbId;
    };

    QThread thread_;
    std::unique_ptr<pixet::Database> db_; // created lazily, on the worker thread
    QVector<Req> stack_;
    QSet<qint64> pending_;
    bool processing_ = false;
    std::atomic<qreal> devicePixelRatio_{1.0}; // see setDevicePixelRatio()
};
