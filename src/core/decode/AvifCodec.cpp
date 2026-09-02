#include "AvifCodec.h"

#include <cstring>

#include <avif/avif.h>

#include "DecodeLimits.h"

namespace pixet {

bool decodeAvif(const uint8_t *data, size_t size, RgbImage &out) {
    avifDecoder *decoder = avifDecoderCreate();
    if (!decoder) return false;

    bool ok = false;
    if (avifDecoderSetIOMemory(decoder, data, size) == AVIF_RESULT_OK &&
        avifDecoderParse(decoder) == AVIF_RESULT_OK && avifDecoderNextImage(decoder) == AVIF_RESULT_OK &&
        // Before avifRGBImageAllocatePixels below, which is the first full-size allocation
        // here - and then out.pixels is a second one the same size again.
        decodelimits::pixelsAllowed(decoder->image->width, decoder->image->height)) {
        avifImage *image = decoder->image;

        avifRGBImage rgb;
        avifRGBImageSetDefaults(&rgb, image);
        rgb.format = AVIF_RGB_FORMAT_RGB;
        rgb.depth = 8;

        if (avifRGBImageAllocatePixels(&rgb) == AVIF_RESULT_OK) {
            if (avifImageYUVToRGB(image, &rgb) == AVIF_RESULT_OK) {
                out.w = (int)image->width;
                out.h = (int)image->height;
                out.pixels.resize((size_t)out.w * out.h * 3);
                // rgb.rowBytes may include row padding beyond width*3 - copy row by
                // row respecting the real stride rather than assuming it's tightly
                // packed like RgbImage itself always is.
                for (int y = 0; y < out.h; ++y) {
                    std::memcpy(out.pixels.data() + (size_t)y * out.w * 3, rgb.pixels + (size_t)y * rgb.rowBytes,
                                (size_t)out.w * 3);
                }
                ok = true;
            }
            avifRGBImageFreePixels(&rgb);
        }
    }

    avifDecoderDestroy(decoder);
    return ok;
}

bool readAvifDimensions(const uint8_t *data, size_t size, int &width, int &height) {
    avifDecoder *decoder = avifDecoderCreate();
    if (!decoder) return false;

    bool ok = false;
    if (avifDecoderSetIOMemory(decoder, data, size) == AVIF_RESULT_OK &&
        avifDecoderParse(decoder) == AVIF_RESULT_OK) {
        width = (int)decoder->image->width;
        height = (int)decoder->image->height;
        ok = width > 0 && height > 0;
    }

    avifDecoderDestroy(decoder);
    return ok;
}

} // namespace pixet
