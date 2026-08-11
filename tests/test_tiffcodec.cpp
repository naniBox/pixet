#include "TestHarness.h"
#include "TestPaths.h"

#include <cstring>

#include <tiffio.h>

#include "db/Schema.h"
#include "decode/JpegCodec.h"
#include "decode/TiffCodec.h"
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

// No production encodeTiff (never needed - TIFF sources are only ever decoded, then
// re-encoded as JPEG for storage). libtiff has no built-in "write to memory"
// convenience the way libpng's simplified API does, so this wraps TIFFClientOpen()
// with a small growable-buffer write callback, same spirit as TiffCodec.cpp's
// read-side memory callbacks.
struct MemWriteState {
    std::vector<uint8_t> *buf;
    toff_t pos;
};

tmsize_t twWrite(thandle_t handle, void *data, tmsize_t n) {
    auto *state = static_cast<MemWriteState *>(handle);
    size_t needed = (size_t)state->pos + (size_t)n;
    if (state->buf->size() < needed) state->buf->resize(needed);
    memcpy(state->buf->data() + state->pos, data, (size_t)n);
    state->pos += n;
    return n;
}
tmsize_t twRead(thandle_t, void *, tmsize_t) { return 0; }
toff_t twSeek(thandle_t handle, toff_t off, int whence) {
    auto *state = static_cast<MemWriteState *>(handle);
    toff_t newPos;
    switch (whence) {
        case SEEK_SET: newPos = off; break;
        case SEEK_CUR: newPos = state->pos + off; break;
        case SEEK_END: newPos = (toff_t)state->buf->size() + off; break;
        default: return (toff_t)-1;
    }
    state->pos = newPos;
    return newPos;
}
int twClose(thandle_t) { return 0; }
toff_t twSize(thandle_t handle) { return (toff_t)static_cast<MemWriteState *>(handle)->buf->size(); }

std::vector<uint8_t> encodeTiffForTest(const RgbImage &img) {
    std::vector<uint8_t> out;
    MemWriteState state{&out, 0};
    TIFF *tif =
        TIFFClientOpen("memory", "w", (thandle_t)&state, twRead, twWrite, twSeek, twClose, twSize, nullptr, nullptr);
    if (!tif) return {};

    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, (uint32_t)img.w);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, (uint32_t)img.h);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 3);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, (uint32_t)img.h);

    for (int y = 0; y < img.h; ++y) {
        TIFFWriteScanline(tif, const_cast<uint8_t *>(img.pixels.data() + (size_t)y * img.w * 3), y, 0);
    }

    TIFFClose(tif);
    return out;
}

} // namespace

PIXET_TEST(TiffCodecDecodesGradient) {
    RgbImage src = makeGradientImage(64, 48);
    std::vector<uint8_t> tiffBytes = encodeTiffForTest(src);
    PIXET_CHECK(!tiffBytes.empty());

    RgbImage decoded;
    PIXET_CHECK(decodeTiff(tiffBytes.data(), tiffBytes.size(), decoded));
    PIXET_CHECK(decoded.w == 64);
    PIXET_CHECK(decoded.h == 48);
    PIXET_CHECK(decoded.pixels[0] == src.pixels[0]);
    PIXET_CHECK(decoded.pixels[2] == src.pixels[2]);
}

PIXET_TEST(TiffCodecFailsOnGarbageData) {
    std::vector<uint8_t> garbage = {1, 2, 3, 4, 5};
    RgbImage decoded;
    PIXET_CHECK(!decodeTiff(garbage.data(), garbage.size(), decoded));
}

PIXET_TEST(ReadTiffDimensionsMatchesSource) {
    RgbImage src = makeGradientImage(200, 150);
    std::vector<uint8_t> tiffBytes = encodeTiffForTest(src);

    int w = 0, h = 0;
    PIXET_CHECK(readTiffDimensions(tiffBytes.data(), tiffBytes.size(), w, h));
    PIXET_CHECK(w == 200);
    PIXET_CHECK(h == 150);
}

PIXET_TEST(ThumbGeneratorDecodesAndDownscalesLargeTiff) {
    auto path = testTempPath("thumbgen_large.tiff");
    writeTestFile(path, encodeTiffForTest(makeGradientImage(1600, 1200)));

    ThumbResult result = generateThumb(path, Format::Tiff, 320, 85);
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

PIXET_TEST(ThumbGeneratorDoesNotUpscaleSmallTiff) {
    auto path = testTempPath("thumbgen_small.tiff");
    writeTestFile(path, encodeTiffForTest(makeGradientImage(100, 80)));

    ThumbResult result = generateThumb(path, Format::Tiff, 320, 85);
    PIXET_CHECK(result.tier == ThumbTier::Decoded);
    PIXET_CHECK(result.width == 100);
    PIXET_CHECK(result.height == 80);
    PIXET_CHECK(result.origWidth == 100);
    PIXET_CHECK(result.origHeight == 80);
}

PIXET_TEST(ThumbGeneratorFailsOnMissingTiffFile) {
    ThumbResult result = generateThumb(nonexistentPath("tiff"), Format::Tiff);
    PIXET_CHECK(result.tier == ThumbTier::Failed);
}
