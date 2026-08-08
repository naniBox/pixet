#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pixet {

// Reads an entire file into memory. Returns false on any I/O error (missing,
// locked, permission denied) rather than throwing - callers scan real
// directories where individual files can vanish/fail mid-walk.
bool readWholeFile(const std::wstring &path, std::vector<uint8_t> &out);

} // namespace pixet
