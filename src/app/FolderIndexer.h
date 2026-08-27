#pragma once

#include <QMutex>
#include <QObject>
#include <QString>
#include <QThread>
#include <QVector>

#include <atomic>
#include <memory>

namespace pixet {
class Database;
}

// GUI-facing wrapper around pixet_core's Indexer, run non-recursively (one folder,
// not a whole tree) off the UI thread every time the user navigates. This is what
// makes "no mandatory pre-index step" real: browsing a folder is what indexes it.
// Reuses the exact same Pass A/B code as the pixet-index CLI.
class FolderIndexer : public QObject {
    Q_OBJECT

public:
    explicit FolderIndexer(QObject *parent = nullptr);
    ~FolderIndexer() override;

    // Tells the indexer which files in `folderPath` to thumbnail first - the ones the
    // grid currently has on screen, plus a screenful either side. Pass B otherwise works
    // in readdir order, which has nothing to do with the order the grid shows files in,
    // so on a cold folder the user watches ~18 visible cells stay empty while 1280
    // thumbnails are generated somewhere else in the list. See
    // pixet::IndexCallbacks::onWantFirst for what the core does with this.
    //
    // Thread-safe, and deliberately a plain method rather than a slot: the UI thread
    // calls it while the worker thread is *inside* indexFolder(), so a queued connection
    // would sit in the worker's event queue until the whole folder had finished indexing
    // - which is exactly when the answer stops mattering. A mutex-guarded assignment is
    // what makes the hint visible to work already in progress.
    //
    // The folder is carried along and checked at read time so a hint set just before the
    // user navigated away can't reorder the next folder's work by stale file ids.
    // Passing a folder that isn't being indexed is harmless; the hint is simply ignored.
    void setPriorityFiles(const QString &folderPath, QVector<qint64> fileIds);

    // Abandons whatever folder is being indexed right now, as soon as the current wave of
    // thumbnails finishes. Call this when the user navigates somewhere else.
    //
    // Without it the grid appears to hang on the *new* folder: indexFolder() is a queued
    // slot on one worker thread, so a second call sits in that thread's event queue until
    // the first returns. A folder of 335 RAWs therefore blocks a folder of two JPEGs for as
    // long as the RAWs take, and the two JPEGs are what the user is actually looking at.
    //
    // Thread-safe and a plain method for the same reason setPriorityFiles() is: the worker
    // is *inside* indexFolder() when this is called, so a queued connection would only be
    // delivered once the thing it was meant to interrupt had already finished.
    //
    // Nothing is lost. The cancelled folder keeps whatever thumbnails it committed and its
    // remaining files stay state=New, which is exactly where an un-browsed folder sits
    // anyway - navigating back re-indexes only what's left.
    void cancelCurrent();

public slots:
    // force=true is the explicit "Refresh" action - bypasses the mtime freshness
    // shortcut and re-verifies every file in the folder (but only re-thumbnails
    // files whose mtime/size actually changed). forceRethumbnail=true goes further -
    // regenerates every thumbnail in the folder unconditionally (the "Force
    // Re-thumbnail" context menu action); implies force.
    void indexFolder(QString path, bool force, bool forceRethumbnail = false);

signals:
    void started(QString path);
    // Pass A just committed for this folder - the file list (names) is final, so a
    // full grid reload here will show correct filenames even though most thumbnails
    // are still pending. Fired even when nothing needed rescanning (already fresh),
    // so the receiver doesn't need to special-case that.
    void filesListed(QString path);
    // A Pass B batch just committed - some thumbnails are newly ready. Fired more
    // often than filesListed; the receiver should do an incremental update; a full
    // model reset here would wipe already-displayed thumbnails and cause flicker.
    void thumbsProgress(QString path);
    // Indexing this folder hit a database error and gave up on some or all of it. Separate
    // from finished(), which still fires afterwards - the folder is done either way, just
    // incompletely. Worth surfacing rather than swallowing: the user asked for this folder
    // and is looking at a grid that may be missing thumbnails, with nothing on screen
    // distinguishing that from "still working".
    void indexFailed(QString path, QString message);
    void finished(QString path);

private:
    QThread thread_;
    std::unique_ptr<pixet::Database> db_; // created lazily, on the worker thread

    // Written by the UI thread (setPriorityFiles), read by the worker thread from inside
    // Pass B - see setPriorityFiles() on why this is a mutex rather than a queued signal.
    QMutex priorityMutex_;
    QString priorityPath_;
    QVector<qint64> priorityIds_;

    // Set by the UI thread (cancelCurrent), polled by the worker from inside the run it is
    // cancelling. Cleared at the start of every indexFolder() so a cancellation can only
    // ever apply to the run it was aimed at, never to the next one - the race that would
    // otherwise leave a folder half-indexed for no reason the user could see.
    std::atomic<bool> cancelRequested_{false};
};
