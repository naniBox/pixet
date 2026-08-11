#include "TestHarness.h"
#include "TestPaths.h"

#include "db/Schema.h"
#include "decode/JpegCodec.h"
#include "thumb/ThumbGenerator.h"

using namespace pixet;

namespace {

std::vector<uint8_t> makeGradientJpeg(int w, int h, int quality) {
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
    std::vector<uint8_t> jpeg;
    encodeJpeg(img, quality, jpeg);
    return jpeg;
}

} // namespace

PIXET_TEST(ThumbGeneratorUnsupportedFormatSkipsDecode) {
    // No file at this path - if ThumbGenerator tried to read it, this would fail
    // differently (Failed, not Unsupported). Confirms format is checked first.
    // Format::Unknown, not a specific format: every real format now has a decoder
    // (see test_{png,raw,video,tiff,webp,avif,heif}codec.cpp) - Unknown is the only
    // value that's permanently, correctly Unsupported by definition.
    ThumbResult result = generateThumb(nonexistentPath("xyz"), Format::Unknown);
    PIXET_CHECK(result.tier == ThumbTier::Unsupported);
}

PIXET_TEST(ThumbGeneratorFailsOnMissingFile) {
    ThumbResult result = generateThumb(nonexistentPath("jpg"), Format::Jpeg);
    PIXET_CHECK(result.tier == ThumbTier::Failed);
}

PIXET_TEST(ThumbGeneratorDecodesAndDownscalesLargeJpeg) {
    auto path = testTempPath("thumbgen_large.jpg");
    writeTestFile(path, makeGradientJpeg(1600, 1200, 90));

    ThumbResult result = generateThumb(path, Format::Jpeg, 320, 85);
    PIXET_CHECK(result.tier == ThumbTier::Decoded); // no EXIF thumb present -> main-image path
    PIXET_CHECK(!result.jpegBytes.empty());
    int longEdge = result.width > result.height ? result.width : result.height;
    PIXET_CHECK(longEdge <= 320);
    PIXET_CHECK(longEdge > 0);

    // Round-trip: the stored bytes must actually decode back to those dimensions.
    RgbImage decoded;
    PIXET_CHECK(decodeJpeg(result.jpegBytes.data(), result.jpegBytes.size(), 0, decoded));
    PIXET_CHECK(decoded.w == result.width);
    PIXET_CHECK(decoded.h == result.height);

    // origWidth/origHeight must be the true source dimensions, not the thumbnail's -
    // regression test for a bug where files.width/height got the thumbnail's (much
    // smaller) size instead of the original image's.
    PIXET_CHECK(result.origWidth == 1600);
    PIXET_CHECK(result.origHeight == 1200);
    PIXET_CHECK(result.origWidth != result.width);
    PIXET_CHECK(result.origHeight != result.height);
}

PIXET_TEST(ThumbGeneratorDoesNotUpscaleSmallJpeg) {
    auto path = testTempPath("thumbgen_small.jpg");
    writeTestFile(path, makeGradientJpeg(100, 80, 90));

    ThumbResult result = generateThumb(path, Format::Jpeg, 320, 85);
    PIXET_CHECK(result.tier == ThumbTier::Decoded);
    PIXET_CHECK(result.width == 100);
    PIXET_CHECK(result.height == 80);
    PIXET_CHECK(result.origWidth == 100);
    PIXET_CHECK(result.origHeight == 80);
}
