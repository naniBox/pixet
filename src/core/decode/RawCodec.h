#pragma once

#include <cstddef>
#include <cstdint>

#include "RgbImage.h"

namespace pixet {

// Extracts the embedded preview camera RAW files typically carry (often a full-size
// or near-full-size JPEG - DNG and most manufacturer RAW formats include one) via
// LibRaw's thumbnail API. Far cheaper than a full demosaic decode when available,
// which - per the same embedded-preview-first rationale that holds for JPEG, where 96.5%
// of real files hit that path - is worth trying first here too.
// targetLongEdge is forwarded to the inner JPEG decode when the embedded preview
// happens to itself be a JPEG (the common case), so a large embedded preview still
// gets libjpeg's cheap scaled-DCT decode rather than a full-size one. Returns false
// if there's no usable embedded preview at all (caller should fall back to
// decodeRaw()) or on any error - LibRaw returns error codes rather than throwing, and
// this preserves that (never aborts on a corrupt/unsupported RAW file).
bool decodeRawThumb(const uint8_t *data, size_t size, int targetLongEdge, RgbImage &out);

// Full RAW decode: demosaics the sensor data via LibRaw's default pipeline (sRGB
// output, embedded white balance/rotation applied automatically). Slow relative to
// every other tier in the ladder - only reached when there's no usable embedded
// preview. Always decodes at native resolution (LibRaw has no scaled-decode option);
// the caller downscales afterward via resizeBoxDownscale.
bool decodeRaw(const uint8_t *data, size_t size, RgbImage &out);

// Reads just the RAW file's header/metadata (LibRaw's open-without-unpack phase) for
// the image's true native dimensions, without decoding any pixel data. Already
// accounts for the embedded orientation (swapped for a 90-degree rotation), matching
// what decodeRawThumb()/decodeRaw() actually produce (LibRaw auto-rotates its output
// to match, unlike JPEG where orientation is applied as a separate explicit step).
bool readRawDimensions(const uint8_t *data, size_t size, int &width, int &height);

} // namespace pixet
