#include "RgbImage.h"

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <utility>

extern "C" {
#include <jpeglib.h>
}

namespace pixet {

namespace {

struct JpegErrorMgr {
    jpeg_error_mgr pub;
    jmp_buf jmp;
};

void jpegErrorExit(j_common_ptr cinfo) {
    auto *err = reinterpret_cast<JpegErrorMgr *>(cinfo->err);
    longjmp(err->jmp, 1);
}

// libjpeg's default output_message() writes warnings to stderr; invisible in a
// WIN32-subsystem app and just noise over a scan of a large real library.
void silentOutputMessage(j_common_ptr) {}

} // namespace

void resizeBoxDownscale(const RgbImage &src, int targetLongEdge, RgbImage &dst) {
    int srcLong = std::max(src.w, src.h);
    if (srcLong <= targetLongEdge || srcLong == 0) {
        dst = src;
        return;
    }

    double scale = (double)targetLongEdge / (double)srcLong;
    int dstW = std::max(1, (int)std::lround(src.w * scale));
    int dstH = std::max(1, (int)std::lround(src.h * scale));

    RgbImage result;
    result.w = dstW;
    result.h = dstH;
    result.pixels.assign((size_t)dstW * dstH * 3, 0);

    for (int y = 0; y < dstH; ++y) {
        int sy0 = (int)((int64_t)y * src.h / dstH);
        int sy1 = (int)((int64_t)(y + 1) * src.h / dstH);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > src.h) sy1 = src.h;

        for (int x = 0; x < dstW; ++x) {
            int sx0 = (int)((int64_t)x * src.w / dstW);
            int sx1 = (int)((int64_t)(x + 1) * src.w / dstW);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > src.w) sx1 = src.w;

            long sumR = 0, sumG = 0, sumB = 0;
            int count = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                const uint8_t *row = src.pixels.data() + (size_t)sy * src.w * 3;
                for (int sx = sx0; sx < sx1; ++sx) {
                    sumR += row[sx * 3 + 0];
                    sumG += row[sx * 3 + 1];
                    sumB += row[sx * 3 + 2];
                    ++count;
                }
            }
            uint8_t *d = result.pixels.data() + (size_t)(y * dstW + x) * 3;
            d[0] = (uint8_t)(sumR / count);
            d[1] = (uint8_t)(sumG / count);
            d[2] = (uint8_t)(sumB / count);
        }
    }

    dst = std::move(result);
}

void applyOrientation(RgbImage &img, int orientation) {
    if (orientation <= 1 || orientation > 8 || img.empty()) return;

    int w = img.w, h = img.h;
    bool swapDims = orientation >= 5;
    int newW = swapDims ? h : w;
    int newH = swapDims ? w : h;

    std::vector<uint8_t> newPixels((size_t)newW * newH * 3);

    for (int y = 0; y < h; ++y) {
        const uint8_t *srcRow = img.pixels.data() + (size_t)y * w * 3;
        for (int x = 0; x < w; ++x) {
            const uint8_t *s = srcRow + (size_t)x * 3;
            int dx, dy;
            switch (orientation) {
                case 2: dx = w - 1 - x; dy = y; break;             // mirror horizontal
                case 3: dx = w - 1 - x; dy = h - 1 - y; break;     // rotate 180
                case 4: dx = x; dy = h - 1 - y; break;             // mirror vertical
                case 5: dx = y; dy = x; break;                     // transpose
                case 6: dx = h - 1 - y; dy = x; break;             // rotate 90 CW
                case 7: dx = h - 1 - y; dy = w - 1 - x; break;     // anti-transpose
                case 8: dx = y; dy = w - 1 - x; break;             // rotate 90 CCW
                default: dx = x; dy = y; break;
            }
            uint8_t *d = newPixels.data() + (size_t)(dy * newW + dx) * 3;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
        }
    }

    img.w = newW;
    img.h = newH;
    img.pixels = std::move(newPixels);
}

bool encodeJpeg(const RgbImage &img, int quality, std::vector<uint8_t> &out) {
    if (img.empty()) return false;

    jpeg_compress_struct cinfo;
    JpegErrorMgr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpegErrorExit;
    jerr.pub.output_message = silentOutputMessage;

    unsigned char *outBuf = nullptr;
    unsigned long outSize = 0;

    if (setjmp(jerr.jmp)) {
        jpeg_destroy_compress(&cinfo);
        if (outBuf) free(outBuf);
        return false;
    }

    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &outBuf, &outSize);

    cinfo.image_width = img.w;
    cinfo.image_height = img.h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);

    jpeg_start_compress(&cinfo, TRUE);
    JSAMPROW rowPtr[1];
    while (cinfo.next_scanline < cinfo.image_height) {
        rowPtr[0] = (JSAMPLE *)(img.pixels.data() + (size_t)cinfo.next_scanline * img.w * 3);
        jpeg_write_scanlines(&cinfo, rowPtr, 1);
    }
    jpeg_finish_compress(&cinfo);

    out.assign(outBuf, outBuf + outSize);
    jpeg_destroy_compress(&cinfo);
    free(outBuf);
    return true;
}

} // namespace pixet
