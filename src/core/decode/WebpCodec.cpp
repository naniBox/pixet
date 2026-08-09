#include "WebpCodec.h"

#include <webp/decode.h>

namespace pixet {

bool decodeWebp(const uint8_t *data, size_t size, RgbImage &out) {
    int w = 0, h = 0;
    uint8_t *rgb = WebPDecodeRGB(data, size, &w, &h);
    if (!rgb) return false;

    out.w = w;
    out.h = h;
    out.pixels.assign(rgb, rgb + (size_t)w * h * 3);
    WebPFree(rgb);
    return true;
}

bool readWebpDimensions(const uint8_t *data, size_t size, int &width, int &height) {
    return WebPGetInfo(data, size, &width, &height) != 0;
}

} // namespace pixet
