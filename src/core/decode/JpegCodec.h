#pragma once

#include <cstddef>
#include <cstdint>

#include "RgbImage.h"

namespace pixet {

// Decodes a JPEG buffer, using libjpeg's scaled-DCT decode (1/1, 1/2, 1/4, 1/8) to land
// close to targetLongEdge from above whenever the source is large enough - roughly 8x
// cheaper than decoding full-size and downscaling after. If the image is already smaller
// than targetLongEdge, decodes at full (native) size. Returns false on any decode error
// (corrupt/truncated/non-JPEG data) rather than throwing - this runs over real-world
// files and a bad one must not abort the scan.
bool decodeJpeg(const uint8_t *data, size_t size, int targetLongEdge, RgbImage &out);

// Reads just the JPEG header (SOF marker) for the image's true native pixel
// dimensions, without decoding any pixel data - cheap enough to always call
// alongside a thumbnail decode, which may itself be reading a scaled-DCT or
// embedded-preview version at a much smaller size. Returns false on a corrupt/
// non-JPEG buffer.
bool readJpegDimensions(const uint8_t *data, size_t size, int &width, int &height);

} // namespace pixet
