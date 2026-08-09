#pragma once

#include <cstddef>
#include <cstdint>

#include "RgbImage.h"

namespace pixet {

// Decodes a (still-image) AVIF buffer at native resolution via libavif. No orientation
// handling: AVIF can carry "irot"/"imir" rotation/mirror item properties (it shares
// HEIF's underlying ISOBMFF container), but this is uncommon in practice compared to
// JPEG/HEIC from phone cameras - most AVIF in the wild is web-optimized and already
// pixel-correct - so, like PNG/WebP, this is a known scope limitation rather than a
// parsed/applied transform. Returns false on any decode error, or if the file is an
// animated AVIF sequence rather than a still image (only the first/primary image is
// ever wanted here).
bool decodeAvif(const uint8_t *data, size_t size, RgbImage &out);

// Reads just the AVIF container's parsed metadata (width/height) for the image's true
// native dimensions, without decoding any pixel data - avifDecoderParse() alone
// (skipping avifDecoderNextImage(), the actual AV1 decode) is enough for this.
bool readAvifDimensions(const uint8_t *data, size_t size, int &width, int &height);

} // namespace pixet
