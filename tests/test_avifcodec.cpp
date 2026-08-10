// Like camera RAW and video (see test_rawcodec.cpp/test_videocodec.cpp), there's no
// round-trip fixture here - this vcpkg build's libavif links a decode-only AV1 backend
// (avifEncoderWrite fails with no available encoder, confirmed empirically: an earlier
// version of this file tried encoding a fixture on the fly via libavif's own encode
// API, the same approach that works fine for PNG/TIFF/WebP, and it came back empty
// every time). These tests cover error-handling and format-gating only. AVIF was the
// user's second-lowest P4 priority specifically because their phone doesn't produce
// it, so a real-file live-verification pass (the fallback used for RAW/video, which
// don't have an encode path either) wasn't pursued either - there's no real AVIF
// sample available on the dev machine to verify against.
#include "TestHarness.h"

#include "db/Schema.h"
#include "decode/AvifCodec.h"
#include "thumb/ThumbGenerator.h"

using namespace pixet;

PIXET_TEST(AvifCodecFailsOnGarbageData) {
    std::vector<uint8_t> garbage = {1, 2, 3, 4, 5};
    RgbImage decoded;
    PIXET_CHECK(!decodeAvif(garbage.data(), garbage.size(), decoded));
}

PIXET_TEST(ReadAvifDimensionsFailsOnGarbageData) {
    std::vector<uint8_t> garbage = {1, 2, 3, 4, 5};
    int w = 0, h = 0;
    PIXET_CHECK(!readAvifDimensions(garbage.data(), garbage.size(), w, h));
}

PIXET_TEST(ThumbGeneratorAvifIsNoLongerUnsupported) {
    // Missing file, not garbage bytes - if Avif were still gated as Unsupported (like
    // Heic still is), this would come back Unsupported instead of Failed. Confirms
    // Format::Avif actually reaches the AVIF decode path now.
    ThumbResult result = generateThumb("Z:\\does\\not\\exist.avif", Format::Avif);
    PIXET_CHECK(result.tier == ThumbTier::Failed);
}
