#include "AppPaths.h"

#include <Windows.h>
#include <ShlObj.h>

#include <stdexcept>

#include "StringUtil.h"

#pragma comment(lib, "Shell32.lib")

namespace pixet {

std::string appDataDir() {
    PWSTR base = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        throw std::runtime_error("could not resolve %LOCALAPPDATA%");
    }
    std::wstring dir = std::wstring(base) + L"\\pixet";
    CoTaskMemFree(base);

    if (!CreateDirectoryW(dir.c_str(), nullptr)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            throw std::runtime_error("could not create app data directory");
        }
    }
    return toUtf8(dir);
}

std::string indexDbPath() { return appDataDir() + "\\index.db"; }
std::string thumbsDbPath() { return appDataDir() + "\\thumbs.db"; }

} // namespace pixet
