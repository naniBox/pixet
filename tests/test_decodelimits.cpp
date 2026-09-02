#include "TestHarness.h"
#include "TestPaths.h"

#include <cstring>

#include <png.h>

#include "db/Schema.h"
#include "decode/DecodeLimits.h"
#include "decode/DisplayCodec.h"
#include "decode/PngCodec.h"
#include "thumb/ThumbGenerator.h"
#include "util/Shutdown.h"

using namespace pixet;

namespace {

// The limits are process-global (see DecodeLimits.h), so a test that changes them and then
// throws would silently change the answers every test registered after it gets - which for
// a suite whose order depends on link order is a genuinely awful failure to debug.
// Everything below goes through this.
struct LimitsGuard {
    int64_t savedFileBytes;
    int64_t savedPixels;
    LimitsGuard(int64_t fileBytes, int64_t pixels)
        : savedFileBytes(decodelimits::maxFileBytes()), savedPixels(decodelimits::maxPixels()) {
        decodelimits::configure(fileBytes, pixels);
    }
    ~LimitsGuard() { decodelimits::configure(savedFileBytes, savedPixels); }
};

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

// Same helper as test_pngcodec.cpp's. PNG is the convenient format to test the limits
// through because it has no embedded-preview tier to fall back to, so what the limit does
// is the only thing deciding the outcome.
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

PIXET_TEST(DecodeLimitsArithmetic) {
    LimitsGuard guard(1000, 1000);

    PIXET_CHECK(decodelimits::maxFileBytes() == 1000);
    PIXET_CHECK(decodelimits::maxPixels() == 1000);

    PIXET_CHECK(decodelimits::fileSizeAllowed(999));
    PIXET_CHECK(decodelimits::fileSizeAllowed(1000)); // the limit itself passes, not just under it
    PIXET_CHECK(!decodelimits::fileSizeAllowed(1001));

    PIXET_CHECK(decodelimits::pixelsAllowed(10, 100)); // exactly at the limit
    PIXET_CHECK(!decodelimits::pixelsAllowed(10, 101));
    PIXET_CHECK(decodelimits::pixelsAllowed(1, 1));

    // A failed stat hands back a negative size, and rejecting on that would turn "couldn't
    // read the file" into "file is too big" - the wrong diagnosis for the wrong reason.
    PIXET_CHECK(decodelimits::fileSizeAllowed(-1));
    // Nonsense dimensions are the codec's to report as a broken file, not this function's
    // to reinterpret as oversized.
    PIXET_CHECK(decodelimits::pixelsAllowed(0, 0));
    PIXET_CHECK(decodelimits::pixelsAllowed(-5, 10));
}

PIXET_TEST(DecodeLimitsZeroMeansUnlimited) {
    LimitsGuard guard(0, 0);

    // The whole point of 0 as the "off" value: it has to survive numbers that would
    // overflow any naive comparison rather than wrapping into a spurious rejection.
    PIXET_CHECK(decodelimits::fileSizeAllowed(1LL << 62));
    PIXET_CHECK(decodelimits::pixelsAllowed(2000000, 2000000)); // 4e12 pixels
}

PIXET_TEST(DecodeLimitsPixelCapDoesNotOverflow) {
    // Dimensions whose product overflows int64. A naive `width * height <= limit` would
    // wrap and quietly pass; pixelsAllowed divides instead.
    LimitsGuard guard(0, 1000);
    PIXET_CHECK(!decodelimits::pixelsAllowed(4000000000LL, 4000000000LL));
}

PIXET_TEST(PngCodecRefusesImageOverPixelLimit) {
    RgbImage src = makeGradientImage(64, 48); // 3072 pixels
    std::vector<uint8_t> pngBytes = encodePngForTest(src);
    PIXET_CHECK(!pngBytes.empty());

    {
        LimitsGuard guard(0, 3072); // exactly at the limit - still decodes
        RgbImage decoded;
        PIXET_CHECK(decodePng(pngBytes.data(), pngBytes.size(), decoded));
        PIXET_CHECK(decoded.w == 64 && decoded.h == 48);
    }
    {
        LimitsGuard guard(0, 3071); // one pixel under
        RgbImage decoded;
        PIXET_CHECK(!decodePng(pngBytes.data(), pngBytes.size(), decoded));
        PIXET_CHECK(decoded.empty()); // refused before allocating, so nothing came back
    }
}

PIXET_TEST(ThumbGeneratorReportsOverSizedFileAsUnsupported) {
    RgbImage src = makeGradientImage(64, 48);
    std::vector<uint8_t> pngBytes = encodePngForTest(src);
    std::string path = testTempPath("decodelimits_big.png");
    writeTestFile(path, pngBytes);

    {
        // Deliberately smaller than the file, so the size gate is what stops it - and it
        // has to stop it before the read, which is the entire point of gating on a stat.
        LimitsGuard guard(16, 0);
        ThumbResult result = generateThumb(path, Format::Png, 32, 85);
        // Unsupported, not Failed: nothing is wrong with this file, we simply decided not
        // to open it. Failed would have the indexer record it as broken forever.
        PIXET_CHECK(result.tier == ThumbTier::Unsupported);
        PIXET_CHECK(result.jpegBytes.empty());
    }
    {
        LimitsGuard guard((int64_t)pngBytes.size(), 0); // exactly big enough
        ThumbResult result = generateThumb(path, Format::Png, 32, 85);
        PIXET_CHECK(result.tier == ThumbTier::Decoded);
        PIXET_CHECK(!result.jpegBytes.empty());
    }
}

PIXET_TEST(ThumbGeneratorReportsOverSizedImageAsUnsupported) {
    RgbImage src = makeGradientImage(64, 48);
    std::string path = testTempPath("decodelimits_wide.png");
    writeTestFile(path, encodePngForTest(src));

    LimitsGuard guard(0, 100); // 3072 pixels, so well over
    ThumbResult result = generateThumb(path, Format::Png, 32, 85);
    // Same reasoning as the size gate above. This one is decided in ThumbGenerator from the
    // header dimensions rather than by the codec returning false, precisely so it doesn't
    // arrive here as Failed - see the comment at that check.
    PIXET_CHECK(result.tier == ThumbTier::Unsupported);
    PIXET_CHECK(result.jpegBytes.empty());
}

PIXET_TEST(DecodeForDisplayRefusesOverSizedFile) {
    RgbImage src = makeGradientImage(64, 48);
    std::vector<uint8_t> pngBytes = encodePngForTest(src);
    std::string path = testTempPath("decodelimits_display.png");
    writeTestFile(path, pngBytes);

    {
        // The preview pane and fullscreen viewer read the file whole on their own threads,
        // entirely separately from the indexer, so the limit has to hold on that path too -
        // opening the file that took 50GB to thumbnail must not cost 50GB to look at.
        LimitsGuard guard(16, 0);
        RgbImage out;
        PIXET_CHECK(!decodeForDisplay(path, Format::Png, 32, out));
    }
    {
        LimitsGuard guard(0, 0);
        RgbImage out;
        PIXET_CHECK(decodeForDisplay(path, Format::Png, 32, out));
        PIXET_CHECK(!out.empty());
    }
}

PIXET_TEST(ShutdownFlagStartsClear) {
    // Only the default is asserted, and deliberately so: requestShutdown() is one-way by
    // design (see Shutdown.h), so calling it here would make generateThumb return
    // ThumbTier::Cancelled for every test registered after this one - an order-dependent
    // failure across the whole suite in exchange for one assertion. Adding a reset just to
    // test it would mean putting a footgun in production code to check that the
    // footgun-free version works.
    PIXET_CHECK(!shutdownRequested());
}
