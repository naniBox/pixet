#pragma once

#include <string>

namespace pixet {

// UTF-16 <-> UTF-8 conversion. The rest of the codebase (SQLite storage, Qt via
// QString::toStdString()/fromStdString(), every path/filename signature in
// pixet_core) uses UTF-8 throughout - these two exist only for the Windows-specific
// implementation files (*_win.cpp under util/ and scan/) that cross into a WinAPI
// call expecting natively UTF-16 wide strings, right at that boundary.
std::string toUtf8(const std::wstring &utf16);
std::wstring toUtf16(const std::string &utf8);

} // namespace pixet
