#include "DisplayCodec.h"

#include <algorithm>

#include "AvifCodec.h"
#include "HeifCodec.h"
#include "JpegCodec.h"
#include "PngCodec.h"
#include "RawCodec.h"
#include "TiffCodec.h"
#include "VideoCodec.h"
#include "WebpCodec.h"
#include "../meta/JpegExif.h"
#include "../util/FileIO.h"

namespace pixet {

bool decodeForDisplay(const std::string &filePath, Format fmt, int targetLongEdge, RgbImage &out) {
    if (fmt == Format::Unknown) return false;

    if (fmt == Format::Video) {
        // decodeVideoPosterFrame() takes the path directly rather than a buffer (see
        // its own doc comment) - video files are too large to read wholesale just to
        // seek a few seconds in, unlike every image format below. No embedded-preview
        // tier to prefer either - it's already just the one extracted frame, at
        // whatever resolution the video itself is.
        RgbImage native;
        if (!decodeVideoPosterFrame(filePath, native)) return false;
        if (targetLongEdge > 0) {
            resizeBoxDownscale(native, targetLongEdge, out);
        } else {
            out = std::move(native); // "native resolution" (fullscreen zoom) - don't downscale
        }
        return true;
    }

    std::vector<uint8_t> fileBytes;
    if (!readWholeFile(filePath, fileBytes)) return false;

    // "Give me native resolution" (fullscreen zoom) means don't settle for an
    // embedded preview even though it would nominally satisfy a size check - it's
    // never actually full resolution.
    bool preferEmbeddedPreview = targetLongEdge > 0;

    // decodeJpeg()'s scaled-DCT step can only ever downscale - if the embedded preview
    // (JPEG's own EXIF thumbnail, typically ~160px; RAW/HEIC's embedded preview, often
    // near-full-size) turns out smaller than what was actually asked for, it lands at
    // its own native size instead of targetLongEdge. Accepting that silently is fine
    // for a small ask (a thumbnail-sized target easily clears a 160px EXIF thumb), but
    // for a large one (fullscreen fit, sized to the whole screen) it means visibly
    // settling for thumbnail quality - see the caller-supplied targetLongEdge at each
    // call site. Reject and fall through to the real decode instead of accepting
    // whatever came back too small to have been worth preferring in the first place.
    auto bigEnough = [targetLongEdge](const RgbImage &img) {
        return std::max(img.w, img.h) >= targetLongEdge;
    };

    switch (fmt) {
        case Format::Jpeg: {
            ExifInfo exif = parseJpegExif(fileBytes.data(), fileBytes.size());
            bool decoded = false;
            if (preferEmbeddedPreview && exif.hasThumb()) {
                decoded = decodeJpeg(fileBytes.data() + exif.thumbOffset, exif.thumbLength, targetLongEdge, out) &&
                          bigEnough(out);
            }
            if (!decoded) decoded = decodeJpeg(fileBytes.data(), fileBytes.size(), targetLongEdge, out);
            if (!decoded) return false;
            applyOrientation(out, exif.orientation);
            return true;
        }
        case Format::Png:
            return decodePng(fileBytes.data(), fileBytes.size(), out);
        case Format::Raw:
            if (preferEmbeddedPreview && decodeRawThumb(fileBytes.data(), fileBytes.size(), targetLongEdge, out) &&
                bigEnough(out)) {
                return true;
            }
            return decodeRaw(fileBytes.data(), fileBytes.size(), out);
        case Format::Tiff:
            return decodeTiff(fileBytes.data(), fileBytes.size(), out);
        case Format::Webp:
            return decodeWebp(fileBytes.data(), fileBytes.size(), out);
        case Format::Avif:
            return decodeAvif(fileBytes.data(), fileBytes.size(), out);
        case Format::Heic:
            if (preferEmbeddedPreview && decodeHeifThumb(fileBytes.data(), fileBytes.size(), out) &&
                bigEnough(out)) {
                return true;
            }
            return decodeHeif(fileBytes.data(), fileBytes.size(), out);
        default:
            return false;
    }
}

} // namespace pixet
