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

std::vector<DirEntry> listDir(const std::wstring &path) {
    std::vector<DirEntry> entries;

    std::wstring pattern = path;
    if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';

    WIN32_FIND_DATAW findData;
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &findData, FindExSearchNameMatch, nullptr,
                                 FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        throw std::runtime_error("listDir failed (" + std::to_string(err) + "): " + toUtf8(path));
    }

    do {
        std::wstring name = findData.cFileName;
        if (name == L"." || name == L"..") continue;

        DirEntry entry;
        entry.name = name;
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

int64_t dirMtimeUnix(const std::wstring &path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        throw std::runtime_error("dirMtimeUnix failed: " + toUtf8(path));
    }
    return fileTimeToUnix(data.ftLastWriteTime);
}

} // namespace pixet
