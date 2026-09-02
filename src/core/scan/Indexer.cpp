#include "Indexer.h"

#include "../util/Profile.h"

#include <algorithm>
#include <future>
#include <limits>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "../db/Database.h"
#include "../meta/JpegExif.h"
#include "../thumb/ThumbGenerator.h"
#include "../util/FileIO.h"
#include "../util/PathUtil.h"
#include "../util/Shutdown.h"
#include "../util/Time.h"
#include "DirRows.h"
#include "DirWalker.h"

namespace pixet {

namespace {

constexpr size_t kBatchSize = 64;

// EXIF sits in the file header, so a bounded prefix is all backfillGps() needs to parse it -
// the same budget HoverInfoWorker uses for its own on-demand read.
constexpr size_t kExifPrefixBytes = 64 * 1024;

// 0 (the IndexOptions default) means "auto" - hardware_concurrency() can itself return 0
// when the platform can't determine a core count, which floor(..., 1) turns into "run
// sequentially" rather than a ThreadPool of zero workers hanging on the first submit().
size_t resolveThreadCount(int requested) {
    if (requested > 0) return (size_t)requested;
    unsigned hw = std::thread::hardware_concurrency();
    return hw > 0 ? (size_t)hw : 1;
}

struct ExistingFile {
    int64_t id;
    int64_t mtime;
    int64_t size;
    int64_t thumbId; // 0 = none
};

// One file in a directory still waiting for a thumbnail. Pass B holds these in the
// order the work will be done, which IndexCallbacks::onWantFirst is allowed to reorder.
struct PendingRow {
    int64_t fileId;
    std::string name;
    int64_t existingThumbId; // 0 = none
};

// Moves `wanted`'s file ids to the front of pending[from, end), in the order given,
// leaving everything else in its existing relative order behind them. Ids that aren't
// in that range are ignored - see IndexCallbacks::onWantFirst on why that's the
// contract rather than an error.
//
// stable_sort rather than a partition because it does both halves of the job at once:
// the wanted ids come out ordered by the caller's own ranking, and everything else
// keeps the order it already had. Duplicate ids in `wanted` keep their first (highest)
// rank, so a caller repeating one costs nothing.
void movePendingToFront(std::vector<PendingRow> &pending, size_t from, const std::vector<int64_t> &wanted) {
    if (wanted.empty() || from >= pending.size()) return;

    std::unordered_map<int64_t, size_t> rank;
    rank.reserve(wanted.size());
    for (size_t i = 0; i < wanted.size(); ++i) rank.emplace(wanted[i], i);

    auto rankOf = [&rank](const PendingRow &p) {
        auto it = rank.find(p.fileId);
        return it == rank.end() ? std::numeric_limits<size_t>::max() : it->second;
    };
    std::stable_sort(pending.begin() + (std::ptrdiff_t)from, pending.end(),
                      [&rankOf](const PendingRow &a, const PendingRow &b) { return rankOf(a) < rankOf(b); });
}

struct PendingThumb {
    int64_t fileId;
    Format fmt;
    int64_t oldThumbId; // 0 = none. Nonzero means this thumb is *replacing* an
                         // existing one (e.g. a RAW file upgrading from
                         // embedded-preview to full render via --render-raws) - that
                         // old blob needs deleting or it leaks in thumbs.db forever.
    ThumbResult result;
};

} // namespace

Indexer::Indexer(Database &db, IndexOptions opts)
    : db_(db), opts_(std::move(opts)), claims_(db), pool_(resolveThreadCount(opts_.threadCount)) {}

void Indexer::indexOneDirectory(int64_t dirId, const std::string &dirPath,
                                 std::vector<std::pair<int64_t, std::string>> &subdirsOut, IndexStats &stats,
                                 const IndexCallbacks &callbacks) {
    int64_t nowMs = nowMillis();
    if (!claims_.tryClaim(dirId, opts_.owner, nowMs)) {
        stats.dirsSkippedClaimed++;
        return;
    }

    // Everything past the claim runs under this guard. Without it, a throw from any DB call -
    // the observed case was a failed COMMIT - left two things broken behind it, both worse
    // than the original error:
    //
    //  1. An open transaction on this connection. The connection is reused for every
    //     subsequent directory, so the *next* BEGIN IMMEDIATE would fail with "cannot start a
    //     transaction within a transaction" and keep failing - one transient error turning
    //     into a permanently broken indexer.
    //  2. A claim row for this directory, which then blocks any indexer from touching it until
    //     the heartbeat goes stale. Two such rows were left behind by the crash that prompted
    //     this.
    //
    // rollback() is itself allowed to fail (there may be no transaction open, depending on
    // where the throw happened) and its failure is deliberately ignored - it's cleanup, and
    // the original exception is the one worth propagating.
    // Dismissed by every path that returns normally - those already release the claim
    // themselves, and there's no transaction open to roll back.
    struct FailureGuard {
        Indexer *self;
        int64_t dirId;
        bool dismissed = false;
        ~FailureGuard() {
            if (dismissed) return;
            try { self->db_.rollback(); } catch (...) {}
            try { self->claims_.release(dirId, self->opts_.owner); } catch (...) {}
        }
    } guard{this, dirId};

    // dirMtimeUnix throws if the directory can't be stat'd, and must not be called bare here:
    // unguarded, one unreadable folder throws straight out through run(), aborting the entire
    // index run *and* leaking the claim row taken just above until its heartbeat goes stale.
    // Tolerable on Windows, where an unreadable folder in a photo tree is unusual; not on
    // macOS, where TCC-protected directories are everywhere and
    // "index my home folder" would reliably die on the first one. Handled the same way the
    // listDir failure below already was: release the claim, count it, skip the folder.
    int64_t actualMtime = 0;
    try {
        PIXET_PROF_SCOPE("idx.dirMtime");
        actualMtime = dirMtimeUnix(dirPath);
    } catch (const std::exception &) {
        guard.dismissed = true;
        claims_.release(dirId, opts_.owner);
        stats.dirsSkippedUnreadable++;
        return;
    }

    int64_t storedMtime = 0, scannedAt = 0;
    {
        auto sel = db_.prepare("SELECT mtime, scanned_at FROM dirs WHERE id=?");
        sel.bind(1, dirId);
        if (sel.step()) {
            storedMtime = sel.columnInt64(0);
            scannedAt = sel.columnInt64(1);
        }
    }

    bool freshEnough =
        !opts_.forceRescan && !opts_.forceRethumbnail && scannedAt > 0 && storedMtime == actualMtime;

    if (freshEnough) {
        stats.dirsSkippedFresh++;
        // Only a recursive run has any use for the children, and on a non-recursive one
        // there may not be rows for them at all - see the subdirectory branch of Pass A.
        if (opts_.recursive) {
            auto sel = db_.prepare("SELECT id, path FROM dirs WHERE parent_id=?");
            sel.bind(1, dirId);
            while (sel.step()) {
                subdirsOut.emplace_back(sel.columnInt64(0), sel.columnText(1));
            }
        }
    } else {
        std::vector<DirEntry> entries;
        try {
            PIXET_PROF_SCOPE("idx.listDir");
            entries = listDir(dirPath);
        } catch (const std::exception &) {
            guard.dismissed = true;
            claims_.release(dirId, opts_.owner);
            stats.dirsSkippedUnreadable++;
            return; // directory vanished / permission denied mid-walk - just skip it
        }

        std::unordered_map<std::string, ExistingFile> existing;
        {
            auto sel = db_.prepare("SELECT id, name, mtime, size, thumb_id FROM files WHERE dir_id=?");
            sel.bind(1, dirId);
            while (sel.step()) {
                ExistingFile ef;
                ef.id = sel.columnInt64(0);
                std::string name = sel.columnText(1);
                ef.mtime = sel.columnInt64(2);
                ef.size = sel.columnInt64(3);
                ef.thumbId = sel.columnIsNull(4) ? 0 : sel.columnInt64(4);
                existing.emplace(std::move(name), ef);
            }
        }

        PIXET_PROF_SCOPE("idx.passA");
        db_.beginTransaction();

        for (const auto &entry : entries) {
            if (entry.isDir) {
                // A subdirectory earns a `dirs` row only when this run is actually going
                // to index it. That guard used to be absent, and the row was written for
                // every directory merely *seen* - which on a non-recursive run is pure
                // waste, since run() ignores subdirsOut entirely in that case.
                //
                // It was not harmless waste. `dirs` is append-only and was also
                // BackgroundReconciler's entire worklist, so every directory ever glimpsed
                // became permanent background work: the sweep listed it, that registered
                // its children, and the next pass swept those. One navigation to `/` was
                // therefore enough to enlist the whole disk one level at a time - measured
                // on a real library at 41,349 directory rows for 7,264 media files, with
                // /nix and /System alone accounting for 31,598 of them. The visible symptom
                // was macOS asking for access to Downloads/Documents/Desktop at apparently
                // random moments, which was the rotation reaching them.
                //
                // pixet-index's whole-tree pre-warm is unaffected: it runs recursive, so it
                // still registers and descends everything, and it registers a directory at
                // the moment it commits to indexing it rather than on sight.
                if (opts_.recursive) {
                    subdirsOut.emplace_back(upsertDir(db_, joinPath(dirPath, entry.name), dirId),
                                             joinPath(dirPath, entry.name));
                }
                continue;
            }

            Format fmt = classifyFormat(entry.name);
            if (fmt == Format::Unknown) continue; // not a media file - don't clutter the index

            auto it = existing.find(entry.name);
            if (it == existing.end()) {
                auto ins = db_.prepare(
                    "INSERT INTO files(dir_id, name, mtime, size, kind, fmt, orientation, state) "
                    "VALUES(?,?,?,?,?,?,1,0)");
                ins.bind(1, dirId);
                ins.bind(2, entry.name);
                ins.bind(3, entry.mtimeUnix);
                ins.bind(4, entry.size);
                ins.bind(5, (int64_t)kindForFormat(fmt));
                ins.bind(6, (int64_t)fmt);
                ins.step();
                stats.filesNew++;
            } else {
                const ExistingFile &ef = it->second;
                if (opts_.forceRethumbnail || ef.mtime != entry.mtimeUnix || ef.size != entry.size) {
                    // Changed in place, or a full re-thumbnail was explicitly requested
                    // regardless of whether anything actually changed - drop the stale
                    // thumb and re-queue for Pass B.
                    if (ef.thumbId != 0) {
                        auto delThumb = db_.prepare("DELETE FROM thumbs.thumbs WHERE id=?");
                        delThumb.bind(1, ef.thumbId);
                        delThumb.step();
                    }
                    auto upd = db_.prepare(
                        "UPDATE files SET mtime=?, size=?, fmt=?, kind=?, thumb_id=NULL, state=0 WHERE id=?");
                    upd.bind(1, entry.mtimeUnix);
                    upd.bind(2, entry.size);
                    upd.bind(3, (int64_t)fmt);
                    upd.bind(4, (int64_t)kindForFormat(fmt));
                    upd.bind(5, ef.id);
                    upd.step();
                }
                existing.erase(it);
            }
        }

        // Whatever's left in `existing` is no longer on disk.
        for (const auto &[name, ef] : existing) {
            (void)name;
            if (ef.thumbId != 0) {
                auto delThumb = db_.prepare("DELETE FROM thumbs.thumbs WHERE id=?");
                delThumb.bind(1, ef.thumbId);
                delThumb.step();
            }
            auto delFile = db_.prepare("DELETE FROM files WHERE id=?");
            delFile.bind(1, ef.id);
            delFile.step();
            stats.filesRemoved++;
        }

        auto updDir = db_.prepare("UPDATE dirs SET mtime=?, scanned_at=? WHERE id=?");
        updDir.bind(1, actualMtime);
        updDir.bind(2, nowUnixSeconds());
        updDir.bind(3, dirId);
        updDir.step();

        db_.commit();
    }

    // Pass A (or the fresh-cache equivalent) is done - the file list for this
    // directory is final, even though most thumbnails are probably still pending.
    if (callbacks.onFilesListed) callbacks.onFilesListed(dirId, dirPath);

    claims_.heartbeat(dirId, opts_.owner, nowMillis());

    // Pass B: thumbnail everything still pending in this directory - state=New,
    // plus state=DoneNeedsRender too when a --render-raws pass is asking to catch up
    // every RAW file still sitting on its fast embedded-preview thumbnail (see
    // IndexOptions::renderRaws).
    std::string pendingSql = "SELECT id, name, fmt, thumb_id FROM files WHERE dir_id=? AND (state=" +
                              std::to_string((int64_t)FileState::New);
    if (opts_.renderRaws) pendingSql += " OR state=" + std::to_string((int64_t)FileState::DoneNeedsRender);
    pendingSql += ")";

    PIXET_PROF_SCOPE("idx.passB");
    std::vector<PendingRow> pending;
    {
        auto sel = db_.prepare(pendingSql);
        sel.bind(1, dirId);
        while (sel.step()) {
            int64_t existingThumbId = sel.columnIsNull(3) ? 0 : sel.columnInt64(3);
            pending.push_back({sel.columnInt64(0), sel.columnText(1), existingThumbId});
        }
    }

    std::vector<PendingThumb> batch;
    batch.reserve(kBatchSize);

    auto flushBatch = [&]() {
        if (batch.empty()) return;
        db_.beginTransaction();
        for (auto &pt : batch) {
            switch (pt.result.tier) {
                case ThumbTier::EmbeddedPreview:
                case ThumbTier::Decoded: {
                    auto insThumb = db_.prepare("INSERT INTO thumbs.thumbs(w, h, fmt, bytes) VALUES(?,?,?,?)");
                    insThumb.bind(1, (int64_t)pt.result.width);
                    insThumb.bind(2, (int64_t)pt.result.height);
                    insThumb.bind(3, (int64_t)Format::Jpeg);
                    insThumb.bind(4, pt.result.jpegBytes);
                    insThumb.step();
                    int64_t thumbId = db_.lastInsertRowId();

                    // files.width/height are the ORIGINAL image's dimensions, not the
                    // thumbnail's (that's thumbs.thumbs.w/h, bound above) - fall back to
                    // the thumbnail's own size only if the header-only dimension read
                    // failed (result.origWidth/Height left at 0), so the UI still shows
                    // something rather than a blank field.
                    int64_t fileWidth = pt.result.origWidth > 0 ? pt.result.origWidth : pt.result.width;
                    int64_t fileHeight = pt.result.origHeight > 0 ? pt.result.origHeight : pt.result.height;
                    // RAW's embedded-preview tier lands on DoneNeedsRender, not Done -
                    // a later --render-raws pass picks it back up for the real render
                    // (see FileState::DoneNeedsRender). Every other case (RAW that
                    // already got the full render, or any other format's own tiers)
                    // is a plain Done.
                    FileState newState = (pt.fmt == Format::Raw && pt.result.tier == ThumbTier::EmbeddedPreview)
                                              ? FileState::DoneNeedsRender
                                              : FileState::Done;
                    auto updFile = db_.prepare(
                        "UPDATE files SET width=?, height=?, orientation=?, thumb_id=?, state=?, "
                        "gps_lat=?, gps_lon=?, gps_checked=? WHERE id=?");
                    updFile.bind(1, fileWidth);
                    updFile.bind(2, fileHeight);
                    updFile.bind(3, (int64_t)pt.result.orientation);
                    updFile.bind(4, thumbId);
                    updFile.bind(5, (int64_t)newState);
                    // NULL rather than 0,0 when there are no coordinates - (0,0) is a real
                    // place in the Gulf of Guinea, and the marker must not light up for it.
                    if (pt.result.hasGps) {
                        updFile.bindDouble(6, pt.result.gpsLatitude);
                        updFile.bindDouble(7, pt.result.gpsLongitude);
                    } else {
                        updFile.bindNull(6);
                        updFile.bindNull(7);
                    }
                    // Only claim the file was checked if the generator actually got far
                    // enough to look; leaving it 0 otherwise lets the backfill sweep pick it
                    // up later rather than recording a false "no GPS here".
                    updFile.bind(8, (int64_t)(pt.result.gpsChecked ? 1 : 0));
                    updFile.bind(9, pt.fileId);
                    updFile.step();

                    if (pt.oldThumbId != 0) {
                        auto delThumb = db_.prepare("DELETE FROM thumbs.thumbs WHERE id=?");
                        delThumb.bind(1, pt.oldThumbId);
                        delThumb.step();
                    }

                    if (pt.result.tier == ThumbTier::EmbeddedPreview) stats.thumbsEmbedded++;
                    else stats.thumbsDecoded++;
                    break;
                }
                // Nothing was attempted - the process is quitting (see util/Shutdown.h).
                // Write nothing at all: the row stays state=New, which is where an
                // un-thumbnailed file normally sits, so the next scan picks it up as if
                // this run had simply never reached it. Recording anything here would
                // permanently condemn a folder's worth of good files for the crime of
                // being in the wave that was in flight when the user closed the window.
                case ThumbTier::Cancelled: break;
                case ThumbTier::Unsupported:
                case ThumbTier::Failed: {
                    auto updFile = db_.prepare("UPDATE files SET state=? WHERE id=?");
                    updFile.bind(1, (int64_t)(pt.result.tier == ThumbTier::Unsupported ? FileState::Unsupported
                                                                                        : FileState::Failed));
                    updFile.bind(2, pt.fileId);
                    updFile.step();

                    if (pt.result.tier == ThumbTier::Unsupported) stats.thumbsUnsupported++;
                    else stats.thumbsFailed++;
                    break;
                }
            }
        }
        db_.commit();
        batch.clear();
        claims_.heartbeat(dirId, opts_.owner, nowMillis());
        if (callbacks.onProgress) callbacks.onProgress(stats);
    };

    // Pass B dispatches in *waves* and commits in *batches*, and those are two
    // different sizes for two different reasons.
    //
    // A wave is how many generateThumb() calls are in flight at once - they're
    // submitted to pool_ concurrently and collected together (see ThreadPool.h). It is
    // also how often onWantFirst gets re-asked, since a wave boundary is the only point
    // at which the remaining work can be reordered. So with that hook installed a wave
    // is the pool's own thread count: the smallest wave that still keeps every thread
    // busy, and therefore the tightest the queue can track a scrolling grid. With no
    // hook there is nothing to re-ask and nothing to reorder, so a wave is simply a
    // whole batch.
    //
    // A batch is how many results go into one transaction. Commits aren't free, so this
    // stays at kBatchSize however small the waves get. --render-raws is the deliberate
    // exception and commits every wave: a real demosaic decode is slow enough that
    // batching 63 more in before reporting progress would defeat wanting to *watch* a
    // large RAW folder render rather than wait for it in one lump. That's applied to the
    // whole run once renderRaws is set, not per-item file format (a --render-raws run's
    // pending list can still include ordinary new JPEGs) - a little extra, still-cheap
    // transaction overhead on those beats reasoning about per-item batch boundaries
    // under concurrent dispatch.
    const size_t poolWave = std::max<size_t>(1, std::min(kBatchSize, resolveThreadCount(opts_.threadCount)));
    const size_t waveCap = (opts_.renderRaws || callbacks.onWantFirst) ? poolWave : kBatchSize;

    // Commit granularity ramps rather than being one number, because the first commit and
    // the hundredth are worth completely different things.
    //
    // Nothing reaches the screen until a batch commits, so a flat kBatchSize means the user
    // waits for 64 files before seeing *any* thumbnail - and the on-screen-first ordering
    // above buys nothing, because the screenful it carefully generated first sits
    // uncommitted behind 44 more files nobody is looking at. Starting at one wave makes the
    // first paint land as soon as the first screenful exists.
    //
    // Doubling from there back up to kBatchSize keeps the steady state cheap: a long folder
    // still ends up committing in 64s, so this costs a handful of extra transactions at the
    // start of a directory rather than 3x of them throughout. --render-raws stays pinned at
    // 1 - a real demosaic is slow enough that batching at all defeats watching it progress.
    size_t flushAfter = opts_.renderRaws ? 1 : waveCap;
    const size_t flushCeiling = opts_.renderRaws ? 1 : kBatchSize;

    // The last answer onWantFirst gave. Re-sorting the remaining work when nothing has
    // moved is pure overhead - on a 1280-file folder with 8-wide waves that would be
    // ~160 sorts of ~1300 rows each - and the answer only changes when the user scrolls,
    // which is rare next to a wave. Skipping is safe because sorting [from, end) and
    // then advancing `from` leaves the rest of the range already in the order asked for.
    std::vector<int64_t> lastWanted;

    for (size_t waveStart = 0; waveStart < pending.size(); waveStart += waveCap) {
        // Asked *before* dispatching, first wave included - the whole point is that a
        // cold folder's first wave is the screenful the user is looking at, not whatever
        // readdir happened to return first.
        if (callbacks.onWantFirst) {
            std::vector<int64_t> remaining;
            remaining.reserve(pending.size() - waveStart);
            for (size_t j = waveStart; j < pending.size(); ++j) remaining.push_back(pending[j].fileId);

            std::vector<int64_t> wanted = callbacks.onWantFirst(dirId, dirPath, remaining);
            if (wanted != lastWanted) {
                movePendingToFront(pending, waveStart, wanted);
                lastWanted = std::move(wanted);
            }
        }

        size_t waveEnd = std::min(pending.size(), waveStart + waveCap);

        std::vector<std::future<ThumbResult>> futures;
        futures.reserve(waveEnd - waveStart);
        for (size_t j = waveStart; j < waveEnd; ++j) {
            Format fmt = classifyFormat(pending[j].name);
            // Also forces a fresh (state=New) RAW file straight to a full render
            // during a --render-raws run - see IndexOptions::renderRaws.
            bool forceFullRender = opts_.renderRaws && fmt == Format::Raw;
            std::string filePath = joinPath(dirPath, pending[j].name);
            int targetLongEdge = opts_.targetLongEdge;
            int quality = opts_.quality;
            futures.push_back(pool_.submit([filePath, fmt, targetLongEdge, quality, forceFullRender]() {
                PIXET_PROF_SCOPE("idx.generateThumb");
                return generateThumb(filePath, fmt, targetLongEdge, quality, forceFullRender);
            }));
        }

        for (size_t j = waveStart; j < waveEnd; ++j) {
            const PendingRow &row = pending[j];
            batch.push_back({row.fileId, classifyFormat(row.name), row.existingThumbId, futures[j - waveStart].get()});
        }
        if (batch.size() >= flushAfter || waveEnd == pending.size()) {
            PIXET_PROF_SCOPE("idx.passB.flushBatch");
            flushBatch();
            flushAfter = std::min(flushCeiling, flushAfter * 2);
        }

        // After the flush, so a cancelled run still commits the wave it just finished
        // rather than throwing that work away - see IndexCallbacks::shouldCancel.
        // shutdownRequested() is checked alongside the caller's own hook rather than left
        // to it: pixet-index and the tests install no shouldCancel at all, and a quit has
        // to stop this loop regardless of who asked for the run.
        if (shutdownRequested() || (callbacks.shouldCancel && callbacks.shouldCancel())) {
            if (!batch.empty()) flushBatch();
            break;
        }
    }

    // After Pass B, so files it just thumbnailed already have GPS written from the bytes
    // that were in memory at the time - this only touches what's genuinely still unchecked.
    {
        PIXET_PROF_SCOPE("idx.backfillGps");
        backfillGps(dirId, dirPath, stats);
    }

    guard.dismissed = true;
    claims_.release(dirId, opts_.owner);
}

void Indexer::backfillGps(int64_t dirId, const std::string &dirPath, IndexStats &stats) {
    // JPEG only, matching what parseJpegExifDetails can actually read. A HEIC or RAW file is
    // left at gps_checked = 0 rather than being recorded as "checked, no coordinates", so it
    // gets picked up for free if those formats ever gain GPS support.
    std::vector<std::pair<int64_t, std::string>> todo;
    {
        auto sel = db_.prepare("SELECT id, name FROM files WHERE dir_id=? AND gps_checked=0 AND fmt=?");
        sel.bind(1, dirId);
        sel.bind(2, (int64_t)Format::Jpeg);
        while (sel.step()) todo.emplace_back(sel.columnInt64(0), sel.columnText(1));
    }
    if (todo.empty()) return;

    db_.beginTransaction();
    for (const auto &[fileId, name] : todo) {
        // A bounded prefix, not the whole file: EXIF lives in the header, so this reads tens
        // of KB per photo rather than the several MB it occupies. That difference is what
        // makes backfilling an entire library cheap enough to do inline here.
        std::vector<uint8_t> prefix;
        ExifDetails details;
        if (readFilePrefix(joinPath(dirPath, name), kExifPrefixBytes, prefix) && !prefix.empty()) {
            details = parseJpegExifDetails(prefix.data(), prefix.size());
        }

        auto upd = db_.prepare("UPDATE files SET gps_lat=?, gps_lon=?, gps_checked=1 WHERE id=?");
        if (details.hasGps) {
            // NULL rather than 0,0 when absent - (0,0) is a real location in the Gulf of
            // Guinea, and the grid's marker keys off the column being non-NULL.
            upd.bindDouble(1, details.gpsLatitude);
            upd.bindDouble(2, details.gpsLongitude);
            stats.gpsBackfilled++;
        } else {
            upd.bindNull(1);
            upd.bindNull(2);
        }
        // Marked checked even when the read failed or found nothing, or every future scan
        // would retry the same files forever. A file that actually changes gets a fresh
        // thumbnail pass anyway, which rewrites this from the full bytes.
        upd.bind(3, fileId);
        upd.step();
    }
    db_.commit();
}

void Indexer::run(const std::string &rootPath, IndexStats &stats, const IndexCallbacks &callbacks) {
    int64_t rootId = upsertDir(db_, rootPath, -1);

    // This is a plain worklist, so anything that makes the directory graph cyclic turns it
    // into a loop that keeps inserting dirs rows until the disk fills. DirWalker's macOS
    // implementation refuses to report a symlinked directory as a directory at all (see its
    // AT_SYMLINK_NOFOLLOW comment), which covers the common shape - /tmp -> /private/tmp and
    // friends - but two cheap guards here cover what that can't.
    //
    // What each one actually buys, stated honestly:
    //  - `visited` catches a directory reached twice by the *same* path string. That's the
    //    realistic repeat case, and it's free.
    //  - kMaxDepth bounds everything else. It does NOT detect a directory reached by two
    //    different path strings (macOS firmlinks like /System/Volumes/Data, bind mounts, or
    //    hardlinked directories) - those would still be indexed twice under two paths. Doing
    //    that properly needs device+inode identity, which is a bigger change than a port
    //    should carry (file identity here is deliberately (dir_id, name), not inode). The
    //    depth cap means the failure mode is "some duplicate rows" rather than "fills the
    //    disk", which is the part that actually matters.
    // 64 is far deeper than any real photo library; a tree legitimately deeper than this
    // stops being descended, silently by design rather than by oversight.
    constexpr int kMaxDepth = 64;

    std::unordered_set<std::string> visited;
    visited.insert(rootPath);

    std::vector<std::tuple<int64_t, std::string, int>> queue; // (dirId, path, depth)
    queue.emplace_back(rootId, rootPath, 0);

    while (!queue.empty()) {
        // Also checked here, not just between Pass B waves: a recursive run over a large
        // tree spends real time in Pass A on directories that are already fresh, and those
        // never reach a wave boundary to be cancelled at.
        if (shutdownRequested() || (callbacks.shouldCancel && callbacks.shouldCancel())) break;

        auto [dirId, dirPath, depth] = queue.back();
        queue.pop_back();

        std::vector<std::pair<int64_t, std::string>> subdirs;
        // One bad directory must not end the run, and must never escape to the caller.
        //
        // Bare, the consequence is severe out of all proportion to the cause: a single failed
        // COMMIT throws std::runtime_error all the way out through run(), out of
        // BackgroundReconciler::sweepNext()'s timer slot, and - with no handler anywhere on
        // that QThread - into std::terminate(), killing the whole application. Observed in
        // the field on macOS, where a background hygiene sweep doing work nobody asked for
        // took the app down with it.
        //
        // indexOneDirectory() rolls back and releases its claim before rethrowing, so by the
        // time we get here this directory has left no lock or open transaction behind and the
        // next one can proceed on the same connection.
        try {
            indexOneDirectory(dirId, dirPath, subdirs, stats, callbacks);
        } catch (const std::exception &e) {
            stats.dirsFailed++;
            if (stats.firstFailure.empty()) stats.firstFailure = e.what();
            subdirs.clear(); // whatever it managed to collect isn't trustworthy
        }
        stats.dirsVisited++;
        if (callbacks.onProgress) callbacks.onProgress(stats);

        if (opts_.recursive && depth < kMaxDepth) {
            for (auto &sd : subdirs) {
                if (!visited.insert(sd.second).second) continue; // already walked this path
                queue.emplace_back(sd.first, std::move(sd.second), depth + 1);
            }
        }
    }
}

} // namespace pixet
