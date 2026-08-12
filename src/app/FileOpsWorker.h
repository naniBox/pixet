#pragma once

#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>

#include <atomic>
#include <memory>

namespace pixet {
class Database;
}

// Off-UI-thread wrapper around pixet::fileops::execute() (src/core/fileops/FileOps.h) -
// this is the app's first code path that can move/copy/overwrite a real file, so like
// every other background worker here (FolderIndexer, ThumbLoader, ...) it owns its own
// QThread and a lazily-constructed Database connection, driven entirely by queued
// signals and never touched from the UI thread.
//
// Two-stage protocol per request, split so a collision-resolution dialog can run on the
// UI thread with zero filesystem work happening underneath it, and so the actual I/O
// (execute()) never has to pause mid-batch waiting on user interaction:
//   1. preflight(Request) - stats every source and the destination off-thread; reports
//      back what got rejected (folders, missing sources) and which destination names
//      collide (Item::hasConflict), so the caller can show one collision dialog per
//      conflict (or none at all) before anything is touched.
//   2. execute(Request)   - `req` now carries a fully-resolved Item::resolution for
//      every conflicting item - no further user interaction is possible once this
//      starts, and it runs straight through to completion (or cancellation).
class FileOpsWorker : public QObject {
    Q_OBJECT

public:
    explicit FileOpsWorker(QObject *parent = nullptr);
    ~FileOpsWorker() override;

    enum class Collision { None, Replace, Skip, KeepBoth };

    struct Item {
        QString srcPath;   // absolute
        QString dstName;   // desired name at the destination; if empty, preflight() fills in srcPath's basename
        qint64 srcFileId = 0; // 0 = source isn't a row pixet's index knows about (external drop/paste)
        qint64 srcDirId = 0;  // 0 = unknown/not indexed

        // Set by preflight(); read (and Collision::None/Replace/KeepBoth/Skip set)
        // by the caller before execute() for every item where hasConflict is true.
        // Ignored by execute() when hasConflict is false.
        bool hasConflict = false;
        qint64 conflictSize = 0;  // the existing destination file's size, for the dialog
        qint64 conflictMtime = 0; // ...and modified time
        Collision resolution = Collision::None;
    };

    struct Request {
        quint64 id = 0;
        bool move = false; // false = copy
        QString dstDirPath;
        QList<Item> items;
    };

    // A file to send to the Recycle Bin/Trash - unlike Item, there's no destination,
    // hence no preflight/collision stage (see deleteFiles() below - single-stage,
    // unlike preflight()/execute()).
    struct DeleteItem {
        QString path;      // absolute
        qint64 fileId = 0; // 0 = not a row pixet's index knows about
        qint64 dirId = 0;
    };

    struct DeleteRequest {
        quint64 id = 0;
        QList<DeleteItem> items;
    };

public slots:
    void preflight(FileOpsWorker::Request req);
    void execute(FileOpsWorker::Request req);
    // Single-stage, unlike preflight()/execute() - a delete has nothing to collide
    // with, so there's no dialog to run before committing. MainWindow's own
    // confirmation dialog happens before this is ever emitted.
    void deleteFiles(FileOpsWorker::DeleteRequest req);

public:
    // Not a slot: std::atomic_bool::store() is thread-safe to call directly from the
    // UI thread while execute() runs on the worker thread, so there's no need to
    // round-trip this through the event queue the way the two slots above must.
    void cancelCurrent() { cancel_.store(true); }

signals:
    // `req` echoes back with `rejected` items already removed from req.items and
    // every remaining item's hasConflict/conflictSize/conflictMtime filled in.
    void preflightReady(FileOpsWorker::Request req, QStringList rejected);
    void progress(quint64 id, int done, int total, QString currentName);
    // srcFileIds: rows to remove from a currently-displayed folder if it happens to
    // be where any of these used to live (see MainWindow::onFileOpFinished - safe to
    // apply unconditionally, since removing a row id that isn't currently loaded is
    // already a no-op). addedNames: names now present in dstDirPath, for incremental
    // model insert into the currently-displayed folder if that's dstDirPath.
    void finished(quint64 id, QString dstDirPath, QList<qint64> srcFileIds, QStringList addedNames, int succeeded,
                  int failed, QStringList errors);
    // removedFileIds: rows to drop from a currently-displayed folder, same
    // safe-even-if-not-loaded contract as finished()'s srcFileIds.
    void deleteFinished(quint64 id, QList<qint64> removedFileIds, int succeeded, int failed, QStringList errors);

private:
    pixet::Database &db();

    QThread thread_;
    std::unique_ptr<pixet::Database> db_; // created lazily, on the worker thread
    std::atomic_bool cancel_{false};
};

// Request (which nests Collision/Item inside its QList<Item> member) and DeleteRequest
// (nesting DeleteItem) are the only types here that ever cross a queued signal/slot
// boundary directly - their own copy constructors (compiler-generated) transitively
// copy everything nested inside via ordinary C++ semantics, so Item/Collision/
// DeleteItem don't need their own metatype registration. Registered explicitly in
// FileOpsWorker's constructor (see the .cpp) - this project has one prior documented
// case of a silent, stderr-invisible failure from a WIN32-subsystem app skipping
// exactly this kind of setup (moveToThread on a parented object), so this isn't left
// to moc's best-effort auto-registration.
Q_DECLARE_METATYPE(FileOpsWorker::Request)
Q_DECLARE_METATYPE(FileOpsWorker::DeleteRequest)
