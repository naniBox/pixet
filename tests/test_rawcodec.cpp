// Unlike JPEG/PNG (and WebP/AVIF/TIFF later), there's no accessible RAW *encoder* to
// synthesize a round-trip fixture with - camera RAW formats are proprietary/complex
// enough that hand-building a minimal valid one isn't practical the way test_exif.cpp
// hand-builds a minimal TIFF/IFD. These tests cover error-handling and format-gating
// only; the actual decode path (embedded-preview extraction, orientation, dimensions)
// was live-verified during development against a real Pixel-phone DNG file on the dev
// machine, and the two-pass --render-raws upgrade (FileState::DoneNeedsRender ->
// forceFullRender -> Done) against a real Sony ARW file - not committed here, since a
// test depending on a specific absolute path on one machine isn't portable/
// reproducible.
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

PIXET_TEST(ThumbGeneratorForceFullRenderStillFailsGracefullyOnMissingFile) {
    // Doesn't verify forceFullRender actually skips the embedded-preview attempt
    // (needs a real decodable RAW file - see the live-verified two-pass flow noted
    // above) - just that the parameter is threaded through generateThumb() without
    // upsetting the ordinary failure path.
    ThumbResult result = generateThumb(L"Z:\\does\\not\\exist.dng", Format::Raw, 320, 85, /*forceFullRender=*/true);
    PIXET_CHECK(result.tier == ThumbTier::Failed);
}
