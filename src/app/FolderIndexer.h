#pragma once

#include <QObject>
#include <QThread>

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
};
