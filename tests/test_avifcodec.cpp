// Like camera RAW and video (see test_rawcodec.cpp/test_videocodec.cpp), there's no
// round-trip fixture here - this vcpkg build's libavif links a decode-only AV1 backend.
// avifEncoderWrite fails with no available encoder: synthesizing a fixture on the fly
// through libavif's own encode API, the approach test_pngcodec/test_tiffcodec/
// test_webpcodec all use, comes back empty every time. So these tests cover
// error-handling and format-gating only. The fallback used for RAW and video - live
// verification against a real file instead of a synthesized one - isn't available here
// either, since there's no real AVIF sample on the dev machine and the camera feeding
// this library doesn't produce any.
#include "TestHarness.h"
#include "TestPaths.h"

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
    ThumbResult result = generateThumb(nonexistentPath("avif"), Format::Avif);
    PIXET_CHECK(result.tier == ThumbTier::Failed);
}
