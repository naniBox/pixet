#include "Indexer.h"

#include <unordered_map>

#include "../db/Database.h"
#include "../thumb/ThumbGenerator.h"
#include "../util/StringUtil.h"
#include "../util/Time.h"
#include "DirWalker.h"

namespace pixet {

namespace {

std::wstring joinPath(const std::wstring &dir, const std::wstring &name) {
    if (!dir.empty() && dir.back() == L'\\') return dir + name;
    return dir + L'\\' + name;
}

constexpr size_t kBatchSize = 64;

struct ExistingFile {
    int64_t id;
    int64_t mtime;
    int64_t size;
    int64_t thumbId; // 0 = none
};

struct PendingThumb {
    int64_t fileId;
    ThumbResult result;
};

} // namespace

Indexer::Indexer(Database &db, IndexOptions opts) : db_(db), opts_(std::move(opts)), claims_(db) {}

int64_t Indexer::upsertDir(const std::wstring &path, int64_t parentId) {
    std::string pathUtf8 = toUtf8(path);

    auto ins = db_.prepare("INSERT OR IGNORE INTO dirs(parent_id, path, mtime, scanned_at) VALUES(?,?,0,0)");
    if (parentId < 0) ins.bindNull(1); else ins.bind(1, parentId);
    ins.bind(2, pathUtf8);
    ins.step();

    auto sel = db_.prepare("SELECT id FROM dirs WHERE path=?");
    sel.bind(1, pathUtf8);
    sel.step();
    return sel.columnInt64(0);
}

void Indexer::indexOneDirectory(int64_t dirId, const std::wstring &dirPath,
                                 std::vector<std::pair<int64_t, std::wstring>> &subdirsOut, IndexStats &stats,
                                 const IndexCallbacks &callbacks) {
    int64_t nowMs = nowMillis();
    if (!claims_.tryClaim(dirId, opts_.owner, nowMs)) {
        stats.dirsSkippedClaimed++;
        return;
    }

    int64_t actualMtime = dirMtimeUnix(dirPath);

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
            subdirsOut.emplace_back(sel.columnInt64(0), toUtf16(sel.columnText(1)));
        }
    } else {
        std::vector<DirEntry> entries;
        try {
            entries = listDir(dirPath);
        } catch (const std::exception &) {
            claims_.release(dirId, opts_.owner);
            return; // directory vanished / permission denied mid-walk - just skip it
        }

        std::unordered_map<std::wstring, ExistingFile> existing;
        {
            auto sel = db_.prepare("SELECT id, name, mtime, size, thumb_id FROM files WHERE dir_id=?");
            sel.bind(1, dirId);
            while (sel.step()) {
                ExistingFile ef;
                ef.id = sel.columnInt64(0);
                std::wstring name = toUtf16(sel.columnText(1));
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
                ins.bind(2, toUtf8(entry.name));
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

    // Pass B: thumbnail everything still pending in this directory.
    std::vector<std::pair<int64_t, std::wstring>> pending; // (fileId, name)
    {
        auto sel = db_.prepare("SELECT id, name, fmt FROM files WHERE dir_id=? AND state=0");
        sel.bind(1, dirId);
        while (sel.step()) {
            pending.emplace_back(sel.columnInt64(0), toUtf16(sel.columnText(1)));
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
                    auto updFile = db_.prepare(
                        "UPDATE files SET width=?, height=?, orientation=?, thumb_id=?, state=1 WHERE id=?");
                    updFile.bind(1, fileWidth);
                    updFile.bind(2, fileHeight);
                    updFile.bind(3, (int64_t)pt.result.orientation);
                    updFile.bind(4, thumbId);
                    updFile.bind(5, pt.fileId);
                    updFile.step();

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

    for (auto &[fileId, name] : pending) {
        Format fmt = classifyFormat(name);
        ThumbResult result = generateThumb(joinPath(dirPath, name), fmt, opts_.targetLongEdge, opts_.quality);
        batch.push_back({fileId, std::move(result)});
        if (batch.size() >= kBatchSize) flushBatch();
    }
    flushBatch();

    claims_.release(dirId, opts_.owner);
}

void Indexer::run(const std::wstring &rootPath, IndexStats &stats, const IndexCallbacks &callbacks) {
    int64_t rootId = upsertDir(rootPath, -1);

    std::vector<std::pair<int64_t, std::wstring>> queue;
    queue.emplace_back(rootId, rootPath);

    while (!queue.empty()) {
        auto [dirId, dirPath] = queue.back();
        queue.pop_back();

        std::vector<std::pair<int64_t, std::wstring>> subdirs;
        indexOneDirectory(dirId, dirPath, subdirs, stats, callbacks);
        stats.dirsVisited++;
        if (callbacks.onProgress) callbacks.onProgress(stats);

        if (opts_.recursive) {
            for (auto &sd : subdirs) queue.push_back(std::move(sd));
        }
    }
}

} // namespace pixet
