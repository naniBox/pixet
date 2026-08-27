#include "RawCodec.h"

#include <utility>

#include <libraw/libraw.h>

#ifdef _WIN32
#include "../util/StringUtil.h"
#endif

#include "JpegCodec.h"
#include "RgbImage.h"

namespace pixet {

namespace {

// LibRaw's open_buffer() takes a non-const pointer even though it only reads - it
// never mutates the caller's buffer.
void *nonConst(const uint8_t *data) { return const_cast<uint8_t *>(data); }

// LibRaw/dcraw's "flip" encoding (0/1/2/3/4/5/6/7) is not EXIF's orientation encoding
// (1-8), even though both describe the same eight cases - see tiff.cpp's
// `t_flip = "50132467"[exifOrientation & 7]` table, which this is the inverse of.
// Needed because decodeRawThumb(), unlike decodeRaw(), decodes the embedded preview
// JPEG directly rather than going through LibRaw's own dcraw_process()/output_flip
// pipeline, so nothing rotates it to upright automatically.
int exifOrientationForFlip(int flip) {
    switch (flip) {
        case 0: return 1;
        case 1: return 2;
        case 2: return 4;
        case 3: return 3;
        case 4: return 5;
        case 5: return 8;
        case 6: return 6;
        case 7: return 7;
        default: return 1;
    }
}

// Dimensions from an already-opened LibRaw. open_file()/open_buffer() alone (no unpack())
// has already parsed the header, so this is pure field reading.
bool dimensionsFrom(LibRaw &raw, int &width, int &height) {
    width = raw.imgdata.sizes.width;
    height = raw.imgdata.sizes.height;
    // sizes.flip uses LibRaw's own encoding (0 = none, 3 = 180, 5 = 90 CCW, 6 = 90 CW),
    // not EXIF's - a 90-degree rotation swaps which dimension is "long".
    if (raw.imgdata.sizes.flip == 5 || raw.imgdata.sizes.flip == 6) std::swap(width, height);
    return width > 0 && height > 0;
}

// Everything after unpack_thumb(), shared by the buffer and file entry points so the two
// can never drift on orientation handling or on which thumbnail types are accepted.
bool extractUnpackedThumb(LibRaw &raw, int targetLongEdge, RgbImage &out) {
    int err = 0;
    libraw_processed_image_t *thumb = raw.dcraw_make_mem_thumb(&err);
    if (!thumb) return false;

    bool ok = false;
    if (thumb->type == LIBRAW_IMAGE_JPEG) {
        // The overwhelmingly common case: the embedded preview already is a JPEG -
        // decode it with our own cheap scaled-DCT path rather than a generic bitmap
        // copy, so a large (often near-full-resolution) embedded preview doesn't cost
        // a full-size decode either.
        ok = decodeJpeg(thumb->data, thumb->data_size, targetLongEdge, out);
    } else if (thumb->type == LIBRAW_IMAGE_BITMAP && thumb->width > 0 && thumb->height > 0) {
        out.w = thumb->width;
        out.h = thumb->height;
        size_t pixelCount = (size_t)thumb->width * thumb->height;
        if (thumb->colors == 3) {
            out.pixels.assign(thumb->data, thumb->data + pixelCount * 3);
            ok = true;
        } else if (thumb->colors == 1) {
            out.pixels.resize(pixelCount * 3);
            for (size_t i = 0; i < pixelCount; ++i) {
                uint8_t v = thumb->data[i];
                out.pixels[i * 3 + 0] = out.pixels[i * 3 + 1] = out.pixels[i * 3 + 2] = v;
            }
            ok = true;
        }
        // Any other channel count (e.g. an embedded CMYK thumbnail) is rare enough
        // not to special-case - falls through to decodeRaw() instead.
    }

    raw.dcraw_clear_mem(thumb);

    // The embedded JPEG's own bytes are stored as the sensor saw them, not upright -
    // decodeJpeg() above has no EXIF of its own to correct that (it's raw thumbnail
    // data extracted by LibRaw, not a standalone EXIF-bearing JPEG file), so apply the
    // file's own orientation (sizes.flip, same field readRawDimensions() reads)
    // ourselves. decodeRaw()'s dcraw_process() path does this internally instead.
    if (ok) applyOrientation(out, exifOrientationForFlip(raw.imgdata.sizes.flip));

    return ok;
}

// LibRaw's own wide-character overload on Windows. Its narrow open_file() interprets the
// path in the active ANSI codepage, not UTF-8 - the same trap FileIO_win.cpp exists to
// avoid - so a photo under a non-ASCII folder name would simply fail to open.
int openRawFile(LibRaw &raw, const std::string &pathUtf8) {
#ifdef _WIN32
    return raw.open_file(toUtf16(pathUtf8).c_str());
#else
    return raw.open_file(pathUtf8.c_str());
#endif
}

} // namespace

bool decodeRawThumb(const uint8_t *data, size_t size, int targetLongEdge, RgbImage &out) {
    LibRaw raw;
    if (raw.open_buffer(nonConst(data), size) != LIBRAW_SUCCESS) return false;
    if (raw.unpack_thumb() != LIBRAW_SUCCESS) return false;
    return extractUnpackedThumb(raw, targetLongEdge, out);
}

bool decodeRawThumbFromFile(const std::string &pathUtf8, int targetLongEdge, RgbImage &out, int &width,
                            int &height) {
    width = height = 0;

    LibRaw raw;
    if (openRawFile(raw, pathUtf8) != LIBRAW_SUCCESS) return false;
    // Before unpack_thumb(), so the caller still learns the file's real dimensions even when
    // there turns out to be no usable preview and it has to fall back to a full decode.
    dimensionsFrom(raw, width, height);

    if (raw.unpack_thumb() != LIBRAW_SUCCESS) return false;
    return extractUnpackedThumb(raw, targetLongEdge, out);
}

bool decodeRaw(const uint8_t *data, size_t size, RgbImage &out) {
    LibRaw raw;
    if (raw.open_buffer(nonConst(data), size) != LIBRAW_SUCCESS) return false;
    if (raw.unpack() != LIBRAW_SUCCESS) return false;

    raw.imgdata.params.output_bps = 8;
    raw.imgdata.params.output_color = 1; // sRGB
    // Fastest demosaic (bilinear) rather than LibRaw's higher-quality default (AHD) -
    // this only ever runs when there's no usable embedded preview, and the result
    // gets box-downscaled to a thumbnail immediately after anyway, so the extra
    // quality of a slower algorithm wouldn't survive to be visible.
    raw.imgdata.params.user_qual = 0;
    // -1 (the default) means "use the orientation embedded in the file" - LibRaw
    // applies it to the output automatically, so there's no separate orientation step
    // needed afterward here, unlike JPEG's explicit applyOrientation() call.
    raw.imgdata.params.user_flip = -1;

    if (raw.dcraw_process() != LIBRAW_SUCCESS) return false;

    int err = 0;
    libraw_processed_image_t *img = raw.dcraw_make_mem_image(&err);
    if (!img) return false;

    bool ok = false;
    if (img->type == LIBRAW_IMAGE_BITMAP && img->colors == 3 && img->bits == 8 && img->width > 0 &&
        img->height > 0) {
        out.w = img->width;
        out.h = img->height;
        out.pixels.assign(img->data, img->data + (size_t)img->width * img->height * 3);
        ok = true;
    }

    raw.dcraw_clear_mem(img);
    return ok;
}

bool readRawDimensions(const uint8_t *data, size_t size, int &width, int &height) {
    LibRaw raw;
    if (raw.open_buffer(nonConst(data), size) != LIBRAW_SUCCESS) return false;

    // open_buffer() alone (no unpack()) already parses the file's metadata/header,
    // including sizes.width/height - a genuine header-only read, no pixel decode.
    return dimensionsFrom(raw, width, height);
}

} // namespace pixet
