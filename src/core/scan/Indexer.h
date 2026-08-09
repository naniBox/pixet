#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "Claims.h"

namespace pixet {

class Database;

struct IndexStats {
    int64_t dirsVisited = 0;
    int64_t dirsSkippedClaimed = 0; // held by another owner with a fresh heartbeat
    int64_t dirsSkippedFresh = 0;   // mtime unchanged since last scan, trusted cache
    int64_t filesNew = 0;
    int64_t filesRemoved = 0;
    int64_t thumbsEmbedded = 0;   // ThumbTier::EmbeddedPreview
    int64_t thumbsDecoded = 0;    // ThumbTier::Decoded
    int64_t thumbsUnsupported = 0;
    int64_t thumbsFailed = 0;
};

struct IndexOptions {
    bool recursive = true;
    bool forceRescan = false; // bypass the dir-mtime-unchanged skip
    // Unconditionally re-thumbnails every file in the directory, even ones whose
    // (mtime, size) haven't changed since the last scan - forceRescan alone only
    // catches new/changed/removed files, deliberately, so a plain Refresh stays cheap
    // on a large folder. This is the heavier "no really, redo everything" escape
    // hatch (e.g. after a Pass B bug fix changed what gets extracted). Implies
    // forceRescan.
    bool forceRethumbnail = false;
    // RAW only: replaces every RAW file's embedded-preview-derived thumbnail
    // (FileState::DoneNeedsRender) with one from a full demosaic render of the actual
    // sensor data - the expensive path the embedded-preview-first ladder exists to
    // avoid by default. Deliberately a separate, explicit opt-in (`pixet-index
    // --render-raws`) rather than something Pass B does automatically: unlike
    // forceRethumbnail (a blunt "redo everything" escape hatch), this is a targeted,
    // recurring "catch up whatever's still on the fast path" pass meant to be run
    // periodically after normal (fast) indexing, not a one-off recovery tool. Also
    // forces any brand-new (state=New) RAW file encountered during this same run
    // straight to a full render rather than the normal embedded-preview-first
    // attempt - if the user is explicitly asking for real renders right now, a
    // freshly-discovered RAW file shouldn't get the fast placeholder either.
    bool renderRaws = false;
    int targetLongEdge = 320;
    int quality = 85;
    std::string owner; // claim owner id, e.g. "pid:1234"
};

struct IndexCallbacks {
    // Fired once per directory, right after Pass A (walk+diff) commits - the file
    // list (names) for that directory is final from this point on, even though most
    // or all thumbnails are still pending. Lets a GUI show filenames immediately
    // instead of waiting for Pass B to finish. Also fires for an already-fresh
    // directory (nothing to do), so callers don't need to special-case that.
    std::function<void(int64_t dirId, const std::wstring &dirPath)> onFilesListed;

    // Fired after each Pass B batch commits (a handful of thumbnails just became
    // available) and once per directory visited overall. A GUI can use this to pull
    // newly-ready thumbnails into view incrementally rather than in one final jump.
    std::function<void(const IndexStats &)> onProgress;
};

// Walks and thumbnails a directory tree, reusing the exact same code path the GUI's
// on-demand FolderIndexer will call for a single folder (see devlog/plan) - this
// class is the one place Pass A (walk+diff) and Pass B (thumbnail state=0 files)
// actually live.
class Indexer {
public:
    Indexer(Database &db, IndexOptions opts);

    void run(const std::wstring &rootPath, IndexStats &stats, const IndexCallbacks &callbacks = {});

private:
    Database &db_;
    IndexOptions opts_;
    ClaimManager claims_;

    int64_t upsertDir(const std::wstring &path, int64_t parentId);
    void indexOneDirectory(int64_t dirId, const std::wstring &dirPath,
                            std::vector<std::pair<int64_t, std::wstring>> &subdirsOut, IndexStats &stats,
                            const IndexCallbacks &callbacks);
};

} // namespace pixet
