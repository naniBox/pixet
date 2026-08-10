#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pixet {

struct DirEntry {
    std::string name; // just the entry name, not a full path - UTF-8
    bool isDir = false;
    int64_t size = 0;
    int64_t mtimeUnix = 0;
};

// Lists the immediate contents of one directory (non-recursive). Skips "." and "..".
// `path` is UTF-8. Throws std::runtime_error if the directory can't be opened.
std::vector<DirEntry> listDir(const std::string &path);

// Directory's own mtime (entries added/removed/renamed change this; editing an
// existing file's content in place does not). Used for the cheap "is this folder
// still fresh?" check before deciding whether to rescan it. `path` is UTF-8.
int64_t dirMtimeUnix(const std::string &path);

} // namespace pixet
