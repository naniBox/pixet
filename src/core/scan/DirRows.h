#pragma once

#include <cstdint>
#include <string>

namespace pixet {

class Database;

// Ensures a `dirs` row exists for `path` (inserting one, parented at `parentId`, if
// not) and returns its id. `parentId` < 0 means no parent (a scan root, or - for the
// fileops smart-move path - any directory reached without walking down from a known
// parent). Shared by Indexer (which had this as a private method until file-ops
// needed the exact same "give me this directory's id, creating the row if this is
// the first time we've ever heard of it" operation) so there's exactly one
// implementation of the INSERT-OR-IGNORE-then-SELECT pair.
int64_t upsertDir(Database &db, const std::string &path, int64_t parentId);

struct PruneStats {
    int64_t dirsBarren = 0;   // held no media and had no surviving children
    int64_t dirsMissing = 0;  // the directory itself is provably gone from disk
    int64_t filesRemoved = 0; // rows left behind by a removed directory, or previously orphaned
    int64_t thumbsRemoved = 0;
};

// Removes `dirs` rows that no longer describe anything worth keeping, and everything
// orphaned behind them. Two distinct kinds of junk:
//
//  - **Barren rows.** A `dirs` row used to be written for every subdirectory merely
//    *seen* during a walk, not only those actually indexed, and nothing ever removed
//    one. Since BackgroundReconciler's worklist was "every row in dirs", one visit to a
//    filesystem root was enough to enlist the whole disk: the sweep listed each
//    directory, which registered its children, which the next pass swept in turn. A real
//    library reached 41,349 rows for 7,264 media files, 99.4% of them holding no media
//    at all, expanding by one directory level per pass, forever - and prompting for TCC
//    access to Downloads/Documents/Desktop each time the rotation reached one.
//    Indexer no longer registers a directory it isn't going to index, but that only
//    stops the growth; this is what clears what's already there.
//  - **Missing rows.** Directories deleted since they were indexed. Their `files` rows
//    and thumbnail blobs were previously kept forever.
//
// Barren rows go first and by pure SQL, because that is the bulk of the work and needs
// no I/O; only the survivors are then stat'd. That ordering matters on macOS beyond
// speed - statting tens of thousands of paths across the whole disk is itself a way to
// trip the TCC prompts this exists to stop.
//
// A directory is *never* removed for being unreachable, only for being provably absent -
// see DirPresence. A row with a live claim is left alone whatever its state, since
// something is working in it right now.
PruneStats pruneDirs(Database &db);

} // namespace pixet
