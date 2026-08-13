#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "Claims.h"
#include "../util/ThreadPool.h"

namespace pixet {

class Database;

struct IndexStats {
    int64_t dirsVisited = 0;
    int64_t dirsSkippedClaimed = 0; // held by another owner with a fresh heartbeat
    int64_t dirsSkippedFresh = 0;   // mtime unchanged since last scan, trusted cache
    // Couldn't be stat'd or listed at all - permission denied, or it vanished mid-walk.
    // Counted rather than silently swallowed because on macOS this is *routine*, not
    // exceptional: TCC gates ~/Documents, ~/Downloads and ~/Desktop behind a per-app
    // grant, and a run over a whole home directory will legitimately skip folders. A
    // number the user can see is the difference between "working as designed" and
    // "why is half my library missing".
    int64_t dirsSkippedUnreadable = 0;
    // Files whose EXIF GPS was filled in this run by the backfill pass (see
    // Indexer::backfillGps) - i.e. files scanned before GPS was extracted at all. Counts
    // only rows that gained coordinates, not every file examined, since that's what
    // actually changes what the UI shows.
    int64_t gpsBackfilled = 0;
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

    // How many worker threads Pass B (thumbnail generation) spreads across - see
    // ThreadPool.h and Indexer::indexOneDirectory()'s Pass B. 0 = auto-detect
    // (std::thread::hardware_concurrency(), floored at 1). Deliberately only Pass B:
    // Pass A (the directory walk/diff) and every DB write stay on the single thread
    // that owns the Database connection - see Database.h's own "not thread-safe"
    // contract - this only parallelizes generateThumb(), which is a pure,
    // side-effect-free function (see ThumbGenerator.h).
    int threadCount = 0;
};

struct IndexCallbacks {
    // Fired once per directory, right after Pass A (walk+diff) commits - the file
    // list (names) for that directory is final from this point on, even though most
    // or all thumbnails are still pending. Lets a GUI show filenames immediately
    // instead of waiting for Pass B to finish. Also fires for an already-fresh
    // directory (nothing to do), so callers don't need to special-case that.
    std::function<void(int64_t dirId, const std::string &dirPath)> onFilesListed;

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

    void run(const std::string &rootPath, IndexStats &stats, const IndexCallbacks &callbacks = {});

private:
    Database &db_;
    IndexOptions opts_;
    ClaimManager claims_;
    // Sized once from opts_.threadCount (resolved in the constructor) and reused across
    // every directory's Pass B for this run() call - see ThreadPool.h on why persistent
    // beats spawning fresh threads per directory/batch.
    ThreadPool pool_;

    void indexOneDirectory(int64_t dirId, const std::string &dirPath,
                            std::vector<std::pair<int64_t, std::string>> &subdirsOut, IndexStats &stats,
                            const IndexCallbacks &callbacks);

    // Fills in files.gps_lat/gps_lon/gps_checked for files in this directory that nothing has
    // looked at yet - i.e. rows written before GPS was extracted during thumbnailing at all.
    //
    // Lives here rather than in one caller so *every* path that indexes a folder gets it: an
    // on-demand navigation, the background reconciler's sweep, and pixet-index alike. Putting
    // it only in the background sweep would mean a folder's geotag markers didn't appear
    // until that sweep reached it, which on a large library is many minutes after the user
    // opened the folder.
    //
    // Cheap enough to sit on this path: EXIF is in the file header, so this reads a bounded
    // prefix per file rather than the whole image, and each file is only ever examined once.
    void backfillGps(int64_t dirId, const std::string &dirPath, IndexStats &stats);
};

} // namespace pixet
