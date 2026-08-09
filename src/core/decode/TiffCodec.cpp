#include "TiffCodec.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include <tiffio.h>

namespace pixet {

namespace {

// libtiff has no built-in "open from memory" convenience (unlike libjpeg/libpng's
// mem-src / simplified APIs) - TIFFClientOpen() needs these five small callbacks
// wrapping our already-in-memory file buffer instead.
struct MemTiffState {
    const uint8_t *data;
    tmsize_t size;
    toff_t pos;
};

tmsize_t tiffRead(thandle_t handle, void *buf, tmsize_t n) {
    auto *state = static_cast<MemTiffState *>(handle);
    tmsize_t avail = state->size - state->pos;
    if (avail < 0) avail = 0;
    tmsize_t toRead = n < avail ? n : avail;
    if (toRead > 0) std::memcpy(buf, state->data + state->pos, (size_t)toRead);
    state->pos += toRead;
    return toRead;
}

tmsize_t tiffWrite(thandle_t, void *, tmsize_t) { return 0; } // read-only source, never called

toff_t tiffSeek(thandle_t handle, toff_t off, int whence) {
    auto *state = static_cast<MemTiffState *>(handle);
    toff_t newPos;
    switch (whence) {
        case SEEK_SET: newPos = off; break;
        case SEEK_CUR: newPos = state->pos + off; break;
        case SEEK_END: newPos = state->size + off; break;
        default: return (toff_t)-1;
    }
    state->pos = newPos;
    return newPos;
}

int tiffClose(thandle_t) { return 0; } // buffer is caller-owned, nothing to release

toff_t tiffSize(thandle_t handle) { return static_cast<MemTiffState *>(handle)->size; }

TIFF *openMemTiff(const uint8_t *data, size_t size, MemTiffState &state) {
    // libtiff's default warning/error handlers print straight to stderr - same
    // silencing rationale as libjpeg's output_message() override elsewhere. These are
    // process-global (not per-TIFF*), but resetting them here is cheap and idempotent.
    TIFFSetWarningHandler(nullptr);
    TIFFSetErrorHandler(nullptr);

    state = MemTiffState{data, (tmsize_t)size, 0};
    return TIFFClientOpen("memory", "r", (thandle_t)&state, tiffRead, tiffWrite, tiffSeek, tiffClose, tiffSize,
                           nullptr, nullptr);
}

} // namespace

bool decodeTiff(const uint8_t *data, size_t size, RgbImage &out) {
    MemTiffState state;
    TIFF *tif = openMemTiff(data, size, state);
    if (!tif) return false;

    uint32_t w = 0, h = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    if (w == 0 || h == 0) {
        TIFFClose(tif);
        return false;
    }

    std::vector<uint32_t> raster((size_t)w * h);
    // ORIENTATION_TOPLEFT: rows top-to-bottom in the output raster (libtiff's plain
    // TIFFReadRGBAImage(), without the orientation parameter, is bottom-to-top by
    // default - an old X11/OpenGL-style convention that would otherwise flip every
    // decoded thumbnail upside down).
    if (!TIFFReadRGBAImageOriented(tif, w, h, raster.data(), ORIENTATION_TOPLEFT, 0)) {
        TIFFClose(tif);
        return false;
    }

    out.w = (int)w;
    out.h = (int)h;
    out.pixels.resize((size_t)w * h * 3);
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        uint32_t px = raster[i];
        out.pixels[i * 3 + 0] = (uint8_t)TIFFGetR(px);
        out.pixels[i * 3 + 1] = (uint8_t)TIFFGetG(px);
        out.pixels[i * 3 + 2] = (uint8_t)TIFFGetB(px);
    }

    TIFFClose(tif);
    return true;
}

bool readTiffDimensions(const uint8_t *data, size_t size, int &width, int &height) {
    MemTiffState state;
    TIFF *tif = openMemTiff(data, size, state);
    if (!tif) return false;

    uint32_t w = 0, h = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);

    // Defaults to ORIENTATION_TOPLEFT (1, "no rotation") if the tag is absent -
    // TIFFGetField() leaves the output untouched on a missing tag, so pre-seeding it
    // is what makes that the correct fallback.
    uint16_t orientation = ORIENTATION_TOPLEFT;
    TIFFGetField(tif, TIFFTAG_ORIENTATION, &orientation);

    width = (int)w;
    height = (int)h;
    if (orientation >= 5) std::swap(width, height); // 5-8 rotate 90deg, same as EXIF

    TIFFClose(tif);
    return width > 0 && height > 0;
}

} // namespace pixet
