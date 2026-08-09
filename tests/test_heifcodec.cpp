// Like AVIF (see test_avifcodec.cpp), no round-trip fixture here: vcpkg.json declares
// libheif with default-features off specifically to skip x265 (the HEVC encoder,
// a heavy dependency not needed for read-only thumbnailing) - confirmed by no x265
// buildtree existing at all, so there's no encoder plugin to synthesize a fixture
// with, only the libde265 decode backend this codec actually needs. These tests cover
// error-handling and format-gating only. HEIC was the user's lowest P4 priority
// (their phone shoots JPEG, not HEIC), and there's no real HEIC sample on the dev
// machine to live-verify against either.
#include "TestHarness.h"

#include "db/Schema.h"
#include "decode/HeifCodec.h"
#include "thumb/ThumbGenerator.h"

using namespace pixet;

PIXET_TEST(HeifCodecThumbFailsOnGarbageData) {
    std::vector<uint8_t> garbage = {1, 2, 3, 4, 5, 6, 7, 8};
    RgbImage img;
    PIXET_CHECK(!decodeHeifThumb(garbage.data(), garbage.size(), img));
}

PIXET_TEST(HeifCodecFullDecodeFailsOnGarbageData) {
    std::vector<uint8_t> garbage = {1, 2, 3, 4, 5, 6, 7, 8};
    RgbImage img;
    PIXET_CHECK(!decodeHeif(garbage.data(), garbage.size(), img));
}

PIXET_TEST(ReadHeifDimensionsFailsOnGarbageData) {
    std::vector<uint8_t> garbage = {1, 2, 3, 4, 5, 6, 7, 8};
    int w = 0, h = 0;
    PIXET_CHECK(!readHeifDimensions(garbage.data(), garbage.size(), w, h));
}

PIXET_TEST(ThumbGeneratorHeicIsNoLongerUnsupported) {
    // Missing file, not garbage bytes - if Heic were still gated as Unsupported, this
    // would come back Unsupported instead of Failed. Confirms Format::Heic actually
    // reaches the HEIF decode path now - the last format in the P4 priority list.
    ThumbResult result = generateThumb(L"Z:\\does\\not\\exist.heic", Format::Heic);
    PIXET_CHECK(result.tier == ThumbTier::Failed);
}
