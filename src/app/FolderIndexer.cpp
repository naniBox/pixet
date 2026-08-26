#include "FolderIndexer.h"

#include <QMutexLocker>

#include "Preferences.h"
#include "db/Database.h"
#include "scan/Indexer.h"
#include "util/AppPaths.h"
#include "util/ProcessId.h"
#include "util/Profile.h"

FolderIndexer::FolderIndexer(QObject *parent) : QObject(parent) {
    moveToThread(&thread_);
    thread_.start();
}

FolderIndexer::~FolderIndexer() {
    thread_.quit();
    thread_.wait();
}

void FolderIndexer::setPriorityFiles(const QString &folderPath, QVector<qint64> fileIds) {
    QMutexLocker lock(&priorityMutex_);
    priorityPath_ = folderPath;
    priorityIds_ = std::move(fileIds);
}

void FolderIndexer::indexFolder(QString path, bool force, bool forceRethumbnail) {
    emit started(path);

    if (!db_) db_ = std::make_unique<pixet::Database>(pixet::indexDbPath(), pixet::thumbsDbPath(), false);

    pixet::IndexOptions opts;
    opts.recursive = false;
    opts.forceRescan = force;
    opts.forceRethumbnail = forceRethumbnail;
    opts.targetLongEdge = prefs::thumbnailTargetLongEdge();
    opts.owner = "gui:pid:" + std::to_string(pixet::currentProcessId());
    // On-demand, triggered by the user navigating somewhere and waiting on it - gets
    // full parallelism (0 = auto-detect, see prefs::indexerThreadCount()). Unlike
    // BackgroundReconciler/RawRenderer, which stay pinned to threadCount=1 - see their
    // own comments on why.
    opts.threadCount = prefs::indexerThreadCount();

    pixet::Indexer indexer(*db_, opts);
    pixet::IndexStats stats;

    pixet::IndexCallbacks callbacks;
    callbacks.onFilesListed = [this, path](int64_t, const std::string &) { emit filesListed(path); };
    callbacks.onProgress = [this, path](const pixet::IndexStats &) { emit thumbsProgress(path); };
    // Called on this worker thread once per Pass B wave. `remaining` is ignored: the
    // grid's answer is "these are on screen", and the core already drops whatever isn't
    // still pending, so there's nothing to intersect here. opts.recursive is false, so
    // dirPath is always `path` - the folder check that matters is against the hint's own
    // folder, which may be one the user has since navigated away from.
    callbacks.onWantFirst = [this, path](int64_t, const std::string &,
                                          const std::vector<int64_t> &) -> std::vector<int64_t> {
        QMutexLocker lock(&priorityMutex_);
        if (priorityPath_ != path) return {};
        return std::vector<int64_t>(priorityIds_.begin(), priorityIds_.end());
    };

    // Guarded for the same reason as BackgroundReconciler::sweepNext() - an exception on this
    // QThread has no handler above it and reaches std::terminate() - plus one specific to
    // this path: finished() must be emitted no matter what. MainWindow::onIndexerStarted()
    // posts "Indexing <folder>..." with no timeout and relies solely on onIndexerFinished()
    // to clear it, so an early return would leave that message up for the rest of the
    // session, over a grid that had quietly stopped filling in.
    try {
        PIXET_PROF_SCOPE("folderIndexer.run");
        indexer.run(path.toStdString(), stats, callbacks);
    } catch (const std::exception &e) {
        emit indexFailed(path, QString::fromUtf8(e.what()));
    }
    if (stats.dirsFailed > 0) emit indexFailed(path, QString::fromStdString(stats.firstFailure));

    emit finished(path);
}
