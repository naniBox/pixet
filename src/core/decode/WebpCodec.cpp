#include "WebpCodec.h"

#include <webp/decode.h>

#include "DecodeLimits.h"

namespace pixet {

bool decodeWebp(const uint8_t *data, size_t size, RgbImage &out) {
    int w = 0, h = 0;
    // WebPGetInfo first, purely to have the dimensions *before* WebPDecodeRGB allocates the
    // full-size buffer itself - by the time that returns w and h, the memory is already
    // spent and a limit could only free it again. A header parse is cheap enough that
    // paying for it on every decode is not worth avoiding.
    if (!WebPGetInfo(data, size, &w, &h) || !decodelimits::pixelsAllowed(w, h)) return false;

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
