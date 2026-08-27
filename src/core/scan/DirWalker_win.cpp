#include "DirWalker.h"

#include <Windows.h>

#include <stdexcept>

#include "../util/StringUtil.h"

namespace pixet {

namespace {

int64_t fileTimeToUnix(const FILETIME &ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    // 100ns intervals since 1601-01-01 -> seconds since 1970-01-01.
    constexpr int64_t kEpochDiffSeconds = 11644473600LL;
    return static_cast<int64_t>(u.QuadPart / 10'000'000ULL) - kEpochDiffSeconds;
}

} // namespace

std::vector<DirEntry> listDir(const std::string &path) {
    std::vector<DirEntry> entries;

    std::wstring pattern = toUtf16(path);
    if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';

    WIN32_FIND_DATAW findData;
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &findData, FindExSearchNameMatch, nullptr,
                                 FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        throw std::runtime_error("listDir failed (" + std::to_string(err) + "): " + path);
    }

    do {
        std::wstring name = findData.cFileName;
        if (name == L"." || name == L"..") continue;

        DirEntry entry;
        entry.name = toUtf8(name);
        entry.isDir = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entry.mtimeUnix = fileTimeToUnix(findData.ftLastWriteTime);
        if (!entry.isDir) {
            ULARGE_INTEGER sz;
            sz.LowPart = findData.nFileSizeLow;
            sz.HighPart = findData.nFileSizeHigh;
            entry.size = static_cast<int64_t>(sz.QuadPart);
        }
        entries.push_back(std::move(entry));
    } while (FindNextFileW(h, &findData));

    FindClose(h);
    return entries;
}

DirPresence dirPresence(const std::string &path) {
    std::wstring wide = toUtf16(path);
    DWORD attrs = GetFileAttributesW(wide.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        return (attrs & FILE_ATTRIBUTE_DIRECTORY) ? DirPresence::Present : DirPresence::Missing;
    }
    // The macOS side of this distinction is load-bearing (see DirPresence) and Windows
    // has the same shape for different reasons: a disconnected network share or an
    // unmounted removable drive must not read as "this folder was deleted". Only the
    // two not-found codes are definitive; ERROR_ACCESS_DENIED and everything else means
    // the question went unanswered.
    DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND || err == ERROR_INVALID_NAME) {
        return DirPresence::Missing;
    }
    return DirPresence::Unreadable;
}

int64_t dirMtimeUnix(const std::string &path) {
    std::wstring wide = toUtf16(path);
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &data)) {
        throw std::runtime_error("dirMtimeUnix failed: " + path);
    }
    return fileTimeToUnix(data.ftLastWriteTime);
}

} // namespace pixet
