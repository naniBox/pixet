#include "TestHarness.h"
#include "TestPaths.h"

#include <webp/encode.h>

#include "db/Schema.h"
#include "decode/JpegCodec.h"
#include "decode/WebpCodec.h"
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

// No production encodeWebp (never needed - WebP sources are only ever decoded, then
// re-encoded as JPEG for storage) - libwebp's own simple encode API used directly here
// to synthesize a real WebP fixture on the fly, same spirit as the other format tests.
std::vector<uint8_t> encodeWebpForTest(const RgbImage &img) {
    uint8_t *out = nullptr;
    size_t size = WebPEncodeRGB(img.pixels.data(), img.w, img.h, img.w * 3, 90.0f, &out);
    if (size == 0 || !out) return {};
    std::vector<uint8_t> result(out, out + size);
    WebPFree(out);
    return result;
}

} // namespace

PIXET_TEST(WebpCodecDecodesGradient) {
    RgbImage src = makeGradientImage(64, 48);
    std::vector<uint8_t> webpBytes = encodeWebpForTest(src);
    PIXET_CHECK(!webpBytes.empty());

    RgbImage decoded;
    PIXET_CHECK(decodeWebp(webpBytes.data(), webpBytes.size(), decoded));
    PIXET_CHECK(decoded.w == 64);
    PIXET_CHECK(decoded.h == 48);
}

PIXET_TEST(WebpCodecFailsOnGarbageData) {
    std::vector<uint8_t> garbage = {1, 2, 3, 4, 5};
    RgbImage decoded;
    PIXET_CHECK(!decodeWebp(garbage.data(), garbage.size(), decoded));
}

PIXET_TEST(ReadWebpDimensionsMatchesSource) {
    RgbImage src = makeGradientImage(200, 150);
    std::vector<uint8_t> webpBytes = encodeWebpForTest(src);

    int w = 0, h = 0;
    PIXET_CHECK(readWebpDimensions(webpBytes.data(), webpBytes.size(), w, h));
    PIXET_CHECK(w == 200);
    PIXET_CHECK(h == 150);
}

PIXET_TEST(ThumbGeneratorDecodesAndDownscalesLargeWebp) {
    auto path = testTempPath("thumbgen_large.webp");
    writeTestFile(path, encodeWebpForTest(makeGradientImage(1600, 1200)));

    ThumbResult result = generateThumb(path, Format::Webp, 320, 85);
    PIXET_CHECK(result.tier == ThumbTier::Decoded);
    PIXET_CHECK(!result.jpegBytes.empty());
    int longEdge = result.width > result.height ? result.width : result.height;
    PIXET_CHECK(longEdge <= 320);
    PIXET_CHECK(longEdge > 0);

    RgbImage decoded;
    PIXET_CHECK(decodeJpeg(result.jpegBytes.data(), result.jpegBytes.size(), 0, decoded));
    PIXET_CHECK(decoded.w == result.width);
    PIXET_CHECK(decoded.h == result.height);

    PIXET_CHECK(result.origWidth == 1600);
    PIXET_CHECK(result.origHeight == 1200);
    PIXET_CHECK(result.origWidth != result.width);
    PIXET_CHECK(result.origHeight != result.height);
}

PIXET_TEST(ThumbGeneratorDoesNotUpscaleSmallWebp) {
    auto path = testTempPath("thumbgen_small.webp");
    writeTestFile(path, encodeWebpForTest(makeGradientImage(100, 80)));

    ThumbResult result = generateThumb(path, Format::Webp, 320, 85);
    PIXET_CHECK(result.tier == ThumbTier::Decoded);
    PIXET_CHECK(result.width == 100);
    PIXET_CHECK(result.height == 80);
    PIXET_CHECK(result.origWidth == 100);
    PIXET_CHECK(result.origHeight == 80);
}

PIXET_TEST(ThumbGeneratorFailsOnMissingWebpFile) {
    ThumbResult result = generateThumb("Z:\\does\\not\\exist.webp", Format::Webp);
    PIXET_CHECK(result.tier == ThumbTier::Failed);
}
