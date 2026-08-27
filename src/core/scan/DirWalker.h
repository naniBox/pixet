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

// Whether a directory is still there, distinguishing the two failure modes that a
// single "does it exist?" bool would flatten into one. Nothing else in this codebase
// needs to tell them apart; pruneDirs() does, and getting it wrong destroys data.
//
// `Missing` means the directory is provably gone - the name resolved all the way and
// nothing was there. `Unreadable` means the answer is unknown: permission was denied
// somewhere along the path, the volume isn't mounted, the network share is down. On
// macOS that is the *normal* state of a TCC-protected folder the user hasn't granted
// access to, so treating it as Missing would delete a perfectly good index for a
// folder that is sitting right there.
enum class DirPresence { Present, Missing, Unreadable };
DirPresence dirPresence(const std::string &path);

} // namespace pixet
