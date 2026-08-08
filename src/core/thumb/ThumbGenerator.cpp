#include "ThumbGenerator.h"

#include "../decode/JpegCodec.h"
#include "../meta/JpegExif.h"
#include "../util/FileIO.h"

namespace pixet {

ThumbResult generateThumb(const std::wstring &filePath, Format fmt, int targetLongEdge, int quality) {
    ThumbResult result;

    if (fmt != Format::Jpeg) {
        result.tier = ThumbTier::Unsupported;
        return result;
    }

    std::vector<uint8_t> fileBytes;
    if (!readWholeFile(filePath, fileBytes)) {
        result.tier = ThumbTier::Failed;
        return result;
    }

    ExifInfo exif = parseJpegExif(fileBytes.data(), fileBytes.size());
    result.orientation = exif.orientation;

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

} // namespace pixet
