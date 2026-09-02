#include "ThumbGenerator.h"

#include <utility>

#include "../decode/AvifCodec.h"
#include "../decode/HeifCodec.h"
#include "../decode/JpegCodec.h"
#include "../decode/PngCodec.h"
#include "../cache/RawCache.h"
#include "../decode/RawCodec.h"
#include "../decode/TiffCodec.h"
#include "../decode/VideoCodec.h"
#include "../decode/WebpCodec.h"
#include "../decode/DecodeLimits.h"
#include "../meta/JpegExif.h"
#include "../util/FileIO.h"
#include "../util/FileMove.h" // statFile, for the RAW cache key and the size gate
#include "../util/Shutdown.h"

#include "../util/Profile.h"

namespace pixet {

namespace {

ThumbResult generateJpegThumb(const std::vector<uint8_t> &fileBytes, int targetLongEdge, int quality) {
    ThumbResult result;

    PIXET_PROF_SCOPE("gen.jpeg.total");
    ExifInfo exif;
    {
        PIXET_PROF_SCOPE("gen.jpeg.exifParse");
        exif = parseJpegExif(fileBytes.data(), fileBytes.size());
    }
    result.orientation = exif.orientation;

    // Always read the *original* file's header for its true native dimensions -
    // independent of whatever size the thumbnail decode below actually lands on
    // (embedded EXIF previews are typically ~160px; scaled-DCT decodes land near
    // targetLongEdge; neither is the real image size). A header-only read is cheap
    // (no pixel decode), so there's no cost to doing this unconditionally.
    if (readJpegDimensions(fileBytes.data(), fileBytes.size(), result.origWidth, result.origHeight) &&
        exif.orientation >= 5) {
        std::swap(result.origWidth, result.origHeight); // EXIF 5-8 rotate 90deg
    }

    // Pull GPS out while the bytes are already in memory. The grid's geotag marker needs the
    // answer for every visible cell on every repaint, so it has to be stored rather than read
    // on demand - and doing it here costs a second IFD walk on a buffer we already hold,
    // against re-reading every original later just to answer the same question.
    {
        PIXET_PROF_SCOPE("gen.jpeg.exifDetails");
        ExifDetails details = parseJpegExifDetails(fileBytes.data(), fileBytes.size());
        result.gpsChecked = true;
        result.hasGps = details.hasGps;
        result.gpsLatitude = details.gpsLatitude;
        result.gpsLongitude = details.gpsLongitude;
    }

    RgbImage img;
    bool decoded = false;

    // Take the embedded preview only when it's actually large enough for what was asked for.
    //
    // decodeJpeg downscales but never upscales, so a typical ~160px EXIF thumbnail caps the
    // result at 160px however big targetLongEdge is. Taking the preview unconditionally would
    // make that permanent rather than merely disappointing: the preview is always present, so
    // re-thumbnailing the file at a larger size would regenerate the very same small image,
    // and no amount of "Force Re-thumbnail" could ever improve it. Checked header-only, so
    // rejecting a preview costs no pixel decode.
    bool hadSmallThumb = false;
    if (exif.hasThumb()) {
        int thumbW = 0, thumbH = 0;
        if (readJpegDimensions(fileBytes.data() + exif.thumbOffset, exif.thumbLength, thumbW, thumbH) &&
            (thumbW >= targetLongEdge || thumbH >= targetLongEdge)) {
            decoded = decodeJpeg(fileBytes.data() + exif.thumbOffset, exif.thumbLength, targetLongEdge, img);
            if (decoded) result.tier = ThumbTier::EmbeddedPreview;
        } else {
            hadSmallThumb = true;
        }
    }
    if (!decoded) {
        PIXET_PROF_SCOPE("gen.jpeg.fullDecode");
        PIXET_PROF_COUNT("gen.jpeg.fullDecodeBytes", fileBytes.size());
        decoded = decodeJpeg(fileBytes.data(), fileBytes.size(), targetLongEdge, img);
        if (decoded) result.tier = ThumbTier::Decoded;
    }
    // A preview rejected as too small still beats no thumbnail at all, so it stays the last
    // resort for a file whose full decode fails - truncated or corrupt data with an intact
    // EXIF thumbnail.
    if (!decoded && hadSmallThumb) {
        decoded = decodeJpeg(fileBytes.data() + exif.thumbOffset, exif.thumbLength, targetLongEdge, img);
        if (decoded) result.tier = ThumbTier::EmbeddedPreview;
    }
    if (!decoded) {
        result.tier = ThumbTier::Failed;
        return result;
    }

    {
        PIXET_PROF_SCOPE("gen.jpeg.applyOrientation");
        applyOrientation(img, exif.orientation);
    }
    PIXET_PROF_COUNT("gen.jpeg.decodedPixels", (int64_t)img.w * img.h);

    RgbImage resized;
    {
        PIXET_PROF_SCOPE("gen.jpeg.resize");
        resizeBoxDownscale(img, targetLongEdge, resized);
    }

    PIXET_PROF_SCOPE("gen.jpeg.encode");
    if (!encodeJpeg(resized, quality, result.jpegBytes)) {
        result.tier = ThumbTier::Failed;
        return result;
    }

    result.width = resized.w;
    result.height = resized.h;
    return result;
}

ThumbResult generatePngThumb(const std::vector<uint8_t> &fileBytes, int targetLongEdge, int quality) {
    ThumbResult result;

    // No orientation extraction for PNG: unlike JPEG, there's no widely-used
    // equivalent in practice - PNG's usual sources (screenshots, graphics, web
    // exports) essentially never carry rotation metadata, so result.orientation
    // stays at its default (1, "no rotation") rather than adding a PNG eXIf-chunk
    // parser for a case that's vanishingly rare in a real photo library.
    readPngDimensions(fileBytes.data(), fileBytes.size(), result.origWidth, result.origHeight);

    // The codec would refuse this too (see the guard in PngCodec.cpp), but only by returning false,
    // which is indistinguishable from a corrupt file and would record the row as Failed.
    // The dimensions are already in hand from the header read just above, so decide it
    // here instead and report it for what it is: a file we have chosen not to decode, not
    // one we tried and could not. Costs nothing - readTiffDimensions and friends have
    // already done the only work involved.
    if (!decodelimits::pixelsAllowed(result.origWidth, result.origHeight)) {
        result.tier = ThumbTier::Unsupported;
        return result;
    }

    // libpng has no scaled/progressive decode like libjpeg's DCT scaling - always
    // decodes at native resolution, so there's no embedded-preview or partial-decode
    // tier here, just a straight decode-then-downscale.
    RgbImage img;
    if (!decodePng(fileBytes.data(), fileBytes.size(), img)) {
        result.tier = ThumbTier::Failed;
        return result;
    }

    RgbImage resized;
    resizeBoxDownscale(img, targetLongEdge, resized);

    if (!encodeJpeg(resized, quality, result.jpegBytes)) {
        result.tier = ThumbTier::Failed;
        return result;
    }

    result.tier = ThumbTier::Decoded;
    result.width = resized.w;
    result.height = resized.h;
    return result;
}

ThumbResult generateTiffThumb(const std::vector<uint8_t> &fileBytes, int targetLongEdge, int quality) {
    ThumbResult result;

    // decodeTiff() already auto-corrects for the file's own orientation tag (see
    // TiffCodec.h) - orientation stays at its default (1, "no further rotation
    // needed"), unlike JPEG's explicit applyOrientation() step. readTiffDimensions()
    // accounts for that same rotation when reporting origWidth/origHeight.
    readTiffDimensions(fileBytes.data(), fileBytes.size(), result.origWidth, result.origHeight);

    // The codec would refuse this too (see the guard in TiffCodec.cpp), but only by returning false,
    // which is indistinguishable from a corrupt file and would record the row as Failed.
    // The dimensions are already in hand from the header read just above, so decide it
    // here instead and report it for what it is: a file we have chosen not to decode, not
    // one we tried and could not. Costs nothing - readTiffDimensions and friends have
    // already done the only work involved.
    if (!decodelimits::pixelsAllowed(result.origWidth, result.origHeight)) {
        result.tier = ThumbTier::Unsupported;
        return result;
    }

    // No scaled/progressive decode for TIFF either - always native resolution, then
    // downscale.
    RgbImage img;
    if (!decodeTiff(fileBytes.data(), fileBytes.size(), img)) {
        result.tier = ThumbTier::Failed;
        return result;
    }

    RgbImage resized;
    resizeBoxDownscale(img, targetLongEdge, resized);

    if (!encodeJpeg(resized, quality, result.jpegBytes)) {
        result.tier = ThumbTier::Failed;
        return result;
    }

    result.tier = ThumbTier::Decoded;
    result.width = resized.w;
    result.height = resized.h;
    return result;
}

ThumbResult generateWebpThumb(const std::vector<uint8_t> &fileBytes, int targetLongEdge, int quality) {
    ThumbResult result;

    // No orientation extraction for WebP: it can technically carry EXIF metadata (via
    // the extended VP8X container format), but that's rare in practice - most WebP
    // files are web-optimized graphics/photos without camera EXIF - so this skips it
    // for the same reason PNG does (see generatePngThumb).
    readWebpDimensions(fileBytes.data(), fileBytes.size(), result.origWidth, result.origHeight);

    // The codec would refuse this too (see the guard in WebpCodec.cpp), but only by returning false,
    // which is indistinguishable from a corrupt file and would record the row as Failed.
    // The dimensions are already in hand from the header read just above, so decide it
    // here instead and report it for what it is: a file we have chosen not to decode, not
    // one we tried and could not. Costs nothing - readTiffDimensions and friends have
    // already done the only work involved.
    if (!decodelimits::pixelsAllowed(result.origWidth, result.origHeight)) {
        result.tier = ThumbTier::Unsupported;
        return result;
    }

    RgbImage img;
    if (!decodeWebp(fileBytes.data(), fileBytes.size(), img)) {
        result.tier = ThumbTier::Failed;
        return result;
    }

    RgbImage resized;
    resizeBoxDownscale(img, targetLongEdge, resized);

    if (!encodeJpeg(resized, quality, result.jpegBytes)) {
        result.tier = ThumbTier::Failed;
        return result;
    }

    result.tier = ThumbTier::Decoded;
    result.width = resized.w;
    result.height = resized.h;
    return result;
}

ThumbResult generateAvifThumb(const std::vector<uint8_t> &fileBytes, int targetLongEdge, int quality) {
    ThumbResult result;

    // No orientation extraction for AVIF - see AvifCodec.h for why (irot/imir
    // transforms are uncommon in practice, same scope decision as PNG/WebP).
    readAvifDimensions(fileBytes.data(), fileBytes.size(), result.origWidth, result.origHeight);

    // The codec would refuse this too (see the guard in AvifCodec.cpp), but only by returning false,
    // which is indistinguishable from a corrupt file and would record the row as Failed.
    // The dimensions are already in hand from the header read just above, so decide it
    // here instead and report it for what it is: a file we have chosen not to decode, not
    // one we tried and could not. Costs nothing - readTiffDimensions and friends have
    // already done the only work involved.
    if (!decodelimits::pixelsAllowed(result.origWidth, result.origHeight)) {
        result.tier = ThumbTier::Unsupported;
        return result;
    }

    RgbImage img;
    if (!decodeAvif(fileBytes.data(), fileBytes.size(), img)) {
        result.tier = ThumbTier::Failed;
        return result;
    }

    RgbImage resized;
    resizeBoxDownscale(img, targetLongEdge, resized);

    if (!encodeJpeg(resized, quality, result.jpegBytes)) {
        result.tier = ThumbTier::Failed;
        return result;
    }

    result.tier = ThumbTier::Decoded;
    result.width = resized.w;
    result.height = resized.h;
    return result;
}

ThumbResult generateHeifThumb(const std::vector<uint8_t> &fileBytes, int targetLongEdge, int quality) {
    ThumbResult result;

    // libheif applies the container's own orientation transformations by default -
    // orientation stays at its default (1, "no further rotation needed"), unlike
    // JPEG's explicit applyOrientation() step, same as RAW/TIFF. readHeifDimensions()
    // reads the already-transformed handle dimensions, so origWidth/origHeight are
    // consistent with what actually gets decoded.
    readHeifDimensions(fileBytes.data(), fileBytes.size(), result.origWidth, result.origHeight);

    RgbImage img;
    bool decoded = decodeHeifThumb(fileBytes.data(), fileBytes.size(), img);
    if (decoded) result.tier = ThumbTier::EmbeddedPreview;
    if (!decoded) {
        decoded = decodeHeif(fileBytes.data(), fileBytes.size(), img);
        if (decoded) result.tier = ThumbTier::Decoded;
    }
    if (!decoded) {
        // No up-front pixel gate here, unlike the four formats above, and the difference is
        // deliberate: a HEIC's embedded preview is a genuinely cheap decode of its own,
        // bounded by the preview's size rather than the main image's, so an enormous HEIC
        // can still get a perfectly good thumbnail out of it. Refusing on the main image's
        // dimensions before trying would throw that away for nothing. Only once the preview
        // has come back empty is the size worth reporting - and then it explains the
        // failure, so it's Unsupported rather than Failed (decodeHandle's own guard is what
        // actually refused the full decode).
        result.tier = decodelimits::pixelsAllowed(result.origWidth, result.origHeight) ? ThumbTier::Failed
                                                                                        : ThumbTier::Unsupported;
        return result;
    }

    RgbImage resized;
    resizeBoxDownscale(img, targetLongEdge, resized);

    if (!encodeJpeg(resized, quality, result.jpegBytes)) {
        result.tier = ThumbTier::Failed;
        return result;
    }

    result.width = resized.w;
    result.height = resized.h;
    return result;
}

ThumbResult generateRawThumb(const std::string &filePath, const std::vector<uint8_t> &fileBytes,
                              int targetLongEdge, int quality, bool forceFullRender) {
    ThumbResult result;

    // decodeRaw() (full demosaic) applies the file's embedded orientation to its
    // output automatically via LibRaw's own pipeline; decodeRawThumb() (embedded
    // preview) does its own explicit applyOrientation() call instead, same idea as
    // JPEG's, since it decodes the embedded preview JPEG's raw bytes directly rather
    // than going through that pipeline - see RawCodec.cpp. readRawDimensions()
    // already accounts for the same rotation when reporting origWidth/origHeight.
    readRawDimensions(fileBytes.data(), fileBytes.size(), result.origWidth, result.origHeight);

    RgbImage img;
    bool decoded = false;
    if (!forceFullRender) {
        decoded = decodeRawThumb(fileBytes.data(), fileBytes.size(), targetLongEdge, img);
        if (decoded) result.tier = ThumbTier::EmbeddedPreview;
    }
    if (!decoded) {
        decoded = decodeRaw(fileBytes.data(), fileBytes.size(), img);
        if (decoded) {
            result.tier = ThumbTier::Decoded;
            // Hand the full decode to the display cache on the way past. This is the same
            // demosaic the preview pane and the fullscreen viewer would otherwise each have
            // to do for themselves, and it is by far the most expensive thing in the app -
            // paying for it once, here, is what makes a rendered RAW open in colour
            // immediately rather than after a second wait. The cache downscales to its own
            // configured size, so this does not store a 24MP image.
            int64_t size = 0, mtime = 0;
            if (statFile(filePath, &size, &mtime)) rawcache::store(filePath, mtime, size, img);
        }
    }
    if (!decoded) {
        result.tier = ThumbTier::Failed;
        return result;
    }

    RgbImage resized;
    resizeBoxDownscale(img, targetLongEdge, resized);

    if (!encodeJpeg(resized, quality, result.jpegBytes)) {
        result.tier = ThumbTier::Failed;
        return result;
    }

    result.width = resized.w;
    result.height = resized.h;
    return result;
}

// The embedded-preview rung for RAW, taking the path rather than a pre-read buffer so
// LibRaw reads only the header and the preview itself. See decodeRawThumbFromFile() for the
// numbers; on a spinning disk this is where nearly all of a RAW folder's indexing time goes.
//
// Returns Failed when there is no usable embedded preview, which is a signal rather than an
// error: the caller falls back to the whole-file path, whose full demosaic needs every byte
// anyway.
ThumbResult generateRawThumbFromFile(const std::string &filePath, int targetLongEdge, int quality) {
    ThumbResult result;

    RgbImage img;
    if (!decodeRawThumbFromFile(filePath, targetLongEdge, img, result.origWidth, result.origHeight)) {
        result.tier = ThumbTier::Failed;
        return result;
    }

    RgbImage resized;
    resizeBoxDownscale(img, targetLongEdge, resized);

    if (!encodeJpeg(resized, quality, result.jpegBytes)) {
        result.tier = ThumbTier::Failed;
        return result;
    }

    result.tier = ThumbTier::EmbeddedPreview;
    result.width = resized.w;
    result.height = resized.h;
    return result;
}

// Unlike the image formats above, takes the path directly rather than a pre-read
// buffer - see VideoCodec.h for why (video files are too large to read wholesale just
// to grab a poster frame a few seconds in).
ThumbResult generateVideoThumb(const std::string &filePath, int targetLongEdge, int quality) {
    ThumbResult result;

    RgbImage img;
    if (!decodeVideoPosterFrame(filePath, img)) {
        result.tier = ThumbTier::Failed;
        return result;
    }

    // The poster frame's own (rotation-corrected) size doubles as "the original
    // dimensions" here - video has no separate scaled-decode tier that could land at
    // some other intermediate size the way a photo's thumbnail decode can, so there's
    // no distinct "true native size" to read separately the way readJpegDimensions()/
    // readRawDimensions() do.
    result.origWidth = img.w;
    result.origHeight = img.h;

    RgbImage resized;
    resizeBoxDownscale(img, targetLongEdge, resized);

    if (!encodeJpeg(resized, quality, result.jpegBytes)) {
        result.tier = ThumbTier::Failed;
        return result;
    }

    result.tier = ThumbTier::Decoded;
    result.width = resized.w;
    result.height = resized.h;
    return result;
}

// How much of a file to read when only its header is wanted. Generous next to what any
// of these headers actually need (a PNG's IHDR is 33 bytes in), because the point is to
// cover the common case where a TIFF's first IFD sits a little way into the file, and 1MB
// against a potential 14GB is not a tradeoff worth tuning.
constexpr size_t kHeaderProbeBytes = 1024 * 1024;

// True only when a bounded prefix is enough to prove the image is over the pixel limit.
//
// Exists because the pixel checks in generateXThumb() below all happen *after* the whole
// file has been read - they bound the decode, which is most of the cost, but not the read.
// With the file-size limit at its default that never matters, since anything big enough to
// care about was already refused on its size. But that limit is a user setting and "no
// limit" is one of its choices, and someone who picks it to open their 800MB panoramas
// should not thereby be signing up to read a 14GB file whole before refusing it anyway.
//
// Deliberately conservative in both directions. A header that isn't in the prefix (a TIFF
// whose IFD is at the end of the file is entirely legal, and container formats can put
// their metadata after the payload) simply doesn't answer, and the file goes down the
// normal path - so this can make things faster but never changes a verdict. And a `false`
// means "not proven oversized", not "fine": every later check still runs.
bool provablyOverPixelLimit(const std::string &filePath, Format fmt) {
    switch (fmt) {
        case Format::Png:
        case Format::Tiff:
        case Format::Webp:
        case Format::Avif:
            break;
        // JPEG and RAW are absent because neither reads the whole file to make a
        // thumbnail in the first place (scaled DCT decode; embedded preview straight from
        // the path), and HEIC because its embedded preview is cheap regardless of how big
        // the main image is - refusing here would throw away a perfectly good thumbnail.
        // See generateHeifThumb.
        default: return false;
    }

    if (decodelimits::maxPixels() <= 0) return false; // no limit configured - nothing to prove

    std::vector<uint8_t> prefix;
    if (!readFilePrefix(filePath, kHeaderProbeBytes, prefix) || prefix.empty()) return false;

    int w = 0, h = 0;
    bool known = false;
    switch (fmt) {
        case Format::Png: known = readPngDimensions(prefix.data(), prefix.size(), w, h); break;
        case Format::Tiff: known = readTiffDimensions(prefix.data(), prefix.size(), w, h); break;
        case Format::Webp: known = readWebpDimensions(prefix.data(), prefix.size(), w, h); break;
        case Format::Avif: known = readAvifDimensions(prefix.data(), prefix.size(), w, h); break;
        default: break;
    }
    return known && !decodelimits::pixelsAllowed(w, h);
}

} // namespace

ThumbResult generateThumb(const std::string &filePath, Format fmt, int targetLongEdge, int quality,
                           bool forceFullRender) {
    // First thing, before any I/O: a Pass B wave has already been dispatched to the thread
    // pool by the time a quit arrives, and ThreadPool deliberately runs every queued task
    // rather than dropping them (see util/ThreadPool.cpp), so without this the app spends
    // another whole wave of decodes on its way out - with the UI thread blocked in a
    // destructor waiting for exactly that. See util/Shutdown.h.
    if (shutdownRequested()) {
        ThumbResult result;
        result.tier = ThumbTier::Cancelled;
        return result;
    }

    // Video is handled before the whole-file-read below (it takes the path directly),
    // and before the format gate too (Video isn't in the Jpeg/Png/Raw list there).
    if (fmt == Format::Video) return generateVideoThumb(filePath, targetLongEdge, quality);

    switch (fmt) {
        case Format::Jpeg:
        case Format::Png:
        case Format::Raw:
        case Format::Tiff:
        case Format::Webp:
        case Format::Avif:
        case Format::Heic:
            break;
        default: {
            ThumbResult result;
            result.tier = ThumbTier::Unsupported;
            return result;
        }
    }

    // RAW takes the path rather than a pre-read buffer, for the same reason video does just
    // above and to a far greater degree: the embedded preview sits in the first fraction of
    // the file, so reading the whole thing to reach it costs ~65x the I/O it needs. A miss
    // here (no usable preview, or an unreadable file) falls through to the whole-file read
    // below, which the full-demosaic fallback needs anyway - so nothing is lost by trying.
    if (fmt == Format::Raw && !forceFullRender) {
        ThumbResult result = generateRawThumbFromFile(filePath, targetLongEdge, quality);
        if (result.tier != ThumbTier::Failed) return result;
    }

    // Everything from here on reads the file whole, which is the point at which a file's
    // size stops being the filesystem's problem and becomes ours - and on a Downloads
    // folder rather than a photo library, "a .tif" can mean 14 GiB. Gated from a stat, so
    // an over-size file costs one syscall instead of its own weight in RAM.
    //
    // Reported as Unsupported rather than Failed: this is a decision about the file, and
    // recording it keeps every later scan from re-discovering it. Deliberately after the
    // RAW embedded-preview attempt above, which reads only a header and a preview however
    // large the file is, so a big RAW still gets its cheap thumbnail.
    {
        int64_t sizeBytes = 0, mtimeUnix = 0;
        if (statFile(filePath, &sizeBytes, &mtimeUnix) && !decodelimits::fileSizeAllowed(sizeBytes)) {
            ThumbResult result;
            result.tier = ThumbTier::Unsupported;
            return result;
        }
    }

    // Second gate, on the other limit, from a 1MB prefix rather than the whole file - see
    // provablyOverPixelLimit(). Only useful when the size gate above has been widened or
    // turned off, which is exactly when it is needed.
    if (provablyOverPixelLimit(filePath, fmt)) {
        ThumbResult result;
        result.tier = ThumbTier::Unsupported;
        return result;
    }

    std::vector<uint8_t> fileBytes;
    bool readOk;
    {
        PIXET_PROF_SCOPE("gen.readWholeFile");
        readOk = readWholeFile(filePath, fileBytes);
    }
    PIXET_PROF_COUNT("gen.readBytes", fileBytes.size());
    if (!readOk) {
        ThumbResult result;
        result.tier = ThumbTier::Failed;
        return result;
    }

    switch (fmt) {
        case Format::Jpeg: return generateJpegThumb(fileBytes, targetLongEdge, quality);
        case Format::Png: return generatePngThumb(fileBytes, targetLongEdge, quality);
        case Format::Raw: return generateRawThumb(filePath, fileBytes, targetLongEdge, quality, forceFullRender);
        case Format::Tiff: return generateTiffThumb(fileBytes, targetLongEdge, quality);
        case Format::Webp: return generateWebpThumb(fileBytes, targetLongEdge, quality);
        case Format::Avif: return generateAvifThumb(fileBytes, targetLongEdge, quality);
        case Format::Heic: return generateHeifThumb(fileBytes, targetLongEdge, quality);
        default: break; // unreachable - filtered out above
    }
    ThumbResult result;
    result.tier = ThumbTier::Unsupported;
    return result;
}

} // namespace pixet
