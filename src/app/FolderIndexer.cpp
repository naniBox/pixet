#include "FolderIndexer.h"

#include "db/Database.h"
#include "scan/Indexer.h"
#include "util/AppPaths.h"
#include "util/ProcessId.h"

FolderIndexer::FolderIndexer(QObject *parent) : QObject(parent) {
    moveToThread(&thread_);
    thread_.start();
}

FolderIndexer::~FolderIndexer() {
    thread_.quit();
    thread_.wait();
}

void FolderIndexer::indexFolder(QString path, bool force, bool forceRethumbnail) {
    emit started(path);

    if (!db_) db_ = std::make_unique<pixet::Database>(pixet::indexDbPath(), pixet::thumbsDbPath(), false);

    pixet::IndexOptions opts;
    opts.recursive = false;
    opts.forceRescan = force;
    opts.forceRethumbnail = forceRethumbnail;
    opts.owner = "gui:pid:" + std::to_string(pixet::currentProcessId());

    pixet::Indexer indexer(*db_, opts);
    pixet::IndexStats stats;

    pixet::IndexCallbacks callbacks;
    callbacks.onFilesListed = [this, path](int64_t, const std::string &) { emit filesListed(path); };
    callbacks.onProgress = [this, path](const pixet::IndexStats &) { emit thumbsProgress(path); };

    indexer.run(path.toStdString(), stats, callbacks);

    emit finished(path);
}
