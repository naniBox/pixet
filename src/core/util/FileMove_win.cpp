#include "FileMove.h"

#include <Windows.h>
#include <shellapi.h>

#include <chrono>
#include <thread>

#include "StringUtil.h"

namespace pixet {

namespace {

int64_t fileTimeToUnix(const FILETIME &ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    // 100ns intervals since 1601-01-01 -> seconds since 1970-01-01. Same conversion
    // as DirWalker_win.cpp's private helper of the same name - not shared, since
    // that one is file-local too and this is a six-line function, not worth a third
    // file just to deduplicate.
    constexpr int64_t kEpochDiffSeconds = 11644473600LL;
    return static_cast<int64_t>(u.QuadPart / 10'000'000ULL) - kEpochDiffSeconds;
}

FsResult mapLastError(DWORD err) {
    switch (err) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return FsResult::SourceMissing;
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS:
            return FsResult::DestExists;
        case ERROR_ACCESS_DENIED:
            return FsResult::PermissionDenied;
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            return FsResult::SharingViolation;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
            return FsResult::DiskFull;
        case ERROR_INVALID_NAME:
        case ERROR_FILENAME_EXCED_RANGE:
            return FsResult::NameInvalid;
        default:
            return FsResult::Unknown;
    }
}

// pixet's own readWholeFile() opens files with FILE_SHARE_READ only (no
// FILE_SHARE_DELETE) - see FileIO_win.cpp - so a file cannot be renamed or deleted
// while pixet is mid-read of it (e.g. decoding a preview for the very item the user
// just cut). That's an entirely ordinary sequence of user actions, not a rare race,
// so the move/copy primitives retry a sharing violation a few times with backoff
// before giving up, rather than surfacing it as an immediate failure.
template <typename Fn>
FsResult withSharingRetry(Fn &&attempt) {
    static constexpr int kDelaysMs[] = {100, 300, 600};
    FsResult result = attempt();
    for (int delay : kDelaysMs) {
        if (result != FsResult::SharingViolation) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        result = attempt();
    }
    return result;
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
    std::wstring src = toUtf16(srcUtf8);
    std::wstring dst = toUtf16(dstUtf8);

    FsResult result = withSharingRetry([&]() -> FsResult {
        // MOVEFILE_COPY_ALLOWED: falls back to copy-then-delete-source across
        // volumes (and only removes the source once the copy is verified complete -
        // that's the API's own contract, not something this call has to orchestrate).
        // MOVEFILE_WRITE_THROUGH: don't report success until the data actually hit
        // disk (relevant specifically for the cross-volume copy fallback). Deliberately
        // no MOVEFILE_REPLACE_EXISTING - this primitive never overwrites.
        if (MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) return FsResult::Ok;
        return mapLastError(GetLastError());
    });

    if (crossVolume) {
        // Only meaningful to a caller deciding whether to trust the pre-op mtime -
        // and the honest answer is "maybe", so always report true and let the
        // caller re-stat rather than pretend this can be known cheaply and reliably
        // from a volume-root string comparison.
        *crossVolume = true;
    }
    return result;
}

FsResult copyFile(const std::string &srcUtf8, const std::string &dstUtf8) {
    std::wstring src = toUtf16(srcUtf8);
    std::wstring dst = toUtf16(dstUtf8);

    return withSharingRetry([&]() -> FsResult {
        BOOL cancel = FALSE;
        if (CopyFileExW(src.c_str(), dst.c_str(), nullptr, nullptr, &cancel, COPY_FILE_FAIL_IF_EXISTS)) {
            return FsResult::Ok;
        }
        return mapLastError(GetLastError());
    });
}

FsResult renameWithinDir(const std::string &fromUtf8, const std::string &toUtf8) {
    return moveFile(fromUtf8, toUtf8, nullptr); // same primitive - MoveFileExW handles a plain rename fine
}

FsResult removeFile(const std::string &pathUtf8) {
    std::wstring wide = toUtf16(pathUtf8);
    return withSharingRetry([&]() -> FsResult {
        if (DeleteFileW(wide.c_str())) return FsResult::Ok;
        return mapLastError(GetLastError());
    });
}

FsResult moveToTrash(const std::string &pathUtf8) {
    // SHFileOperationW's pFrom requires a double-null-terminated path list, even for a
    // single path - toUtf16() only null-terminates once (the implicit one from c_str()),
    // so this appends the extra terminator explicitly rather than reusing that buffer
    // as-is.
    std::wstring doubleNulled = toUtf16(pathUtf8);
    doubleNulled.push_back(L'\0');

    return withSharingRetry([&]() -> FsResult {
        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = doubleNulled.c_str();
        // ALLOWUNDO: Recycle Bin, not a permanent delete - the whole point of this
        // primitive. NOCONFIRMATION/SILENT/NOERRORUI: pixet owns its own confirmation
        // dialog and progress/error reporting, so none of the shell's own UI should
        // ever show up underneath this call.
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;

        int result = SHFileOperationW(&op);
        if (result == 0 && !op.fAnyOperationsAborted) return FsResult::Ok;
        // SHFileOperationW's return value is a legacy mix of old CopyEngine DE_* pseudo-
        // codes and ordinary Win32 error codes - confirmed empirically, not assumed: a
        // syntactically well-formed but missing path (e.g. "C:\nope\gone.bin") comes
        // back as plain ERROR_FILE_NOT_FOUND (2), which mapLastError() already handles,
        // but a path this parser considers malformed (no drive letter - the shared
        // cross-platform test fixture's deliberately POSIX-style
        // "/no-such-dir/nope.bin" triggered this) comes back as 0x7C (DE_INVALIDFILES),
        // a code space mapLastError() doesn't cover. Handled explicitly here rather
        // than folded into mapLastError(), which only promises to cover real
        // GetLastError() values.
        if (result == 0x7C) return FsResult::NameInvalid;
        return mapLastError((DWORD)result);
    });
}

bool fileExists(const std::string &pathUtf8) {
    std::wstring wide = toUtf16(pathUtf8);
    DWORD attrs = GetFileAttributesW(wide.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool isDirectory(const std::string &pathUtf8) {
    std::wstring wide = toUtf16(pathUtf8);
    DWORD attrs = GetFileAttributesW(wide.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool statFile(const std::string &pathUtf8, int64_t *sizeOut, int64_t *mtimeUnixOut) {
    std::wstring wide = toUtf16(pathUtf8);
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &data)) return false;
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;

    if (sizeOut) {
        ULARGE_INTEGER sz;
        sz.LowPart = data.nFileSizeLow;
        sz.HighPart = data.nFileSizeHigh;
        *sizeOut = static_cast<int64_t>(sz.QuadPart);
    }
    if (mtimeUnixOut) *mtimeUnixOut = fileTimeToUnix(data.ftLastWriteTime);
    return true;
}

} // namespace pixet
