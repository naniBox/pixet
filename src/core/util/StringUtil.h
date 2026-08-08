#pragma once

#include <string>

namespace pixet {

// UTF-16 <-> UTF-8 conversion. Windows file APIs are natively UTF-16;
// SQLite storage and most of the rest of the codebase use UTF-8.
std::string toUtf8(const std::wstring &utf16);
std::wstring toUtf16(const std::string &utf8);

} // namespace pixet
