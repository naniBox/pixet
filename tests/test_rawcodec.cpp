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
//
// decodeRawThumbFromFile() is covered the same way and for the same reason. What can be
// asserted here is its failure contract, which is what generateThumb()'s fallback depends
// on: false must mean "no usable embedded preview", so that a miss falls through to the
// whole-file read rather than losing the file. Its success path was verified against a real
// 335-file Sony ARW folder, cross-checked against the camera JPEG sitting beside each RAW.
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

PIXET_TEST(RawCodecThumbFromFileFailsOnAMissingFile) {
    RgbImage img;
    int w = -1, h = -1;
    PIXET_CHECK(!decodeRawThumbFromFile(nonexistentPath("arw"), 320, img, w, h));
    // Zeroed rather than left at whatever the caller had, so a caller that ignores the
    // return value can't mistake stale stack values for real dimensions.
    PIXET_CHECK(w == 0);
    PIXET_CHECK(h == 0);
}

PIXET_TEST(RawCodecThumbFromFileFailsOnGarbageContent) {
    // A file that exists and opens fine but is not a RAW at all - distinct from the
    // missing-file case above, and the one that matters for the fallback in
    // generateThumb(): a false here has to mean "no usable preview, go read the whole
    // file", not "this file is unreadable".
    std::vector<uint8_t> garbage = {1, 2, 3, 4, 5, 6, 7, 8};
    std::string path = testTempPath("rawcodec_garbage.arw");
    writeTestFile(path, garbage);
    RgbImage img;
    int w = -1, h = -1;
    PIXET_CHECK(!decodeRawThumbFromFile(path, 320, img, w, h));
}

PIXET_TEST(ThumbGeneratorRawIsNoLongerUnsupported) {
    // Missing file, not garbage bytes - if Raw were still gated as Unsupported (like
    // Heic still is), this would come back Unsupported instead of Failed. Confirms
    // Format::Raw actually reaches the RAW decode path now.
    ThumbResult result = generateThumb(nonexistentPath("dng"), Format::Raw);
    PIXET_CHECK(result.tier == ThumbTier::Failed);
}

PIXET_TEST(ThumbGeneratorForceFullRenderStillFailsGracefullyOnMissingFile) {
    // Doesn't verify forceFullRender actually skips the embedded-preview attempt
    // (needs a real decodable RAW file - see the live-verified two-pass flow noted
    // above) - just that the parameter is threaded through generateThumb() without
    // upsetting the ordinary failure path.
    ThumbResult result = generateThumb(nonexistentPath("dng"), Format::Raw, 320, 85, /*forceFullRender=*/true);
    PIXET_CHECK(result.tier == ThumbTier::Failed);
}
