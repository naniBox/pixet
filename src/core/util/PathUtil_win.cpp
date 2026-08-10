#include "PathUtil.h"

#include <Windows.h>

#include <vector>

#include "StringUtil.h"

namespace pixet {

std::string normalizePath(const std::string &path) {
    std::wstring wide = toUtf16(path);
    std::vector<wchar_t> buf(MAX_PATH * 4);
    DWORD len = GetFullPathNameW(wide.c_str(), (DWORD)buf.size(), buf.data(), nullptr);
    if (len == 0 || len >= buf.size()) return path;

    std::wstring abs(buf.data(), len);
    while (abs.size() > 3 && abs.back() == L'\\') abs.pop_back(); // normalize, keep "C:\"
    return toUtf8(abs);
}

} // namespace pixet
