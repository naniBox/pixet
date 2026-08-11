#pragma once

#include <string>

namespace pixet {

// Unicode normalization to NFC (precomposed form). Like StringUtil.h's UTF-16 helpers on
// Windows, this exists only for the macOS implementation files under util/ and scan/ - it
// is not part of pixet_core's portable API, and there is no Windows counterpart because
// NTFS doesn't hand back decomposed filenames. See Unicode_mac.cpp for why it's needed.
std::string toNfc(const std::string &utf8);

} // namespace pixet
