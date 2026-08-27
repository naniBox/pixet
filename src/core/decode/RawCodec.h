#pragma once

#include <cstddef>
#include <cstdint>

#include <string>

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

// decodeRawThumb() + readRawDimensions() in one, reading through LibRaw's own file
// datastream instead of being handed the file's bytes up front.
//
// This is not a convenience overload, it is the difference between reading ~0.4MB and ~24MB
// per file. A RAW's embedded preview lives near the front (measured on Sony ARW: a 5KB
// thumbnail at 39KB in and a 229KB preview ending 0.36MB into a 23.6MB file), so LibRaw
// needs only the header, the metadata and the preview's own bytes. Handing it a buffer
// instead forces the caller to read all 24MB first, and on a spinning disk that read *is*
// the thumbnailing time - a 335-file folder of ARWs costs 7.8GB of I/O to extract 120MB of
// previews.
//
// Both dimensions and preview come from one open, so a caller that wants both doesn't pay
// for a second header parse. Returns false if there is no usable embedded preview (the
// caller should fall back to reading the file and calling decodeRaw()) or on any error;
// width/height may still have been filled in when it does.
//
// `pathUtf8` is UTF-8, converted to UTF-16 on Windows so non-ASCII paths open correctly.
bool decodeRawThumbFromFile(const std::string &pathUtf8, int targetLongEdge, RgbImage &out, int &width,
                            int &height);

} // namespace pixet
