#include "FileIO.h"

#include <Windows.h>

#include <algorithm>

#include "StringUtil.h"

namespace pixet {

bool readWholeFile(const std::string &path, std::vector<uint8_t> &out) {
    std::wstring widePath = toUtf16(path);
    HANDLE h = CreateFileW(widePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0) {
        CloseHandle(h);
        return false;
    }

    out.resize((size_t)size.QuadPart);
    size_t totalRead = 0;
    bool ok = true;
    while (totalRead < out.size()) {
        DWORD toRead = (DWORD)std::min<size_t>(out.size() - totalRead, 1u << 20); // 1MB chunks
        DWORD bytesRead = 0;
        if (!ReadFile(h, out.data() + totalRead, toRead, &bytesRead, nullptr) || bytesRead == 0) {
            ok = false;
            break;
        }
        totalRead += bytesRead;
    }

    CloseHandle(h);
    if (!ok) out.clear();
    return ok;
}

} // namespace pixet
