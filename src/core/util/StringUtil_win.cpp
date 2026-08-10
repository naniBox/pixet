#include "StringUtil.h"

#include <Windows.h>

namespace pixet {

std::string toUtf8(const std::wstring &utf16) {
    if (utf16.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), (int)utf16.size(), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, utf16.data(), (int)utf16.size(), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring toUtf16(const std::string &utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), out.data(), len);
    return out;
}

} // namespace pixet
