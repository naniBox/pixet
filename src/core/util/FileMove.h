#pragma once

#include <cstdint>
#include <string>

namespace pixet {

enum class FsResult {
    Ok,
    SourceMissing,
    DestExists,
    PermissionDenied,
    SharingViolation,
    DiskFull,
    NameInvalid,
    Unknown,
};

const char *fsResultName(FsResult r);

// Never overwrites: `dstUtf8` must not already exist (fileops::execute resolves
// collisions before calling this - see src/core/fileops/FileOps.h). A same-volume
// move is a metadata rename (instant); cross-volume degrades to copy-then-delete-
// source, and the source is only removed once the copy is verified complete. If
// `crossVolume` is given, it reports which happened - callers that then need the
// post-op (mtime, size) should just re-stat rather than branch on this, since a
// cross-volume copy is not guaranteed to preserve mtime.
//
// Deliberately not std::filesystem::rename()/POSIX rename(2): both silently replace
// an existing destination, which is exactly the thing a collision-safe file-move
// primitive must not do by default.
FsResult moveFile(const std::string &srcUtf8, const std::string &dstUtf8, bool *crossVolume = nullptr);

// Never overwrites. Content and modification time are preserved (best-effort beyond
// that - ACLs/xattrs only insofar as the platform primitive carries them for free).
FsResult copyFile(const std::string &srcUtf8, const std::string &dstUtf8);

// Same-directory rename - used both for the "move the collision victim aside before
// touching anything" safety dance and for publishing a temp copy atomically. Never
// overwrites.
FsResult renameWithinDir(const std::string &fromUtf8, const std::string &toUtf8);

FsResult removeFile(const std::string &pathUtf8);

// Moves a file to the OS Recycle Bin (Windows) / Trash (macOS) rather than permanently
// erasing it - the only delete primitive this codebase exposes, since these are the
// user's real photos and a permanent-delete path isn't worth the risk it carries for
// the convenience it'd save. Never overwrites/merges with anything already in the
// trash - the OS owns collision-naming there.
FsResult moveToTrash(const std::string &pathUtf8);

bool fileExists(const std::string &pathUtf8);
bool isDirectory(const std::string &pathUtf8);

// False if `pathUtf8` doesn't exist or can't be stat'd. Used to write the *actual*
// post-move/copy (mtime, size) into a files row rather than trusting the pre-op
// values - see fileops::execute()'s comment on why that's load-bearing for the
// thumbnail-preservation optimization.
bool statFile(const std::string &pathUtf8, int64_t *sizeOut, int64_t *mtimeUnixOut);

} // namespace pixet
