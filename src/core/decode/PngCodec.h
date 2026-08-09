#pragma once

#include <cstddef>
#include <cstdint>

#include "RgbImage.h"

namespace pixet {

// Decodes a PNG buffer at native resolution. Unlike libjpeg, libpng has no
// scaled/progressive decode - there's no cheaper-than-full-size path, so the caller
// (ThumbGenerator) always downscales afterward via resizeBoxDownscale. Handles
// palette, grayscale, and any bit depth transparently (libpng's simplified API
// normalizes all of it to 8-bit RGB); an alpha channel is composited away rather than
// preserved, since the JPEG this ultimately gets re-encoded as has no alpha channel to
// put it in anyway. Returns false on any decode error (corrupt/truncated/non-PNG data).
bool decodePng(const uint8_t *data, size_t size, RgbImage &out);

// Reads just the PNG header (IHDR) for the image's true native dimensions, without
// decoding any pixel data.
bool readPngDimensions(const uint8_t *data, size_t size, int &width, int &height);

} // namespace pixet
