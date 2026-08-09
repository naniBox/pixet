#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../db/Schema.h"

namespace pixet {

// Which rung of the extraction ladder actually produced the thumbnail - the P1
// benchmark gate reports throughput broken down by this.
enum class ThumbTier {
    EmbeddedPreview, // decoded from an EXIF/container-embedded thumbnail - cheapest
    Decoded,         // decoded the main image (scaled-DCT where the format allows it)
    Unsupported,     // no decoder for this format yet
    Failed,          // decode was attempted and failed (corrupt/truncated file)
};

struct ThumbResult {
    ThumbTier tier = ThumbTier::Failed;
    std::vector<uint8_t> jpegBytes; // encoded thumbnail, ready to store in thumbs.db
    int width = 0;  // thumbnail's own pixel dimensions (what's actually in jpegBytes)
    int height = 0;
    // The original file's true native pixel dimensions - independent of `width`/
    // `height` above, which describe the (much smaller) generated thumbnail. Either
    // may be 0 if the source dimensions couldn't be determined (e.g. read failed).
    int origWidth = 0;
    int origHeight = 0;
    int orientation = 1;
};

// Runs the extraction ladder for one file: embedded preview -> scaled decode ->
// resize to targetLongEdge -> re-encode as JPEG q`quality`. Never throws - every
// failure mode (missing file, corrupt data, unsupported format) comes back as a
// ThumbResult with the appropriate tier so a bulk scan can continue past it.
ThumbResult generateThumb(const std::wstring &filePath, Format fmt, int targetLongEdge = 320, int quality = 85);

} // namespace pixet
