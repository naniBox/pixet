#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include <atomic>

#include "../util/FileMove.h"

namespace pixet {

class Database;

namespace fileops {

enum class OpKind { Copy, Move };

// How a name collision at the destination was resolved - decided entirely before
// execute() is ever called (see the collision pre-flight in FileOpsWorker/
// CollisionDialog, app-layer). execute() itself only consumes the *result* of that
// decision via PlannedItem::dstName/replaceExisting - Skip simply means an item never
// becomes part of Plan::items at all. Kept here anyway as the shared vocabulary
// between the app-layer dialog and whatever built the Plan.
enum class Collision { Replace, Skip, KeepBoth };

struct PlannedItem {
    std::string srcPath; // absolute, normalizePath()'d, UTF-8
    std::string dstName; // final name in Plan::dstDirPath - already collision-resolved
    int64_t srcFileId = 0; // 0 = source isn't a row pixet's index knows about (external drop/paste)
    int64_t srcDirId = 0;  // 0 = unknown/not indexed
    bool replaceExisting = false; // dstName already exists there and the user chose Replace
};

struct Plan {
    OpKind kind = OpKind::Copy;
    std::string dstDirPath; // absolute, normalized, UTF-8
    std::vector<PlannedItem> items;
};

struct ItemOutcome {
    std::string srcPath;
    std::string dstName;
    int64_t srcFileId = 0; // echoed so a caller can drop that grid row on a successful move
    int64_t dstFileId = 0; // the files row now representing the destination (0 if none)
    bool ok = false;
    bool thumbPreserved = false; // true only on the smart-move path (see execute())
    FsResult fsResult = FsResult::Ok;
    std::string error; // human-readable, empty on success
};

struct Progress {
    size_t done = 0;
    size_t total = 0;
    std::string currentName;
};

struct Report {
    std::vector<ItemOutcome> outcomes;
    size_t succeeded = 0;
    size_t failed = 0;
    bool cancelled = false;
};

// ASCII-only case-insensitive ordering (matches the target filesystems' own default
// case-insensitivity) - deliberately not full Unicode case-folding, which is a much
// bigger and locale-sensitive problem (e.g. Turkish İ/i) this doesn't need to solve.
struct CaseInsensitiveLess {
    bool operator()(const std::string &a, const std::string &b) const;
};

// Finds the first "name (2)", "name (3)", ... (last-dot extension split, matching
// Schema.cpp's classifyFormat() convention - so "a.tar.gz" -> "a.tar (2).gz", and a
// leading-dot dotfile with no other dot, e.g. ".DS_Store", keeps its name intact as
// the stem: ".DS_Store (2)") that is neither present on disk in `dirPath` nor already
// claimed by `alsoTaken` (names earlier items in the same batch have taken - checked
// case-insensitively, since both target filesystems are case-insensitive by default).
// Returns an empty string if no free name is found within a bounded number of tries.
std::string uniqueNameFor(const std::string &dirPath, const std::string &name,
                           const std::set<std::string, CaseInsensitiveLess> &alsoTaken);

// Executes `plan` item by item; each item is independent, so one failure doesn't
// abort the batch - a large batch across a network share or a full disk is expected
// to partially fail sometimes, and every earlier successful item stays done. Never
// throws: any DB exception for a given item is caught and recorded as that item's
// failure, and the batch continues with the next one. `owner` is used as the claim
// owner id for the source/destination directories (best-effort - see the .cpp file
// comment on why a claim conflict here is never treated as fatal). `onProgress`, if
// given, is called after every item; `cancel`, if given, is polled before each item
// (a batch already in flight for one item always finishes that item first).
Report execute(Database &db, const Plan &plan, const std::string &owner,
               const std::function<void(const Progress &)> &onProgress = nullptr,
               const std::atomic_bool *cancel = nullptr);

} // namespace fileops
} // namespace pixet
