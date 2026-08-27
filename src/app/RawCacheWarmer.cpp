#include "RawCacheWarmer.h"

#include <QMetaObject>

#include "cache/RawCache.h"
#include "util/FileMove.h"

RawCacheWarmer::RawCacheWarmer(QObject *parent) : QObject(parent) {
    qRegisterMetaType<QStringList>("QStringList");
    moveToThread(&thread_);
    // Idle priority on purpose. This is speculative work - the user may never open any of
    // these - so it must never take a core away from the thumbnail decoding that is filling
    // the grid they are actually looking at.
    thread_.start(QThread::IdlePriority);
}

RawCacheWarmer::~RawCacheWarmer() {
    thread_.quit();
    thread_.wait();
}

void RawCacheWarmer::warm(const QStringList &filePaths) {
    // Stamped here, on the UI thread, rather than inside the slot: the point is for the
    // in-flight run to notice it has been superseded, and a generation bumped only once the
    // worker got around to the new request would be bumped too late to do that.
    const qint64 gen = ++generation_;
    QMetaObject::invokeMethod(this, "doWarm", Qt::QueuedConnection, Q_ARG(QStringList, filePaths),
                              Q_ARG(qint64, gen));
}

void RawCacheWarmer::doWarm(QStringList filePaths, qint64 generation) {
    for (const QString &path : filePaths) {
        if (generation != generation_.load(std::memory_order_relaxed)) return; // viewport moved on
        const std::string utf8 = path.toStdString();
        int64_t size = 0, mtime = 0;
        // The same (mtime, size) identity the cache is keyed by. A file that has since been
        // edited or removed simply won't match anything, which is the correct outcome here -
        // there is nothing to warm.
        if (!pixet::statFile(utf8, &size, &mtime)) continue;
        pixet::rawcache::prewarm(utf8, mtime, size);
    }
}
