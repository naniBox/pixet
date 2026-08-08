#pragma once

#include <Windows.h>

#include <string>

// Returns a fresh (deleted-if-existing) temp path for `name`, so each test gets its
// own index.db/thumbs.db pair without interference between runs.
inline std::wstring testTempPath(const std::wstring &name) {
    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring dir = std::wstring(tempDir) + L"pixet_tests\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = dir + name;
    DeleteFileW(path.c_str());
    return path;
}
