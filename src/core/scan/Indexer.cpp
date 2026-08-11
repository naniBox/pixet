#include "Indexer.h"

#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "../db/Database.h"
#include "../thumb/ThumbGenerator.h"
#include "../util/PathUtil.h"
#include "../util/Time.h"
#include "DirWalker.h"

namespace pixet {

namespace {

constexpr size_t kBatchSize = 64;

struct ExistingFile {
    int64_t id;
    int64_t mtime;
    int64_t size;
    int64_t thumbId; // 0 = none
};

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

Indexer::Indexer(Database &db, IndexOptions opts) : db_(db), opts_(std::move(opts)), claims_(db) {}

int64_t Indexer::upsertDir(const std::string &path, int64_t parentId) {
    auto ins = db_.prepare("INSERT OR IGNORE INTO dirs(parent_id, path, mtime, scanned_at) VALUES(?,?,0,0)");
    if (parentId < 0) ins.bindNull(1); else ins.bind(1, parentId);
    ins.bind(2, path);
    ins.step();

    auto sel = db_.prepare("SELECT id FROM dirs WHERE path=?");
    sel.bind(1, path);
    sel.step();
    return sel.columnInt64(0);
}

void Indexer::indexOneDirectory(int64_t dirId, const std::string &dirPath,
                                 std::vector<std::pair<int64_t, std::string>> &subdirsOut, IndexStats &stats,
                                 const IndexCallbacks &callbacks) {
    int64_t nowMs = nowMillis();
    if (!claims_.tryClaim(dirId, opts_.owner, nowMs)) {
        stats.dirsSkippedClaimed++;
        return;
    }

    // dirMtimeUnix throws if the directory can't be stat'd, and this call used to be bare -
    // which meant one unreadable folder threw straight out through run() (also unguarded),
    // aborting the entire index run *and* leaking the claim row taken just above until its
    // heartbeat went stale. Tolerable on Windows, where an unreadable folder in a photo
    // tree is unusual; not on macOS, where TCC-protected directories are everywhere and
    // "index my home folder" would reliably die on the first one. Handled the same way the
    // listDir failure below already was: release the claim, count it, skip the folder.
    int64_t actualMtime = 0;
    try {
        actualMtime = dirMtimeUnix(dirPath);
    } catch (const std::exception &) {
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
        auto sel = db_.prepare("SELECT id, path FROM dirs WHERE parent_id=?");
        sel.bind(1, dirId);
        while (sel.step()) {
            subdirsOut.emplace_back(sel.columnInt64(0), sel.columnText(1));
        }
    } else {
        std::vector<DirEntry> entries;
        try {
            entries = listDir(dirPath);
        } catch (const std::exception &) {
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

        db_.beginTransaction();

        for (const auto &entry : entries) {
            if (entry.isDir) {
                subdirsOut.emplace_back(upsertDir(joinPath(dirPath, entry.name), dirId), joinPath(dirPath, entry.name));
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

    std::vector<std::tuple<int64_t, std::string, int64_t>> pending; // (fileId, name, existingThumbId)
    {
        auto sel = db_.prepare(pendingSql);
        sel.bind(1, dirId);
        while (sel.step()) {
            int64_t existingThumbId = sel.columnIsNull(3) ? 0 : sel.columnInt64(3);
            pending.emplace_back(sel.columnInt64(0), sel.columnText(1), existingThumbId);
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
                        "UPDATE files SET width=?, height=?, orientation=?, thumb_id=?, state=? WHERE id=?");
                    updFile.bind(1, fileWidth);
                    updFile.bind(2, fileHeight);
                    updFile.bind(3, (int64_t)pt.result.orientation);
                    updFile.bind(4, thumbId);
                    updFile.bind(5, (int64_t)newState);
                    updFile.bind(6, pt.fileId);
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

    for (auto &[fileId, name, existingThumbId] : pending) {
        Format fmt = classifyFormat(name);
        // Also forces a fresh (state=New) RAW file straight to a full render during a
        // --render-raws run - see IndexOptions::renderRaws.
        bool forceFullRender = opts_.renderRaws && fmt == Format::Raw;
        ThumbResult result =
            generateThumb(joinPath(dirPath, name), fmt, opts_.targetLongEdge, opts_.quality, forceFullRender);
        batch.push_back({fileId, fmt, existingThumbId, std::move(result)});
        // A forced full RAW render is slow enough (real demosaic decode, not a cheap
        // embedded-preview extraction) that batching it in with up to 63 more before
        // the caller hears about it would defeat the point of wanting to *watch*
        // progress on a large RAW folder rather than wait for it in one lump - flush
        // every such item immediately instead of only at the usual batch size.
        if (batch.size() >= kBatchSize || forceFullRender) flushBatch();
    }
    flushBatch();

    claims_.release(dirId, opts_.owner);
}

void Indexer::run(const std::string &rootPath, IndexStats &stats, const IndexCallbacks &callbacks) {
    int64_t rootId = upsertDir(rootPath, -1);

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
        auto [dirId, dirPath, depth] = queue.back();
        queue.pop_back();

        std::vector<std::pair<int64_t, std::string>> subdirs;
        indexOneDirectory(dirId, dirPath, subdirs, stats, callbacks);
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
