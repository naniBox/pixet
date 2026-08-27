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
#include "../cache/RawCache.h"
#include "../meta/JpegExif.h"
#include "../util/FileIO.h"
#include "../util/FileMove.h"

namespace pixet {

bool decodeForDisplay(const std::string &filePath, Format fmt, int targetLongEdge, RgbImage &out) {
    // Identity for the RAW cache below: a decode is only reusable for the same bytes, and
    // (mtime, size) is the same cheap stand-in for "same bytes" that Pass B's freshness
    // check already trusts. Read once here rather than inside the Raw branch so the stat
    // happens before the file is read, not after.
    int64_t mtimeUnix = 0, sizeBytes = 0;
    statFile(filePath, &sizeBytes, &mtimeUnix);
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
        case Format::Raw: {
            // The cache comes *before* the embedded preview here, unlike every other format
            // in this switch, and the reason is not speed.
            //
            // A RAW's embedded preview is not a smaller version of the same picture - it is
            // the camera's own JPEG rendering, made with whatever picture profile was set at
            // the time. Shoot with a monochrome profile and the preview is black and white
            // while the sensor data underneath is colour. Preferring it "because it is big
            // enough" therefore shows a different photograph, not a cheaper one, and no
            // amount of screen size makes that the right answer.
            //
            // So: if a full decode already exists, that is the truth and it wins. Only when
            // there isn't one does the embedded preview get used, and then as a placeholder
            // to put something on screen immediately rather than as the final answer - the
            // background render (RawRenderer, IndexOptions::renderRaws) produces the real
            // decode shortly after, populates this cache, and the view is asked again.
            //
            // The size guard still applies. A request bigger than the configured entry size
            // (a 4K display against a 2560 setting) or the fullscreen viewer's 1:1 zoom,
            // which passes targetLongEdge <= 0 meaning native resolution, cannot be answered
            // from a cache entry without quietly answering a different question.
            const int cacheEdge = rawcache::cachedLongEdge();
            const bool cacheable = cacheEdge > 0 && targetLongEdge > 0 && targetLongEdge <= cacheEdge;
            if (cacheable) {
                if (rawcache::lookup(filePath, mtimeUnix, sizeBytes, out)) return true;
                // A miss goes straight to the demosaic rather than falling back to the
                // embedded preview. Falling back would be permanent: the preview is always
                // "big enough", so nothing would ever reach the decode, nothing would ever
                // populate the cache, and a monochrome-profile RAW would show as black and
                // white for good. Paying for the decode once, here, is what makes every
                // later view of this file instant *and* correct.
                if (!decodeRaw(fileBytes.data(), fileBytes.size(), out)) return false;
                rawcache::store(filePath, mtimeUnix, sizeBytes, out);
                return true;
            }

            // Not cacheable: either the cache is switched off, or this is the fullscreen
            // viewer asking for native resolution, which no entry can satisfy. The original
            // ladder applies - embedded preview if it is large enough for what was asked,
            // full decode otherwise.
            if (preferEmbeddedPreview && decodeRawThumb(fileBytes.data(), fileBytes.size(), targetLongEdge, out) &&
                bigEnough(out)) {
                return true;
            }
            return decodeRaw(fileBytes.data(), fileBytes.size(), out);
        }
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
