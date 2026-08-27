#include "DirRows.h"

#include <string>
#include <vector>

#include "../db/Database.h"
#include "DirWalker.h"

namespace pixet {

namespace {

// A barren row can only be removed once its children are, so the sweep has to repeat
// until it stops finding any. That converges in as many passes as the deepest barren
// chain is long; the cap is the same 64 Indexer::run() bounds its own descent by, and
// exists for the same reason - a cycle in the parent_id graph must cost a bounded amount
// of work rather than spinning here forever.
constexpr int kMaxPrunePasses = 64;

// Deletes the thumbnail blobs belonging to `files` rows matching `filesWhere`, then the
// rows themselves. The blobs have to go first and by explicit query: thumbs.thumbs lives
// in a second, ATTACHed database with no foreign key to files, so dropping a files row
// simply strands its blob where nothing will ever look for it again.
void deleteFilesWhere(Database &db, const std::string &filesWhere, PruneStats &stats) {
    auto countThumbs = db.prepare("SELECT COUNT(*) FROM thumbs.thumbs WHERE id IN "
                                   "(SELECT thumb_id FROM files WHERE thumb_id IS NOT NULL AND " + filesWhere + ")");
    if (countThumbs.step()) stats.thumbsRemoved += countThumbs.columnInt64(0);

    auto countFiles = db.prepare("SELECT COUNT(*) FROM files WHERE " + filesWhere);
    if (countFiles.step()) stats.filesRemoved += countFiles.columnInt64(0);

    db.exec("DELETE FROM thumbs.thumbs WHERE id IN "
            "(SELECT thumb_id FROM files WHERE thumb_id IS NOT NULL AND " + filesWhere + ");");
    db.exec("DELETE FROM files WHERE " + filesWhere + ";");
}

// One pass of "remove every dirs row that holds no media, parents nothing, and isn't
// being worked on". Returns how many it removed, so the caller knows whether another
// pass can find anything.
int64_t pruneBarrenOnce(Database &db) {
    const char *kWhere =
        "NOT EXISTS (SELECT 1 FROM files  f WHERE f.dir_id    = dirs.id) AND "
        "NOT EXISTS (SELECT 1 FROM dirs   c WHERE c.parent_id = dirs.id) AND "
        "NOT EXISTS (SELECT 1 FROM claims k WHERE k.dir_id    = dirs.id)";

    int64_t n = 0;
    auto count = db.prepare(std::string("SELECT COUNT(*) FROM dirs WHERE ") + kWhere);
    if (count.step()) n = count.columnInt64(0);
    if (n > 0) db.exec(std::string("DELETE FROM dirs WHERE ") + kWhere + ";");
    return n;
}

} // namespace

int64_t upsertDir(Database &db, const std::string &path, int64_t parentId) {
    auto ins = db.prepare("INSERT OR IGNORE INTO dirs(parent_id, path, mtime, scanned_at) VALUES(?,?,0,0)");
    if (parentId < 0) ins.bindNull(1); else ins.bind(1, parentId);
    ins.bind(2, path);
    ins.step();

    auto sel = db.prepare("SELECT id FROM dirs WHERE path=?");
    sel.bind(1, path);
    sel.step();
    return sel.columnInt64(0);
}

PruneStats pruneDirs(Database &db) {
    PruneStats stats;

    // Pass 1: the barren rows, in one transaction. No filesystem access at all - this is
    // the bulk of the work on a library that has been crawled, and it has to run before
    // anything starts stat'ing paths.
    db.beginTransaction();
    for (int pass = 0; pass < kMaxPrunePasses; ++pass) {
        int64_t removed = pruneBarrenOnce(db);
        if (removed == 0) break;
        stats.dirsBarren += removed;
    }
    db.commit();

    // Pass 2: of what's left, which directories are provably gone? Read the candidate
    // list out first and stat outside a transaction - holding a write lock across
    // thousands of stat() calls would block every other connection for the duration, and
    // an unresponsive network mount can make a single one of them take seconds.
    std::vector<std::pair<int64_t, std::string>> survivors;
    {
        auto sel = db.prepare("SELECT id, path FROM dirs WHERE NOT EXISTS "
                               "(SELECT 1 FROM claims k WHERE k.dir_id = dirs.id)");
        while (sel.step()) survivors.emplace_back(sel.columnInt64(0), sel.columnText(1));
    }

    std::vector<int64_t> missing;
    for (const auto &[id, path] : survivors) {
        if (dirPresence(path) == DirPresence::Missing) missing.push_back(id);
    }

    db.beginTransaction();
    for (int64_t id : missing) {
        // Re-checked inside the transaction: a claim may have been taken while we were
        // stat'ing, which means an indexer is in this directory right now and is the one
        // entitled to decide what happens to it.
        auto claimed = db.prepare("SELECT 1 FROM claims WHERE dir_id=?");
        claimed.bind(1, id);
        if (claimed.step()) continue;

        deleteFilesWhere(db, "dir_id = " + std::to_string(id), stats);
        auto del = db.prepare("DELETE FROM dirs WHERE id=?");
        del.bind(1, id);
        del.step();
        stats.dirsMissing++;
    }

    // Files whose directory no longer has a row - the rows just orphaned above are
    // already gone, so what this catches is anything stranded by an earlier version, or
    // by the one narrow race this design leaves open: a directory's dirs row is created
    // by Indexer::run() a moment before indexOneDirectory() claims it, so a prune landing
    // in that window can remove a row that is about to be written against. Sweeping
    // orphans every time makes that self-healing rather than a permanent leak.
    deleteFilesWhere(db, "NOT EXISTS (SELECT 1 FROM dirs d WHERE d.id = files.dir_id)", stats);
    db.exec("DELETE FROM claims WHERE NOT EXISTS (SELECT 1 FROM dirs d WHERE d.id = claims.dir_id);");

    // Removing a directory can leave its parent barren, so give the cheap pass one more
    // run rather than waiting for the next cycle to notice.
    for (int pass = 0; pass < kMaxPrunePasses; ++pass) {
        int64_t removed = pruneBarrenOnce(db);
        if (removed == 0) break;
        stats.dirsBarren += removed;
    }
    db.commit();

    return stats;
}

} // namespace pixet
