#include "FileOps.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>

#include "../db/Database.h"
#include "../db/Schema.h"
#include "../scan/Claims.h"
#include "../scan/DirRows.h"
#include "../util/PathUtil.h"
#include "../util/ProcessId.h"
#include "../util/Time.h"

namespace pixet::fileops {

namespace {

// RAII transaction guard - Statement::step()/Database::exec() throw std::runtime_error
// (see Database.cpp), and unlike Indexer's transactions (which are never expected to
// throw in practice and don't guard against it), this is the app's first
// filesystem-mutating code path: a DB exception here must not leak an open
// transaction or propagate out and abort the whole batch (see execute()'s per-item
// try/catch).
struct Transaction {
    explicit Transaction(Database &db) : db_(db) { db_.beginTransaction(); }
    ~Transaction() {
        if (!done_) {
            try {
                db_.rollback();
            } catch (...) {
            }
        }
    }
    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;

    void commit() {
        db_.commit();
        done_ = true;
    }

    Database &db_;
    bool done_ = false;
};

unsigned char asciiLower(unsigned char c) { return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c; }

std::string baseNameOf(const std::string &path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::atomic<int64_t> g_tempCounter{0};

// ".pixet-old-<pid>-<n>" for a collision victim moved aside, ".pixet-tmp-<pid>-<n>"
// isn't used on Windows (MoveFileExW/CopyFileExW don't need a staging name - see
// FileMove_win.cpp) but the macOS implementation uses the equivalent pattern for its
// own copy-then-publish dance (FileMove_mac.cpp). Both are deliberately extension-
// less-recognizable so classifyFormat() (Schema.cpp) returns Format::Unknown for
// them - an orphaned one left by a hard crash is never indexed and never gets a
// files row.
std::string tempNameForVictim(const std::string &dstPath) {
    return dstPath + ".pixet-old-" + std::to_string(currentProcessId()) + "-" + std::to_string(++g_tempCounter);
}

// One item's worth of work: filesystem operation first, DB update second, always - see
// the class comment on execute() for the crash-recovery reasoning behind that order. Never
// throws (any DB exception is caught and turned into a failed outcome) so one bad item
// can't abort the rest of the batch.
void executeOneItem(Database &db, OpKind kind, const std::string &dstDirPath, int64_t dstDirId,
                     const PlannedItem &item, ItemOutcome &outcome) {
    try {
        std::string dstPath = joinPath(dstDirPath, item.dstName);

        // A move back onto the exact same (dir, name) it already occupies - e.g. Cut,
        // then Paste right back where it came from - is a no-op, not an error.
        if (kind == OpKind::Move && item.srcDirId == dstDirId && baseNameOf(item.srcPath) == item.dstName) {
            outcome.ok = true;
            outcome.dstFileId = item.srcFileId;
            return;
        }

        std::string victimTemp;
        int64_t victimThumbId = 0;
        bool hadVictim = false;

        if (item.replaceExisting) {
            victimTemp = tempNameForVictim(dstPath);
            FsResult renameResult = renameWithinDir(dstPath, victimTemp);
            if (renameResult != FsResult::Ok) {
                outcome.fsResult = renameResult;
                outcome.error = std::string("could not move existing file aside: ") + fsResultName(renameResult);
                return;
            }
            hadVictim = true;
        }

        FsResult opResult = (kind == OpKind::Move) ? moveFile(item.srcPath, dstPath) : copyFile(item.srcPath, dstPath);
        if (opResult != FsResult::Ok) {
            // Never proceed to touch the DB, and put the victim back rather than
            // leaving the destination name occupied by neither the old nor the new
            // file.
            if (hadVictim) renameWithinDir(victimTemp, dstPath);
            outcome.fsResult = opResult;
            outcome.error = std::string("file operation failed: ") + fsResultName(opResult);
            return;
        }

        // The *actual* post-op values, not the pre-op ones - this is what stops the
        // next Refresh/BackgroundReconciler pass from re-thumbnailing a file whose
        // thumbnail this call is about to carefully preserve (a cross-volume move
        // degrades to copy-then-delete, which is not guaranteed to preserve mtime).
        int64_t dstSize = 0, dstMtime = 0;
        statFile(dstPath, &dstSize, &dstMtime); // best-effort; 0/0 self-heals on the next sync if this ever fails

        int64_t dstFileId = 0;
        bool preserved = false;

        {
            Transaction txn(db);

            if (hadVictim) {
                auto sel = db.prepare("SELECT id, thumb_id FROM files WHERE dir_id=? AND name=?");
                sel.bind(1, dstDirId);
                sel.bind(2, item.dstName);
                if (sel.step()) {
                    int64_t victimId = sel.columnInt64(0);
                    victimThumbId = sel.columnIsNull(1) ? 0 : sel.columnInt64(1);
                    auto del = db.prepare("DELETE FROM files WHERE id=?");
                    del.bind(1, victimId);
                    del.step();
                }
            }

            if (kind == OpKind::Move && item.srcFileId != 0) {
                // The smart move: keep the existing row (and its thumb_id) alive at
                // the new location instead of Indexer's blind delete+reinsert -
                // zero re-thumbnailing for a file this call already knows moved.
                auto upd = db.prepare("UPDATE files SET dir_id=?, name=?, mtime=?, size=? WHERE id=?");
                upd.bind(1, dstDirId);
                upd.bind(2, item.dstName);
                upd.bind(3, dstMtime);
                upd.bind(4, dstSize);
                upd.bind(5, item.srcFileId);
                upd.step();
                if (db.changes() > 0) {
                    dstFileId = item.srcFileId;
                    preserved = true;
                }
                // changes()==0 means the source row raced away under a concurrent
                // indexer diff (see the class comment on best-effort claims) - fall
                // through to the plain insert below rather than losing the file.
            }

            if (dstFileId == 0) {
                Format fmt = classifyFormat(item.dstName);
                auto ins = db.prepare("INSERT INTO files(dir_id, name, mtime, size, kind, fmt, orientation, state) "
                                       "VALUES(?,?,?,?,?,?,1,0)");
                ins.bind(1, dstDirId);
                ins.bind(2, item.dstName);
                ins.bind(3, dstMtime);
                ins.bind(4, dstSize);
                ins.bind(5, (int64_t)kindForFormat(fmt));
                ins.bind(6, (int64_t)fmt);
                ins.step();
                dstFileId = db.lastInsertRowId();
            }

            txn.commit();
        }

        // Outside the transaction on purpose - SQLite's multi-ATTACH transactions
        // aren't atomic across databases under WAL (the mode Database::Database sets),
        // so this deliberately isn't bundled with the files-row transaction above. A
        // crash between them leaks one orphaned blob (invisible, reclaimed by Reset
        // Index + VACUUM) instead of risking a files row left with a dangling
        // thumb_id if the ordering were reversed.
        if (victimThumbId != 0) {
            try {
                auto del = db.prepare("DELETE FROM thumbs.thumbs WHERE id=?");
                del.bind(1, victimThumbId);
                del.step();
            } catch (const std::exception &) {
                // Not worth failing an otherwise-successful item over a leaked blob.
            }
        }
        if (hadVictim) removeFile(victimTemp); // best-effort; an orphan here is unindexable (Format::Unknown), not silently wrong

        outcome.dstFileId = dstFileId;
        outcome.thumbPreserved = preserved;
        outcome.ok = true;
    } catch (const std::exception &e) {
        outcome.ok = false;
        outcome.error = std::string("database error: ") + e.what();
    }
}

// Mirrors executeOneItem()'s structure for the simpler delete case: no destination, no
// collision, just trash-then-remove-the-row. Never throws (any DB exception is caught
// and turned into a failed outcome), same reasoning as executeOneItem.
void executeOneDelete(Database &db, const DeleteItem &item, ItemOutcome &outcome) {
    try {
        FsResult opResult = moveToTrash(item.path);
        if (opResult != FsResult::Ok) {
            outcome.fsResult = opResult;
            outcome.error = std::string("delete failed: ") + fsResultName(opResult);
            return;
        }

        int64_t thumbId = 0;
        if (item.fileId != 0) {
            Transaction txn(db);
            auto sel = db.prepare("SELECT thumb_id FROM files WHERE id=?");
            sel.bind(1, item.fileId);
            if (sel.step()) thumbId = sel.columnIsNull(0) ? 0 : sel.columnInt64(0);

            auto del = db.prepare("DELETE FROM files WHERE id=?");
            del.bind(1, item.fileId);
            del.step();

            txn.commit();
        }

        // Outside the transaction on purpose - see executeOneItem()'s identical
        // reasoning: SQLite's multi-ATTACH transactions aren't atomic across databases
        // under WAL, so a crash between the two leaks one orphaned (invisible) blob
        // rather than risking a files row left with a dangling thumb_id.
        if (thumbId != 0) {
            try {
                auto del = db.prepare("DELETE FROM thumbs.thumbs WHERE id=?");
                del.bind(1, thumbId);
                del.step();
            } catch (const std::exception &) {
                // Not worth failing an otherwise-successful delete over a leaked blob.
            }
        }

        outcome.ok = true;
    } catch (const std::exception &e) {
        outcome.ok = false;
        outcome.error = std::string("database error: ") + e.what();
    }
}

} // namespace

bool CaseInsensitiveLess::operator()(const std::string &a, const std::string &b) const {
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        unsigned char ca = asciiLower((unsigned char)a[i]);
        unsigned char cb = asciiLower((unsigned char)b[i]);
        if (ca != cb) return ca < cb;
    }
    return a.size() < b.size();
}

std::string uniqueNameFor(const std::string &dirPath, const std::string &name,
                           const std::set<std::string, CaseInsensitiveLess> &alsoTaken) {
    size_t dot = name.find_last_of('.');
    // dot==0 is a leading-dot dotfile with no other '.' (".DS_Store") - treat the
    // whole name as the stem so it becomes ".DS_Store (2)", not "(2).DS_Store".
    bool hasExt = dot != std::string::npos && dot != 0;
    std::string stem = hasExt ? name.substr(0, dot) : name;
    std::string ext = hasExt ? name.substr(dot) : std::string();

    constexpr int kMaxTries = 1000;
    for (int n = 2; n <= kMaxTries; ++n) {
        std::string candidate = stem + " (" + std::to_string(n) + ")" + ext;
        if (alsoTaken.count(candidate)) continue;
        std::string candidatePath = joinPath(dirPath, candidate);
        if (fileExists(candidatePath) || isDirectory(candidatePath)) continue;
        return candidate;
    }
    return {};
}

Report execute(Database &db, const Plan &plan, const std::string &owner,
               const std::function<void(const Progress &)> &onProgress, const std::atomic_bool *cancel) {
    Report report;
    if (plan.items.empty()) return report;

    int64_t dstDirId = upsertDir(db, plan.dstDirPath, -1);

    // Best-effort claims on every directory this batch touches, source and
    // destination alike, claimed in a fixed (ascending dir_id) order - nothing today
    // holds two claims at once, so no lock-ordering cycle exists yet, but this is the
    // first two-claim holder in the codebase and the next one shouldn't have to
    // rediscover the rule. "Best-effort" is deliberate, not a shortcut:
    // BackgroundReconciler can hold a single directory's claim for minutes on a large
    // un-thumbnailed folder, and aborting the user's Cut/Paste because a background
    // sweep happens to be running there would be worse than the alternative. If a
    // claim can't be won, this proceeds anyway - the worst case if something actually
    // races is that a concurrent indexer's blind by-name diff deletes a row this call
    // just updated (see executeOneItem's changes()==0 fallback), which self-heals as
    // one extra re-thumbnail on the next visit. Never data loss or corruption.
    std::vector<int64_t> dirIds{dstDirId};
    for (const auto &item : plan.items) {
        if (item.srcDirId != 0) dirIds.push_back(item.srcDirId);
    }
    std::sort(dirIds.begin(), dirIds.end());
    dirIds.erase(std::unique(dirIds.begin(), dirIds.end()), dirIds.end());

    ClaimManager claims(db);
    for (int64_t dirId : dirIds) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (claims.tryClaim(dirId, owner, nowMillis())) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }

    size_t total = plan.items.size();
    size_t index = 0;
    for (const auto &item : plan.items) {
        if (cancel && cancel->load()) {
            report.cancelled = true;
            break;
        }

        ItemOutcome outcome;
        outcome.srcPath = item.srcPath;
        outcome.dstName = item.dstName;
        outcome.srcFileId = item.srcFileId;

        executeOneItem(db, plan.kind, plan.dstDirPath, dstDirId, item, outcome);

        if (outcome.ok) report.succeeded++;
        else report.failed++;
        report.outcomes.push_back(std::move(outcome));

        ++index;
        if (onProgress) onProgress(Progress{index, total, item.dstName});
        if (index % 16 == 0) {
            for (int64_t dirId : dirIds) claims.heartbeat(dirId, owner, nowMillis());
        }
    }

    for (int64_t dirId : dirIds) claims.release(dirId, owner);
    return report;
}

Report executeDelete(Database &db, const std::vector<DeleteItem> &items, const std::string &owner,
                      const std::function<void(const Progress &)> &onProgress, const std::atomic_bool *cancel) {
    Report report;
    if (items.empty()) return report;

    // Best-effort claims, same reasoning as execute() - see that function's comment.
    std::vector<int64_t> dirIds;
    for (const auto &item : items) {
        if (item.dirId != 0) dirIds.push_back(item.dirId);
    }
    std::sort(dirIds.begin(), dirIds.end());
    dirIds.erase(std::unique(dirIds.begin(), dirIds.end()), dirIds.end());

    ClaimManager claims(db);
    for (int64_t dirId : dirIds) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (claims.tryClaim(dirId, owner, nowMillis())) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }

    size_t total = items.size();
    size_t index = 0;
    for (const auto &item : items) {
        if (cancel && cancel->load()) {
            report.cancelled = true;
            break;
        }

        ItemOutcome outcome;
        outcome.srcPath = item.path;
        outcome.srcFileId = item.fileId;

        executeOneDelete(db, item, outcome);

        if (outcome.ok) report.succeeded++;
        else report.failed++;
        report.outcomes.push_back(std::move(outcome));

        ++index;
        if (onProgress) onProgress(Progress{index, total, baseNameOf(item.path)});
        if (index % 16 == 0) {
            for (int64_t dirId : dirIds) claims.heartbeat(dirId, owner, nowMillis());
        }
    }

    for (int64_t dirId : dirIds) claims.release(dirId, owner);
    return report;
}

} // namespace pixet::fileops
