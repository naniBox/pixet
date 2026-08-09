#pragma once

#include <cstddef>
#include <cstdint>

#include "RgbImage.h"

namespace pixet {

// Decodes a WebP buffer at native resolution via libwebp's simple decode API
// (WebPDecodeRGB). Like PNG/TIFF, no cheaper-than-full-size decode path is used here -
// libwebp's advanced API does support a scaled/reduced decode, which would be a
// reasonable future optimization for large WebP sources, but WebP isn't expected to be
// a large fraction of a real photo library so it hasn't been worth building yet. Any
// alpha channel is dropped (not composited), matching PngCodec's handling - same
// rationale, no alpha channel in the JPEG this ultimately gets re-encoded as. Returns
// false on any decode error.
bool decodeWebp(const uint8_t *data, size_t size, RgbImage &out);

// Reads just the WebP header for the image's true native dimensions, without decoding
// any pixel data.
bool readWebpDimensions(const uint8_t *data, size_t size, int &width, int &height);

} // namespace pixet
