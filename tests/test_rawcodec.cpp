// Unlike JPEG/PNG (and WebP/AVIF/TIFF later), there's no accessible RAW *encoder* to
// synthesize a round-trip fixture with - camera RAW formats are proprietary/complex
// enough that hand-building a minimal valid one isn't practical the way test_exif.cpp
// hand-builds a minimal TIFF/IFD. These tests cover error-handling and format-gating
// only; the actual decode path (embedded-preview extraction, orientation, dimensions)
// was live-verified during development against a real Pixel-phone DNG file on the dev
// machine - not committed here, since a test depending on a specific absolute path on
// one machine isn't portable/reproducible.
#include "TestHarness.h"
#include "TestPaths.h"

#include "db/Schema.h"
#include "decode/RawCodec.h"
#include "thumb/ThumbGenerator.h"

using namespace pixet;

PIXET_TEST(RawCodecThumbFailsOnGarbageData) {
    std::vector<uint8_t> garbage = {1, 2, 3, 4, 5, 6, 7, 8};
    RgbImage img;
    PIXET_CHECK(!decodeRawThumb(garbage.data(), garbage.size(), 320, img));
}

PIXET_TEST(RawCodecFullDecodeFailsOnGarbageData) {
    std::vector<uint8_t> garbage = {1, 2, 3, 4, 5, 6, 7, 8};
    RgbImage img;
    PIXET_CHECK(!decodeRaw(garbage.data(), garbage.size(), img));
}

PIXET_TEST(ReadRawDimensionsFailsOnGarbageData) {
    std::vector<uint8_t> garbage = {1, 2, 3, 4, 5, 6, 7, 8};
    int w = 0, h = 0;
    PIXET_CHECK(!readRawDimensions(garbage.data(), garbage.size(), w, h));
}

PIXET_TEST(ThumbGeneratorRawIsNoLongerUnsupported) {
    // Missing file, not garbage bytes - if Raw were still gated as Unsupported (like
    // Heic still is), this would come back Unsupported instead of Failed. Confirms
    // Format::Raw actually reaches the RAW decode path now.
    ThumbResult result = generateThumb(L"Z:\\does\\not\\exist.dng", Format::Raw);
    PIXET_CHECK(result.tier == ThumbTier::Failed);
}
