#include "JpegCodec.h"

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdlib>
#include <cstring>

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

// libjpeg's default output_message() writes warnings to stderr - over a scan of a large
// real library that's noise we don't want drowning out progress output.
void silentOutputMessage(j_common_ptr) {}

} // namespace

bool decodeJpeg(const uint8_t *data, size_t size, int targetLongEdge, RgbImage &out) {
    if (size == 0) return false;

    jpeg_decompress_struct cinfo;
    JpegErrorMgr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpegErrorExit;
    jerr.pub.output_message = silentOutputMessage;

    if (setjmp(jerr.jmp)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, (unsigned long)size);
    jpeg_read_header(&cinfo, TRUE);

    int longEdge = std::max((int)cinfo.image_width, (int)cinfo.image_height);
    int denom = 1;
    if (targetLongEdge > 0) {
        for (int d : {8, 4, 2}) {
            if (longEdge / d >= targetLongEdge) {
                denom = d;
                break;
            }
        }
    }
    cinfo.scale_num = 1;
    cinfo.scale_denom = denom;
    cinfo.out_color_space = JCS_RGB;

    jpeg_start_decompress(&cinfo);

    out.w = (int)cinfo.output_width;
    out.h = (int)cinfo.output_height;
    out.pixels.assign((size_t)out.w * out.h * 3, 0);

    int channels = cinfo.output_components;
    std::vector<JSAMPLE> rowBuf((size_t)out.w * channels);
    JSAMPROW rowPtr[1];

    int row = 0;
    while (cinfo.output_scanline < cinfo.output_height) {
        rowPtr[0] = rowBuf.data();
        jpeg_read_scanlines(&cinfo, rowPtr, 1);
        uint8_t *dst = out.pixels.data() + (size_t)row * out.w * 3;
        if (channels == 3) {
            std::memcpy(dst, rowBuf.data(), (size_t)out.w * 3);
        } else {
            for (int x = 0; x < out.w; ++x) {
                uint8_t v = rowBuf[(size_t)x * channels];
                dst[x * 3 + 0] = dst[x * 3 + 1] = dst[x * 3 + 2] = v;
            }
        }
        ++row;
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

bool readJpegDimensions(const uint8_t *data, size_t size, int &width, int &height) {
    if (size == 0) return false;

    jpeg_decompress_struct cinfo;
    JpegErrorMgr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpegErrorExit;
    jerr.pub.output_message = silentOutputMessage;

    if (setjmp(jerr.jmp)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, (unsigned long)size);
    jpeg_read_header(&cinfo, TRUE);

    width = (int)cinfo.image_width;
    height = (int)cinfo.image_height;

    jpeg_destroy_decompress(&cinfo);
    return true;
}

} // namespace pixet
