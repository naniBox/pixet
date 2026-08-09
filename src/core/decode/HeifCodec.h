#pragma once

#include <cstddef>
#include <cstdint>

#include "RgbImage.h"

namespace pixet {

// Extracts a HEIF container's embedded thumbnail image, if it has one - HEIF's own
// thumbnail box, the same "cheap preview extraction" concept the decode ladder already
// uses for JPEG's EXIF thumbnail and RAW's LibRaw unpack_thumb. Unlike those two, a
// HEIF thumbnail is still HEVC-coded (no shortcut to a cheaper codec like JPEG's), but
// it's still a much smaller image to decode than the primary one. Returns false if the
// container has no thumbnail image at all (caller should fall back to decodeHeif()).
bool decodeHeifThumb(const uint8_t *data, size_t size, RgbImage &out);

// Full decode of the primary image. libheif applies the container's own orientation
// transformations by default (unlike JPEG, no separate applyOrientation() step is
// needed here - same shape as RAW/TIFF). Always decodes at native resolution - no
// scaled-decode option, same as most formats here other than JPEG.
bool decodeHeif(const uint8_t *data, size_t size, RgbImage &out);

// Reads just the primary image handle's dimensions, without decoding any pixel data.
bool readHeifDimensions(const uint8_t *data, size_t size, int &width, int &height);

} // namespace pixet
