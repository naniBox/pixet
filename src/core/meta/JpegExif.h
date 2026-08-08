#pragma once

#include <cstddef>
#include <cstdint>

namespace pixet {

struct ExifInfo {
    int orientation = 1; // EXIF 1..8, 1 = no transform needed

    // Byte range of the embedded thumbnail JPEG within the *same* buffer that was
    // parsed, if any (0 length = none). Cameras/phones almost always embed a small
    // (~160x120) preview here - decoding it is far cheaper than the main image.
    size_t thumbOffset = 0;
    size_t thumbLength = 0;

    bool hasThumb() const { return thumbLength > 0; }
};

// Scans JPEG markers for an APP1 "Exif" segment and pulls orientation + the
// embedded thumbnail's location out of its TIFF IFD0/IFD1. Tolerant of malformed
// input - always bounds-checks against `size` and simply stops early rather than
// throwing, since this runs over real-world files that may be truncated/corrupt.
ExifInfo parseJpegExif(const uint8_t *data, size_t size);

} // namespace pixet
