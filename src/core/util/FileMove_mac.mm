#include "FileMove.h"

#include <copyfile.h>
#include <cstdio>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

#include <thread>
#include <chrono>

#import <Foundation/Foundation.h>

// Best-effort from the Windows machine this feature was built on, following the same
// pattern as util/FileIO_mac.cpp/util/PathUtil_mac.cpp: written against documented
// POSIX/Darwin behavior, not yet run. See devlog's P5 entries for the precedent - this
// needs an actual pass on the Mac (renamex_np/RENAME_EXCL availability against the real
// deployment target, copyfile() flag behavior, EXDEV handling across an external
// volume, and - the one Objective-C++ addition here - NSFileManager's trashItem:
// behavior/error codes) before being trusted the way the Windows implementation
// already is. This file is .mm (Objective-C++) rather than .cpp purely for
// moveToTrash() - there's no POSIX trash API, NSFileManager is the only correct way to
// reach the real per-volume Trash - every other function here is unchanged plain
// POSIX/Darwin C++ and compiles identically under either extension.
namespace pixet {

namespace {

FsResult mapErrno(int err) {
    switch (err) {
        case ENOENT:
            return FsResult::SourceMissing;
        case EEXIST:
            return FsResult::DestExists;
        case EACCES:
        case EPERM:
            return FsResult::PermissionDenied;
        case ENOSPC:
        case EDQUOT:
            return FsResult::DiskFull;
        case ENAMETOOLONG:
        case EILSEQ:
            return FsResult::NameInvalid;
        default:
            return FsResult::Unknown;
    }
}

// See FileMove_win.cpp's identical rationale: pixet's own file reads can hold a
// descriptor open on the exact file being cut/moved, and unlike Windows' mandatory
// FILE_SHARE_READ-only lock, POSIX has no equivalent to fail *this* call - so a
// sharing-style transient failure is unlikely here, but the retry costs nothing to
// keep symmetric with the Windows primitive's behavior.
template <typename Fn>
FsResult withRetry(Fn &&attempt) {
    static constexpr int kDelaysMs[] = {100, 300, 600};
    FsResult result = attempt();
    for (int delay : kDelaysMs) {
        if (result != FsResult::SharingViolation) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        result = attempt();
    }
    return result;
}

// Publishes `tempPath` at `dstUtf8` without ever overwriting an existing file there -
// the shared last step for both the cross-volume copy fallback in moveFile() and
// copyFile() itself.
FsResult publishTemp(const std::string &tempPath, const std::string &dstUtf8) {
    if (renamex_np(tempPath.c_str(), dstUtf8.c_str(), RENAME_EXCL) == 0) return FsResult::Ok;
    FsResult result = mapErrno(errno);
    ::unlink(tempPath.c_str());
    return result;
}

FsResult copyToTempThenPublish(const std::string &srcUtf8, const std::string &dstUtf8) {
    std::string tempPath = dstUtf8 + ".pixet-tmp-" + std::to_string(::getpid()) + "-" + std::to_string(::rand());

    // COPYFILE_ALL: data + metadata (mtime, flags, ACLs where supported). No
    // COPYFILE_EXCL here - `tempPath` is freshly minted, and requiring exclusivity on
    // it would just make a same-pid retry after a transient failure needlessly
    // finicky.
    if (copyfile(srcUtf8.c_str(), tempPath.c_str(), nullptr, COPYFILE_ALL) != 0) {
        FsResult result = mapErrno(errno);
        ::unlink(tempPath.c_str());
        return result;
    }

    struct stat srcSt {};
    struct stat tmpSt {};
    if (::stat(srcUtf8.c_str(), &srcSt) != 0 || ::stat(tempPath.c_str(), &tmpSt) != 0 ||
        srcSt.st_size != tmpSt.st_size) {
        // Source vanished mid-copy, or the copy is short - never publish a partial file.
        ::unlink(tempPath.c_str());
        return FsResult::Unknown;
    }

    return publishTemp(tempPath, dstUtf8);
}

} // namespace

const char *fsResultName(FsResult r) {
    switch (r) {
        case FsResult::Ok: return "Ok";
        case FsResult::SourceMissing: return "SourceMissing";
        case FsResult::DestExists: return "DestExists";
        case FsResult::PermissionDenied: return "PermissionDenied";
        case FsResult::SharingViolation: return "SharingViolation";
        case FsResult::DiskFull: return "DiskFull";
        case FsResult::NameInvalid: return "NameInvalid";
        default: return "Unknown";
    }
}

FsResult moveFile(const std::string &srcUtf8, const std::string &dstUtf8, bool *crossVolume) {
    bool crossed = false;

    FsResult result = withRetry([&]() -> FsResult {
        if (renamex_np(srcUtf8.c_str(), dstUtf8.c_str(), RENAME_EXCL) == 0) return FsResult::Ok;
        if (errno == EXDEV) {
            crossed = true;
            FsResult copyResult = copyToTempThenPublish(srcUtf8, dstUtf8);
            if (copyResult != FsResult::Ok) return copyResult;
            if (::unlink(srcUtf8.c_str()) != 0) return mapErrno(errno);
            return FsResult::Ok;
        }
        return mapErrno(errno);
    });

    if (crossVolume) *crossVolume = crossed;
    return result;
}

FsResult copyFile(const std::string &srcUtf8, const std::string &dstUtf8) {
    return withRetry([&]() -> FsResult { return copyToTempThenPublish(srcUtf8, dstUtf8); });
}

FsResult renameWithinDir(const std::string &fromUtf8, const std::string &toUtf8) {
    if (renamex_np(fromUtf8.c_str(), toUtf8.c_str(), RENAME_EXCL) == 0) return FsResult::Ok;
    return mapErrno(errno);
}

FsResult removeFile(const std::string &pathUtf8) {
    if (::unlink(pathUtf8.c_str()) == 0) return FsResult::Ok;
    return mapErrno(errno);
}

FsResult moveToTrash(const std::string &pathUtf8) {
    return withRetry([&]() -> FsResult {
        @autoreleasepool {
            NSString *path = [NSString stringWithUTF8String:pathUtf8.c_str()];
            if (!path) return FsResult::NameInvalid; // shouldn't happen - paths are normalized UTF-8 throughout
            NSURL *url = [NSURL fileURLWithPath:path];

            NSError *error = nil;
            if ([[NSFileManager defaultManager] trashItem:url resultingItemURL:nil error:&error]) return FsResult::Ok;
            if (!error) return FsResult::Unknown;

            // NSFileManager surfaces the underlying POSIX errno (when there is one) via
            // NSUnderlyingErrorKey under NSCocoaErrorDomain - prefer that over trying to
            // enumerate every Cocoa-specific NSFileManager error code by hand.
            NSError *underlying = error.userInfo[NSUnderlyingErrorKey];
            if (underlying && [underlying.domain isEqualToString:NSPOSIXErrorDomain]) {
                return mapErrno((int)underlying.code);
            }
            if ([error.domain isEqualToString:NSCocoaErrorDomain]) {
                if (error.code == NSFileNoSuchFileError) return FsResult::SourceMissing;
                if (error.code == NSFileWriteNoPermissionError || error.code == NSFileReadNoPermissionError) {
                    return FsResult::PermissionDenied;
                }
            }
            return FsResult::Unknown;
        }
    });
}

bool fileExists(const std::string &pathUtf8) {
    struct stat st {};
    return ::stat(pathUtf8.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool isDirectory(const std::string &pathUtf8) {
    struct stat st {};
    return ::stat(pathUtf8.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool statFile(const std::string &pathUtf8, int64_t *sizeOut, int64_t *mtimeUnixOut) {
    struct stat st {};
    if (::stat(pathUtf8.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    if (sizeOut) *sizeOut = static_cast<int64_t>(st.st_size);
    if (mtimeUnixOut) *mtimeUnixOut = static_cast<int64_t>(st.st_mtime);
    return true;
}

} // namespace pixet
