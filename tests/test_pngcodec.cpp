#include "TestHarness.h"
#include "TestPaths.h"

#include <cstring>

#include <png.h>

#include "db/Schema.h"
#include "decode/JpegCodec.h"
#include "decode/PngCodec.h"
#include "thumb/ThumbGenerator.h"

using namespace pixet;

namespace {

RgbImage makeGradientImage(int w, int h) {
    RgbImage img;
    img.w = w;
    img.h = h;
    img.pixels.resize((size_t)w * h * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t *p = &img.pixels[(size_t)(y * w + x) * 3];
            p[0] = (uint8_t)(x * 255 / w);
            p[1] = (uint8_t)(y * 255 / h);
            p[2] = 128;
        }
    }
    return img;
}

// No production encodePng (never needed - PNG sources are only ever decoded, then
// re-encoded as JPEG for storage) - libpng's simplified write API used directly here
// to synthesize a real PNG fixture on the fly, same spirit as test_thumbgen.cpp's
// makeGradientJpeg.
std::vector<uint8_t> encodePngForTest(const RgbImage &img) {
    png_image image;
    std::memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    image.width = img.w;
    image.height = img.h;
    image.format = PNG_FORMAT_RGB;

    png_alloc_size_t size = 0;
    png_image_write_to_memory(&image, nullptr, &size, 0, img.pixels.data(), 0, nullptr);
    std::vector<uint8_t> out(size);
    png_image_write_to_memory(&image, out.data(), &size, 0, img.pixels.data(), 0, nullptr);
    png_image_free(&image);
    return out;
}

} // namespace

PIXET_TEST(PngCodecDecodesGradient) {
    RgbImage src = makeGradientImage(64, 48);
    std::vector<uint8_t> pngBytes = encodePngForTest(src);
    PIXET_CHECK(!pngBytes.empty());

    RgbImage decoded;
    PIXET_CHECK(decodePng(pngBytes.data(), pngBytes.size(), decoded));
    PIXET_CHECK(decoded.w == 64);
    PIXET_CHECK(decoded.h == 48);

    // Round-trip a couple of sample pixels - decodePng requests plain RGB output, so
    // this should match the source exactly (no alpha/palette reduction involved).
    PIXET_CHECK(decoded.pixels[0] == src.pixels[0]);
    PIXET_CHECK(decoded.pixels[2] == src.pixels[2]);
}

PIXET_TEST(PngCodecFailsOnGarbageData) {
    std::vector<uint8_t> garbage = {1, 2, 3, 4, 5};
    RgbImage decoded;
    PIXET_CHECK(!decodePng(garbage.data(), garbage.size(), decoded));
}

PIXET_TEST(ReadPngDimensionsMatchesSource) {
    RgbImage src = makeGradientImage(200, 150);
    std::vector<uint8_t> pngBytes = encodePngForTest(src);

    int w = 0, h = 0;
    PIXET_CHECK(readPngDimensions(pngBytes.data(), pngBytes.size(), w, h));
    PIXET_CHECK(w == 200);
    PIXET_CHECK(h == 150);
}

PIXET_TEST(ThumbGeneratorDecodesAndDownscalesLargePng) {
    auto path = testTempPath("thumbgen_large.png");
    writeTestFile(path, encodePngForTest(makeGradientImage(1600, 1200)));

    ThumbResult result = generateThumb(path, Format::Png, 320, 85);
    PIXET_CHECK(result.tier == ThumbTier::Decoded);
    PIXET_CHECK(!result.jpegBytes.empty());
    int longEdge = result.width > result.height ? result.width : result.height;
    PIXET_CHECK(longEdge <= 320);
    PIXET_CHECK(longEdge > 0);

    // Round-trip: the stored bytes (re-encoded as JPEG regardless of source format)
    // must actually decode back to those dimensions.
    RgbImage decoded;
    PIXET_CHECK(decodeJpeg(result.jpegBytes.data(), result.jpegBytes.size(), 0, decoded));
    PIXET_CHECK(decoded.w == result.width);
    PIXET_CHECK(decoded.h == result.height);

    PIXET_CHECK(result.origWidth == 1600);
    PIXET_CHECK(result.origHeight == 1200);
    PIXET_CHECK(result.origWidth != result.width);
    PIXET_CHECK(result.origHeight != result.height);
}

PIXET_TEST(ThumbGeneratorDoesNotUpscaleSmallPng) {
    auto path = testTempPath("thumbgen_small.png");
    writeTestFile(path, encodePngForTest(makeGradientImage(100, 80)));

    ThumbResult result = generateThumb(path, Format::Png, 320, 85);
    PIXET_CHECK(result.tier == ThumbTier::Decoded);
    PIXET_CHECK(result.width == 100);
    PIXET_CHECK(result.height == 80);
    PIXET_CHECK(result.origWidth == 100);
    PIXET_CHECK(result.origHeight == 80);
}

PIXET_TEST(ThumbGeneratorFailsOnMissingPngFile) {
    ThumbResult result = generateThumb("Z:\\does\\not\\exist.png", Format::Png);
    PIXET_CHECK(result.tier == ThumbTier::Failed);
}
