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

} // namespace pixet
