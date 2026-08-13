#pragma once

#include <QImage>
#include <QObject>
#include <QPixmap>
#include <QSet>
#include <QThread>
#include <QVector>

#include <atomic>

#include "util/ThreadPool.h"

namespace pixet {
class Database;
}

// Decodes cached thumbnail blobs off the UI thread, across a small pool of worker
// threads rather than one at a time (see kMaxConcurrentDecodes) - added as part of a
// scalability pass (see devlog) once measurement showed the per-item work (a DB blob
// read + JPEG decode + scale) was real, non-trivial cost that a big-enough on-screen
// burst could stack up serially.
//
// Requests are still a LIFO stack (deduped by file id), same intent as before
// parallelizing: the most recently requested cell - typically wherever the user just
// scrolled to - gets served first. Up to kMaxConcurrentDecodes can now be *in flight*
// at once instead of strictly one, but which one goes next is unchanged.
class ThumbLoader : public QObject {
    Q_OBJECT

public:
    explicit ThumbLoader(QObject *parent = nullptr);
    ~ThumbLoader() override;

    // Screen pixel ratio to decode for. Must be set from the UI thread (the worker can't ask
    // a widget or QScreen anything safely), and is read atomically by the decode thread(s).
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

private:
    struct Req {
        qint64 fileId;
        qint64 thumbId;
    };

    // Deliberately small and fixed, not prefs::indexerThreadCount()/
    // hardware_concurrency(): the burst ThumbLoader ever needs to serve at once is
    // bounded by how many grid cells are visible on screen at a time, typically well
    // under this regardless of how many cores the machine has - and running at full
    // core count here would risk oversubscribing alongside Indexer's own pool (see
    // Indexer.h) when a folder is both being freshly indexed *and* displayed at once.
    static constexpr size_t kMaxConcurrentDecodes = 4;

    QThread thread_;
    pixet::ThreadPool pool_{kMaxConcurrentDecodes};
    QVector<Req> stack_;
    QSet<qint64> pending_;
    int inFlight_ = 0;
    std::atomic<qreal> devicePixelRatio_{1.0}; // see setDevicePixelRatio()

    // Pops the next (most recently requested) item off stack_ and dispatches it to
    // pool_, as long as inFlight_ hasn't already reached kMaxConcurrentDecodes.
    // Called from request() and from onDecodeFinished(), so a slot freed by one
    // completing decode immediately picks up whatever's next.
    void dispatchNext();
    // Runs on ThumbLoader's own thread - hopped back via a queued
    // QMetaObject::invokeMethod call from whichever pool worker thread actually did
    // the decode (see the .cpp) - so this is the only place stack_/pending_/
    // inFlight_ are ever touched, and none of them need a mutex despite the decode
    // work itself running concurrently on pool_. Also the only place a QPixmap is
    // ever constructed (from the QImage the pool worker decoded) - see decodeThumb()'s
    // comment in the .cpp for why that has to stay true.
    void onDecodeFinished(qint64 fileId, QImage image, qreal dpr);
};
