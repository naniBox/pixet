#pragma once

#include <cstdint>
#include <vector>

namespace pixet {

// Interleaved RGB, 3 bytes/pixel, no padding - the shared currency between every
// format decoder and ThumbGenerator. Decode straight into this regardless of source
// format (JPEG/PNG/RAW/...), and everything downstream (orientation, resize, the
// final JPEG re-encode for storage) is format-agnostic from here on.
struct RgbImage {
    std::vector<uint8_t> pixels;
    int w = 0;
    int h = 0;

    bool empty() const { return w <= 0 || h <= 0; }
};

// Downscales to targetLongEdge via box-filter averaging. No-op (copies through) if the
// image is already at or below targetLongEdge - this never upscales.
void resizeBoxDownscale(const RgbImage &src, int targetLongEdge, RgbImage &dst);

// Applies an EXIF orientation transform (1..8) in place. 1 is a no-op. 5-8 rotate and
// swap width/height.
void applyOrientation(RgbImage &img, int orientation);

// Encodes to a baseline JPEG in memory - every format's thumbnail ultimately gets
// stored this way, regardless of source format. Returns false on failure (should be
// rare - input is always a buffer we just decoded/resized ourselves, not untrusted
// data).
bool encodeJpeg(const RgbImage &img, int quality, std::vector<uint8_t> &out);

} // namespace pixet
