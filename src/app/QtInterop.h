#pragma once

#include <QImage>

#include "decode/JpegCodec.h"

// Converts a decoded RgbImage to a QImage (deep copy - RgbImage's buffer doesn't
// outlive the call). Kept as one shared helper since both ThumbLoader and
// PreviewDecoder need it, decoding through pixet_core's own JPEG codec rather than
// Qt's image-format plugins - avoids a runtime plugin-discovery dependency and keeps
// preview/thumbnail decode on the same code path the indexer itself uses.
inline QImage rgbImageToQImage(const pixet::RgbImage &img) {
    if (img.empty()) return QImage();
    QImage qimg(img.pixels.data(), img.w, img.h, img.w * 3, QImage::Format_RGB888);
    return qimg.copy();
}
