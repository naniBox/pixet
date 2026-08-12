#include "FileOpsWorker.h"

#include <set>

#include "db/Database.h"
#include "fileops/FileOps.h"
#include "util/AppPaths.h"
#include "util/FileMove.h"
#include "util/PathUtil.h"
#include "util/ProcessId.h"

namespace {

// Basename of a Qt path string - trivial enough to keep local rather than exposed
// from pixet_core (whose own equivalent, used in FileOps.cpp, works on std::string).
QString baseNameOfQ(const QString &path) {
    int cut = qMax(path.lastIndexOf(QLatin1Char('/')), path.lastIndexOf(QLatin1Char('\\')));
    return cut >= 0 ? path.mid(cut + 1) : path;
}

QString dirNameOfQ(const QString &path) {
    int cut = qMax(path.lastIndexOf(QLatin1Char('/')), path.lastIndexOf(QLatin1Char('\\')));
    return cut >= 0 ? path.left(cut) : QString();
}

} // namespace

FileOpsWorker::FileOpsWorker(QObject *parent) : QObject(parent) {
    // Request is the only custom type here that ever crosses a queued signal/slot
    // boundary directly (see the header comment) - registered once, up front, rather
    // than trusting moc's best-effort auto-registration for a type this app has never
    // used in a signal before. An unregistered type here fails as a qWarning to a
    // console this WIN32-subsystem build doesn't have, exactly the invisible-failure
    // class the devlog already documents for moveToThread on a parented object.
    qRegisterMetaType<FileOpsWorker::Request>("FileOpsWorker::Request");
    qRegisterMetaType<FileOpsWorker::DeleteRequest>("FileOpsWorker::DeleteRequest");
    qRegisterMetaType<QList<qint64>>("QList<qint64>");

    moveToThread(&thread_);
    thread_.start();
}

FileOpsWorker::~FileOpsWorker() {
    thread_.quit();
    thread_.wait();
}

pixet::Database &FileOpsWorker::db() {
    if (!db_) db_ = std::make_unique<pixet::Database>(pixet::indexDbPath(), pixet::thumbsDbPath(), false);
    return *db_;
}

void FileOpsWorker::preflight(FileOpsWorker::Request req) {
    QStringList rejected;
    QList<Item> resolved;
    resolved.reserve(req.items.size());

    for (Item item : req.items) {
        std::string srcUtf8 = item.srcPath.toStdString();
        // Rejected outright rather than attempted and left to fail deep inside
        // execute() - the user finds out immediately ("3 items skipped") instead of
        // it silently showing up as a per-item error buried in a batch report.
        // Directories are explicitly out of scope for this pass (see the class
        // comment on ThumbGridView's dropEvent) - recursive copy/index is a separate
        // feature.
        if (pixet::isDirectory(srcUtf8) || !pixet::fileExists(srcUtf8)) {
            rejected << item.srcPath;
            continue;
        }

        if (item.dstName.isEmpty()) item.dstName = baseNameOfQ(item.srcPath);

        std::string dstPath = pixet::joinPath(req.dstDirPath.toStdString(), item.dstName.toStdString());
        int64_t existingSize = 0, existingMtime = 0;
        // Collision detection is always a filesystem question, never a DB one -
        // pixet's index can be stale or absent for the destination (an external
        // drop into a folder pixet hasn't scanned yet), and both target filesystems
        // are case-insensitive while files.name's UNIQUE constraint is not.
        if (pixet::fileExists(dstPath) && pixet::statFile(dstPath, &existingSize, &existingMtime)) {
            item.hasConflict = true;
            item.conflictSize = existingSize;
            item.conflictMtime = existingMtime;
        }

        resolved << item;
    }

    req.items = resolved;
    emit preflightReady(req, rejected);
}

void FileOpsWorker::execute(FileOpsWorker::Request req) {
    cancel_.store(false);

    pixet::fileops::Plan plan;
    plan.kind = req.move ? pixet::fileops::OpKind::Move : pixet::fileops::OpKind::Copy;
    plan.dstDirPath = req.dstDirPath.toStdString();

    std::set<std::string, pixet::fileops::CaseInsensitiveLess> alsoTaken;
    QList<Item> keptItems; // parallel to plan.items / the eventual report.outcomes

    for (const Item &item : req.items) {
        QString dstName = item.dstName;
        bool replaceExisting = false;

        if (item.hasConflict) {
            switch (item.resolution) {
                case Collision::Skip:
                    continue;
                case Collision::Replace:
                    replaceExisting = true;
                    break;
                case Collision::KeepBoth: {
                    std::string unique =
                        pixet::fileops::uniqueNameFor(plan.dstDirPath, dstName.toStdString(), alsoTaken);
                    if (unique.empty()) continue; // couldn't find a free name within the search bound - drop it
                    dstName = QString::fromStdString(unique);
                    break;
                }
                case Collision::None:
                    // A conflicting item with no decision made is a caller bug -
                    // treat it as Skip rather than silently overwrite or guess.
                    continue;
            }
        }

        alsoTaken.insert(dstName.toStdString());

        pixet::fileops::PlannedItem planned;
        planned.srcPath = item.srcPath.toStdString();
        planned.dstName = dstName.toStdString();
        planned.srcFileId = item.srcFileId;
        planned.srcDirId = item.srcDirId;
        planned.replaceExisting = replaceExisting;
        plan.items.push_back(std::move(planned));
        keptItems.push_back(item);
    }

    std::string owner = "gui:fileops:pid:" + std::to_string(pixet::currentProcessId());

    pixet::fileops::Report report = pixet::fileops::execute(
        db(), plan, owner,
        [this, id = req.id](const pixet::fileops::Progress &p) {
            emit progress(id, (int)p.done, (int)p.total, QString::fromStdString(p.currentName));
        },
        &cancel_);

    QList<qint64> srcFileIds;
    QStringList addedNames;
    QStringList errors;
    for (size_t i = 0; i < report.outcomes.size(); ++i) {
        const auto &outcome = report.outcomes[i];
        if (!outcome.ok) {
            errors << QStringLiteral("%1: %2").arg(QString::fromStdString(outcome.srcPath),
                                                     QString::fromStdString(outcome.error));
            continue;
        }
        addedNames << QString::fromStdString(outcome.dstName);
        // A Copy always leaves the source's original folder untouched. A Move vacates
        // the row from wherever it used to live at unless neither the directory nor the
        // name actually changed (a genuine no-op, e.g. Cut then Paste right back where
        // it came from) - a same-directory *rename* (different name, same dir) still
        // needs the old-named row removed here, or insertOrUpdateFileByName() below
        // inserts a second, ghost row for the same underlying file instead of updating
        // the existing one in place.
        if (plan.kind == pixet::fileops::OpKind::Move && outcome.srcFileId != 0 && (int)i < keptItems.size()) {
            const QString &srcPath = keptItems[(int)i].srcPath;
            bool locationChanged =
                dirNameOfQ(srcPath) != req.dstDirPath || baseNameOfQ(srcPath) != QString::fromStdString(outcome.dstName);
            if (locationChanged) srcFileIds << outcome.srcFileId;
        }
    }

    emit finished(req.id, req.dstDirPath, srcFileIds, addedNames, (int)report.succeeded, (int)report.failed, errors);
}

void FileOpsWorker::deleteFiles(FileOpsWorker::DeleteRequest req) {
    cancel_.store(false);

    std::vector<pixet::fileops::DeleteItem> items;
    items.reserve(req.items.size());
    for (const DeleteItem &item : req.items) {
        pixet::fileops::DeleteItem planned;
        planned.path = item.path.toStdString();
        planned.fileId = item.fileId;
        planned.dirId = item.dirId;
        items.push_back(std::move(planned));
    }

    std::string owner = "gui:fileops:pid:" + std::to_string(pixet::currentProcessId());

    pixet::fileops::Report report = pixet::fileops::executeDelete(
        db(), items, owner,
        [this, id = req.id](const pixet::fileops::Progress &p) {
            emit progress(id, (int)p.done, (int)p.total, QString::fromStdString(p.currentName));
        },
        &cancel_);

    QList<qint64> removedFileIds;
    QStringList errors;
    for (const auto &outcome : report.outcomes) {
        if (!outcome.ok) {
            errors << QStringLiteral("%1: %2").arg(QString::fromStdString(outcome.srcPath),
                                                     QString::fromStdString(outcome.error));
            continue;
        }
        if (outcome.srcFileId != 0) removedFileIds << outcome.srcFileId;
    }

    emit deleteFinished(req.id, removedFileIds, (int)report.succeeded, (int)report.failed, errors);
}
