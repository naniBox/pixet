#include "PngCodec.h"

#include <cstring>

#include <png.h>

#include "DecodeLimits.h"

namespace pixet {

bool decodePng(const uint8_t *data, size_t size, RgbImage &out) {
    if (size == 0) return false;

    png_image image;
    std::memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;

    if (!png_image_begin_read_from_memory(&image, data, size)) return false;

    // Request plain 8-bit RGB regardless of the source's actual bit depth/color
    // type (palette, grayscale, 16-bit, with-or-without alpha) - the simplified API
    // does that normalization internally. Any alpha in the source is composited onto
    // an implicit white background rather than preserved (no background color
    // supplied to png_image_finish_read below), matching RgbImage's plain
    // (non-alpha) layout.
    image.format = PNG_FORMAT_RGB;

    // libpng has no scaled decode, so the buffer below is always the source's full native
    // size - which is exactly what a decompression bomb exploits, a few hundred KB of file
    // expanding to tens of GB of pixels. begin_read has already given us the dimensions,
    // and nothing has been allocated yet, so this is the cheapest possible place to refuse.
    if (!decodelimits::pixelsAllowed(image.width, image.height)) {
        png_image_free(&image);
        return false;
    }

    out.w = (int)image.width;
    out.h = (int)image.height;
    out.pixels.resize(PNG_IMAGE_SIZE(image));

    if (!png_image_finish_read(&image, nullptr, out.pixels.data(), 0, nullptr)) {
        png_image_free(&image);
        out = RgbImage();
        return false;
    }

    png_image_free(&image);
    return true;
}

bool readPngDimensions(const uint8_t *data, size_t size, int &width, int &height) {
    if (size == 0) return false;

    png_image image;
    std::memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;

    if (!png_image_begin_read_from_memory(&image, data, size)) return false;

    width = (int)image.width;
    height = (int)image.height;

    png_image_free(&image);
    return true;
}

} // namespace pixet
