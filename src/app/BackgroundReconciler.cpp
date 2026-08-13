#include "BackgroundReconciler.h"

#include <QDebug>
#include <QTimer>

#include "Preferences.h"
#include "db/Database.h"
#include "scan/Indexer.h"
#include "util/AppPaths.h"
#include "util/ProcessId.h"

namespace {

// Deliberately gentle - this is background hygiene, not a race to finish. A directory
// every second and a half is plenty to eventually catch drift across even a very large
// library without ever being felt during interactive browsing.
constexpr int kPerDirectoryDelayMs = 1500;
// Once a full pass over every known directory completes, rest a while before starting
// the next one rather than immediately re-walking an already-fresh library.
constexpr int kFullCycleRestDelayMs = 10 * 60 * 1000;
} // namespace

BackgroundReconciler::BackgroundReconciler(QObject *parent) : QObject(parent) {
    moveToThread(&thread_);
    thread_.start(QThread::LowestPriority);
}

BackgroundReconciler::~BackgroundReconciler() {
    thread_.quit();
    thread_.wait();
}

void BackgroundReconciler::start() {
    QMetaObject::invokeMethod(this, &BackgroundReconciler::beginLoop, Qt::QueuedConnection);
}

void BackgroundReconciler::triggerFullSweepNow() {
    if (!timer_) return; // start() hasn't run its first beginLoop() yet - that already covers this
    loadDirList();
    // Jump the queue - same trick as RawRenderer::prioritize(): restarting an
    // already-running singleshot timer replaces whatever was left of its wait
    // (including a possible kFullCycleRestDelayMs rest period) with a fresh one.
    timer_->start(0);
}

void BackgroundReconciler::beginLoop() {
    if (!db_) db_ = std::make_unique<pixet::Database>(pixet::indexDbPath(), pixet::thumbsDbPath(), false);

    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, &BackgroundReconciler::sweepNext);

    loadDirList();
    timer_->start(kPerDirectoryDelayMs);
}

void BackgroundReconciler::loadDirList() {
    pending_.clear();
    cursor_ = 0;
    auto sel = db_->prepare("SELECT id, path FROM dirs ORDER BY id");
    while (sel.step()) {
        pending_.emplace_back(sel.columnInt64(0), sel.columnText(1));
    }
}

void BackgroundReconciler::sweepNext() {
    if (cursor_ >= pending_.size()) {
        // Finished a full pass - reload the list (picks up any directories discovered
        // since) and rest before going again.
        loadDirList();
        timer_->start(pending_.empty() ? kPerDirectoryDelayMs : kFullCycleRestDelayMs);
        return;
    }

    auto [dirId, dirPath] = pending_[cursor_++];
    (void)dirId;

    pixet::IndexOptions opts;
    opts.recursive = false;
    // Bypass the directory-mtime freshness shortcut - same as a manual Refresh. That
    // shortcut only catches files added/removed/renamed (it works off the directory's
    // own mtime); it deliberately misses a file edited in place under the same name,
    // which is exactly the drift this sweep exists to catch.
    opts.forceRescan = true;
    opts.forceRethumbnail = false; // only re-thumbnail files whose (mtime, size) actually changed
    opts.targetLongEdge = prefs::thumbnailTargetLongEdge();
    opts.owner = "gui:bg:pid:" + std::to_string(pixet::currentProcessId());
    // Deliberately not prefs::indexerThreadCount() - this sweep already describes
    // itself as idle/low-priority background work (see the class comment), and it
    // runs concurrently with FolderIndexer/RawRenderer on their own QThreads. Letting
    // it *also* claim a full pool of cores would mean up to 3x oversubscription when
    // they overlap, competing with the on-demand navigation the user is actually
    // waiting on for the cores that matter most.
    opts.threadCount = 1;

    pixet::Indexer indexer(*db_, opts);
    pixet::IndexStats stats;

    // Nothing may escape this slot. It runs on its own QThread with no handler anywhere
    // above it, so an exception here doesn't merely abandon the sweep - it reaches
    // std::terminate() and takes the entire application down with it. That is exactly what
    // happened in the field: a failed COMMIT deep inside Indexer killed pixet while the user
    // was doing nothing but browsing photos, brought down by background hygiene work nobody
    // had asked for. Indexer::run() now absorbs per-directory failures itself; this is the
    // outer backstop for the parts of it that sit outside that loop.
    bool failed = false;
    try {
        indexer.run(dirPath, stats, {});
    } catch (const std::exception &e) {
        failed = true;
        qWarning() << "pixet: background sweep failed on" << QString::fromStdString(dirPath) << "-" << e.what();
    }
    if (stats.dirsFailed > 0) {
        failed = true;
        // The message is the whole point of printing this: a crash report shows a throw site
        // but never the string, and without it there's no way to tell which SQLite error
        // actually occurred.
        qWarning() << "pixet: background sweep skipped" << stats.dirsFailed << "directory(ies) under"
                   << QString::fromStdString(dirPath) << "-" << QString::fromStdString(stats.firstFailure);
    }
    if (failed) {
        // Back off to the long delay rather than the 1.5s one. If the database is broken
        // rather than momentarily busy, the per-directory cadence would otherwise emit a
        // warning every second and a half indefinitely, and retrying a wedged DB that fast
        // helps nobody. A transient failure just costs one delayed sweep.
        timer_->start(kFullCycleRestDelayMs);
        return;
    }

    bool changed = stats.filesRemoved > 0 || stats.thumbsEmbedded > 0 || stats.thumbsDecoded > 0 ||
                   stats.thumbsUnsupported > 0 || stats.thumbsFailed > 0;
    // GPS backfill happens inside Indexer itself (see Indexer::backfillGps), so it runs for
    // every path that indexes a folder - this sweep, an on-demand navigation, and
    // pixet-index alike - rather than only for whatever this background pass happens to
    // reach. Without that, a folder's geotag markers wouldn't appear until the sweep got to
    // it, which on a large library is many minutes after opening it.
    if (stats.gpsBackfilled > 0) changed = true;
    if (changed) emit directoryChanged(QString::fromStdString(dirPath));

    timer_->start(kPerDirectoryDelayMs);
}

