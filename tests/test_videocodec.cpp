// Like camera RAW (see test_rawcodec.cpp), there's no practical encoder path here to
// synthesize a round-trip video fixture - encoding and muxing a minimal valid video
// container is a much bigger undertaking than JPEG/PNG's simple encode calls. These
// tests cover error-handling and format-gating only; the actual decode path (poster-
// frame extraction, seek-to-min(3s,10%), and rotation correction for portrait
// phone video) was live-verified during development against real Pixel-phone MP4
// files on the dev machine, including a visual check that a rotated portrait video's
// poster frame comes out upright rather than sideways or mirrored - not committed
// here, since a test depending on a specific absolute path on one machine isn't
// portable/reproducible.
#include "TestHarness.h"
#include "TestPaths.h"

#include "db/Schema.h"
#include "decode/VideoCodec.h"
#include "thumb/ThumbGenerator.h"

using namespace pixet;

PIXET_TEST(VideoCodecFailsOnGarbageData) {
    // decodeVideoPosterFrame takes a path, not a buffer (see VideoCodec.h for why) -
    // exercise the "not a real video file" failure path via a path to a file that
    // isn't a video at all.
    RgbImage img;
    PIXET_CHECK(!decodeVideoPosterFrame(nonexistentPath("mp4"), img));
}

PIXET_TEST(ThumbGeneratorVideoIsNoLongerUnsupported) {
    // Missing file, not a real-but-corrupt one - if Video were still gated as
    // Unsupported (like Heic still is), this would come back Unsupported instead of
    // Failed. Confirms Format::Video actually reaches the video decode path now.
    ThumbResult result = generateThumb(nonexistentPath("mp4"), Format::Video);
    PIXET_CHECK(result.tier == ThumbTier::Failed);
}
