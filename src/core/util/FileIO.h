#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pixet {

// Reads an entire file into memory. `path` is UTF-8. Returns false on any I/O error
// (missing, locked, permission denied) rather than throwing - callers scan real
// directories where individual files can vanish/fail mid-walk.
bool readWholeFile(const std::string &path, std::vector<uint8_t> &out);

// Reads at most `maxBytes` from the start of the file at `path` - `out` ends up
// sized to whatever was actually available, which may be less than `maxBytes` for a
// small file. Same false-on-error contract as readWholeFile(). For callers that only
// ever need a bounded prefix (EXIF segments are capped at 64KB by the JPEG spec
// itself), so reading metadata from a many-megapixel JPEG doesn't mean reading the
// whole multi-MB file.
bool readFilePrefix(const std::string &path, size_t maxBytes, std::vector<uint8_t> &out);

} // namespace pixet
