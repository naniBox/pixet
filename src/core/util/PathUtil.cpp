#include "PathUtil.h"

#include <Windows.h>

#include <vector>

namespace pixet {

std::wstring normalizePath(const std::wstring &path) {
    std::vector<wchar_t> buf(MAX_PATH * 4);
    DWORD len = GetFullPathNameW(path.c_str(), (DWORD)buf.size(), buf.data(), nullptr);
    if (len == 0 || len >= buf.size()) return path;

    std::wstring abs(buf.data(), len);
    while (abs.size() > 3 && abs.back() == L'\\') abs.pop_back(); // normalize, keep "C:\"
    return abs;
}

} // namespace pixet
