#include "BackgroundReconciler.h"

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

    pixet::Indexer indexer(*db_, opts);
    pixet::IndexStats stats;
    indexer.run(dirPath, stats, {});

    bool changed = stats.filesRemoved > 0 || stats.thumbsEmbedded > 0 || stats.thumbsDecoded > 0 ||
                   stats.thumbsUnsupported > 0 || stats.thumbsFailed > 0;
    if (changed) emit directoryChanged(QString::fromStdString(dirPath));

    timer_->start(kPerDirectoryDelayMs);
}
