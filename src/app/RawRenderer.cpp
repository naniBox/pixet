#include "RawRenderer.h"

#include <Windows.h>

#include <QTimer>

#include "db/Database.h"
#include "db/Schema.h"
#include "scan/Indexer.h"
#include "util/AppPaths.h"
#include "util/StringUtil.h"

namespace {
// Gentler than BackgroundReconciler's already-gentle per-directory pacing - a full RAW
// demosaic decode is genuinely expensive (unlike a cheap mtime stat check), so this
// leaves even more room between directories. A directory can hold several pending RAW
// files, all rendered in the same visit (Indexer's own Pass B batching), so this is
// still a meaningfully steady trickle even at one directory per few seconds.
constexpr int kPerDirectoryDelayMs = 4000;
// Nothing pending right now - wait a while before checking again rather than hammering
// the DB with an empty query in a tight loop.
constexpr int kIdleRetryDelayMs = 60 * 1000;
} // namespace

RawRenderer::RawRenderer(QObject *parent) : QObject(parent) {
    moveToThread(&thread_);
    thread_.start(QThread::LowestPriority);
}

RawRenderer::~RawRenderer() {
    thread_.quit();
    thread_.wait();
}

void RawRenderer::start() {
    QMetaObject::invokeMethod(this, &RawRenderer::renderNext, Qt::QueuedConnection);
}

void RawRenderer::prioritize(QString path) {
    priorityDir_ = path;
    // Jump the queue - restarting an already-running QTimer replaces however much of
    // its current wait was left with a fresh (here, 0ms) one, rather than piling up a
    // second pending call alongside it.
    if (timer_) timer_->start(0);
}

void RawRenderer::renderNext() {
    if (!db_) db_ = std::make_unique<pixet::Database>(pixet::indexDbPath(), pixet::thumbsDbPath(), false);

    if (!timer_) {
        timer_ = new QTimer(this);
        timer_->setSingleShot(true);
        connect(timer_, &QTimer::timeout, this, &RawRenderer::renderNext);
    }

    int64_t dirId = 0;
    std::wstring dirPath;

    if (!priorityDir_.isEmpty()) {
        auto sel = db_->prepare("SELECT dirs.id, dirs.path FROM files JOIN dirs ON files.dir_id = dirs.id "
                                 "WHERE files.state=? AND dirs.path=? LIMIT 1");
        sel.bind(1, (int64_t)pixet::FileState::DoneNeedsRender);
        sel.bind(2, pixet::toUtf8(priorityDir_.toStdWString()));
        if (sel.step()) {
            dirId = sel.columnInt64(0);
            dirPath = pixet::toUtf16(sel.columnText(1));
        } else {
            priorityDir_.clear(); // nothing left there - stop favoring it, resume the normal rotation below
        }
    }

    if (dirId == 0) {
        // Any one directory that currently has at least one RAW file still on its
        // embedded-preview thumbnail - processed a whole directory at a time (Indexer's
        // own Pass B batching picks up every such file in it at once), not this file
        // specifically.
        auto sel = db_->prepare("SELECT dirs.id, dirs.path FROM files JOIN dirs ON files.dir_id = dirs.id "
                                 "WHERE files.state=? LIMIT 1");
        sel.bind(1, (int64_t)pixet::FileState::DoneNeedsRender);
        if (sel.step()) {
            dirId = sel.columnInt64(0);
            dirPath = pixet::toUtf16(sel.columnText(1));
        }
    }

    if (dirId == 0) {
        timer_->start(kIdleRetryDelayMs);
        return;
    }

    pixet::IndexOptions opts;
    opts.recursive = false;
    opts.renderRaws = true;
    // Distinct owner id from both the on-demand FolderIndexer ("gui:pid:...") and
    // BackgroundReconciler ("gui:bg:pid:...") - if either is actively working this
    // same directory right now, Indexer's own claim check just skips it for this
    // round rather than contending; the next round tries again.
    opts.owner = "gui:rawrender:pid:" + std::to_string(GetCurrentProcessId());

    pixet::Indexer indexer(*db_, opts);
    pixet::IndexStats stats;

    // A forced full RAW render is slow enough that a caller waiting for the *whole*
    // directory to finish before hearing anything would defeat the point of wanting to
    // watch progress on a large RAW folder rather than wait for it in one lump -
    // Indexer flushes progress per file during a renderRaws pass specifically (see
    // Indexer.cpp) rather than its usual batch-of-64, so this fires once per file.
    QString dirPathQt = QString::fromStdWString(dirPath);
    pixet::IndexCallbacks callbacks;
    callbacks.onProgress = [this, &dirPathQt](const pixet::IndexStats &) { emit directoryChanged(dirPathQt); };

    indexer.run(dirPath, stats, callbacks);

    timer_->start(kPerDirectoryDelayMs);
}
