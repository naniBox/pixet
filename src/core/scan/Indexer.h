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
    int targetLongEdge = 320;
    int quality = 85;
    std::string owner; // claim owner id, e.g. "pid:1234"
};

// Walks and thumbnails a directory tree, reusing the exact same code path the GUI's
// on-demand FolderIndexer will call for a single folder (see devlog/plan) - this
// class is the one place Pass A (walk+diff) and Pass B (thumbnail state=0 files)
// actually live.
class Indexer {
public:
    Indexer(Database &db, IndexOptions opts);

    void run(const std::wstring &rootPath, IndexStats &stats,
              const std::function<void(const IndexStats &)> &onProgress = {});

private:
    Database &db_;
    IndexOptions opts_;
    ClaimManager claims_;

    int64_t upsertDir(const std::wstring &path, int64_t parentId);
    void indexOneDirectory(int64_t dirId, const std::wstring &dirPath,
                            std::vector<std::pair<int64_t, std::wstring>> &subdirsOut, IndexStats &stats);
};

} // namespace pixet
