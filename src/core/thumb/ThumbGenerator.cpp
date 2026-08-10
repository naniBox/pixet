#include "ThumbGenerator.h"

#include <utility>

#include "../decode/AvifCodec.h"
#include "../decode/HeifCodec.h"
#include "../decode/JpegCodec.h"
#include "../decode/PngCodec.h"
#include "../decode/RawCodec.h"
#include "../decode/TiffCodec.h"
#include "../decode/VideoCodec.h"
#include "../decode/WebpCodec.h"
#include "../meta/JpegExif.h"
#include "../util/FileIO.h"

namespace pixet {

namespace {

ThumbResult generateJpegThumb(const std::vector<uint8_t> &fileBytes, int targetLongEdge, int quality) {
    ThumbResult result;

    ExifInfo exif = parseJpegExif(fileBytes.data(), fileBytes.size());
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

    RgbImage img;
    bool decoded = false;

    if (exif.hasThumb()) {
        decoded = decodeJpeg(fileBytes.data() + exif.thumbOffset, exif.thumbLength, targetLongEdge, img);
        if (decoded) result.tier = ThumbTier::EmbeddedPreview;
    }
    if (!decoded) {
        decoded = decodeJpeg(fileBytes.data(), fileBytes.size(), targetLongEdge, img);
        if (decoded) result.tier = ThumbTier::Decoded;
    }
    if (!decoded) {
        result.tier = ThumbTier::Failed;
        return result;
    }

    applyOrientation(img, exif.orientation);

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

ThumbResult generatePngThumb(const std::vector<uint8_t> &fileBytes, int targetLongEdge, int quality) {
    ThumbResult result;

    // No orientation extraction for PNG: unlike JPEG, there's no widely-used
    // equivalent in practice - PNG's usual sources (screenshots, graphics, web
    // exports) essentially never carry rotation metadata, so result.orientation
    // stays at its default (1, "no rotation") rather than adding a PNG eXIf-chunk
    // parser for a case that's vanishingly rare in a real photo library.
    readPngDimensions(fileBytes.data(), fileBytes.size(), result.origWidth, result.origHeight);

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

ThumbResult generateRawThumb(const std::vector<uint8_t> &fileBytes, int targetLongEdge, int quality,
                              bool forceFullRender) {
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
        if (decoded) result.tier = ThumbTier::Decoded;
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

} // namespace

ThumbResult generateThumb(const std::string &filePath, Format fmt, int targetLongEdge, int quality,
                           bool forceFullRender) {
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

    std::vector<uint8_t> fileBytes;
    if (!readWholeFile(filePath, fileBytes)) {
        ThumbResult result;
        result.tier = ThumbTier::Failed;
        return result;
    }

    switch (fmt) {
        case Format::Jpeg: return generateJpegThumb(fileBytes, targetLongEdge, quality);
        case Format::Png: return generatePngThumb(fileBytes, targetLongEdge, quality);
        case Format::Raw: return generateRawThumb(fileBytes, targetLongEdge, quality, forceFullRender);
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
