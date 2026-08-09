#pragma once

#include <cstddef>
#include <cstdint>

#include "RgbImage.h"

namespace pixet {

// Decodes a TIFF buffer at native resolution via libtiff's TIFFReadRGBAImageOriented -
// like libpng, no scaled/progressive decode exists for TIFF, so the caller
// (ThumbGenerator) downscales afterward via resizeBoxDownscale. TIFFReadRGBAImage is
// libtiff's high-level convenience path: it transparently handles the format's wide
// variety of encodings (any bit depth, palette/CMYK/grayscale colorspaces, most
// compression schemes) by normalizing everything to a flat RGBA raster, which is
// exactly what a thumbnail needs (no requirement to preserve exact per-format fidelity
// the way a real image editor would). Passing ORIENTATION_TOPLEFT also makes it
// auto-correct for the file's own TIFFTAG_ORIENTATION tag (the same 1-8 encoding EXIF
// orientation borrows from TIFF), so - like RAW's LibRaw pipeline, and unlike JPEG's
// explicit applyOrientation() step - the output comes out already upright. Returns
// false on any decode error.
bool decodeTiff(const uint8_t *data, size_t size, RgbImage &out);

// Reads just the TIFF header/IFD (image width/height/orientation tags) for the
// image's true native dimensions, without decoding any pixel data. Already accounts
// for the same orientation-driven rotation decodeTiff() applies (swapped for a
// 90-degree rotation), matching what decodeTiff() actually produces.
bool readTiffDimensions(const uint8_t *data, size_t size, int &width, int &height);

} // namespace pixet
