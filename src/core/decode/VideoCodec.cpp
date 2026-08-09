#include "VideoCodec.h"

#include <algorithm>
#include <cmath>
#include <memory>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/display.h>
#include <libswscale/swscale.h>
}

#include "../util/StringUtil.h"

namespace pixet {

namespace {

struct FormatCtxDeleter {
    void operator()(AVFormatContext *ctx) const {
        if (ctx) avformat_close_input(&ctx);
    }
};
struct CodecCtxDeleter {
    void operator()(AVCodecContext *ctx) const { avcodec_free_context(&ctx); }
};
struct FrameDeleter {
    void operator()(AVFrame *f) const { av_frame_free(&f); }
};
struct PacketDeleter {
    void operator()(AVPacket *p) const { av_packet_free(&p); }
};
struct SwsCtxDeleter {
    void operator()(SwsContext *s) const { sws_freeContext(s); }
};

// av_display_rotation_get() returns how the display matrix rotates the frame,
// counter-clockwise; the correction needed to show it upright is the inverse
// (clockwise by the same amount). Reuses applyOrientation()'s EXIF-style orientation
// values (1/3/6/8 - the four pure-rotation cases, no mirroring) for that correction
// rather than writing a separate rotation transform.
int exifOrientationForRotation(double ccwRotationDegrees) {
    if (std::isnan(ccwRotationDegrees)) return 1;
    int cw = ((int)std::lround(-ccwRotationDegrees)) % 360;
    if (cw < 0) cw += 360;
    if (cw >= 315 || cw < 45) return 1;
    if (cw < 135) return 6;
    if (cw < 225) return 3;
    return 8;
}

} // namespace

bool decodeVideoPosterFrame(const std::wstring &filePath, RgbImage &out) {
    // FFmpeg's default logging is chatty (codec/container warnings straight to
    // stderr) - invisible in a WIN32-subsystem app and just noise over a scan of a
    // large real library, same rationale as libjpeg's silenced output_message().
    av_log_set_level(AV_LOG_QUIET);

    std::string path = toUtf8(filePath);

    AVFormatContext *fmtCtxRaw = nullptr;
    if (avformat_open_input(&fmtCtxRaw, path.c_str(), nullptr, nullptr) != 0) return false;
    std::unique_ptr<AVFormatContext, FormatCtxDeleter> fmtCtx(fmtCtxRaw);

    if (avformat_find_stream_info(fmtCtx.get(), nullptr) < 0) return false;

    int videoStreamIdx = av_find_best_stream(fmtCtx.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoStreamIdx < 0) return false;
    AVStream *stream = fmtCtx->streams[videoStreamIdx];

    // Skip demuxing/decoding audio and any other streams - only the one video stream
    // is needed.
    for (unsigned i = 0; i < fmtCtx->nb_streams; ++i) {
        if ((int)i != videoStreamIdx) fmtCtx->streams[i]->discard = AVDISCARD_ALL;
    }

    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) return false;

    std::unique_ptr<AVCodecContext, CodecCtxDeleter> codecCtx(avcodec_alloc_context3(codec));
    if (!codecCtx) return false;
    if (avcodec_parameters_to_context(codecCtx.get(), stream->codecpar) < 0) return false;
    if (avcodec_open2(codecCtx.get(), codec, nullptr) < 0) return false;

    // min(3s, 10% of duration) into the video, per the plan's poster-frame rule -
    // duration can be AV_NOPTS_VALUE/unknown for some containers, in which case just
    // use the fixed 3s target without the percentage cap.
    int64_t seekTarget = 3 * (int64_t)AV_TIME_BASE;
    if (fmtCtx->duration > 0) seekTarget = std::min(seekTarget, fmtCtx->duration / 10);
    av_seek_frame(fmtCtx.get(), -1, seekTarget, AVSEEK_FLAG_BACKWARD);

    std::unique_ptr<AVPacket, PacketDeleter> pkt(av_packet_alloc());
    std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
    if (!pkt || !frame) return false;

    bool gotFrame = false;
    while (av_read_frame(fmtCtx.get(), pkt.get()) >= 0) {
        if (pkt->stream_index == videoStreamIdx) {
            if (avcodec_send_packet(codecCtx.get(), pkt.get()) == 0 &&
                avcodec_receive_frame(codecCtx.get(), frame.get()) == 0) {
                gotFrame = true;
                av_packet_unref(pkt.get());
                break;
            }
        }
        av_packet_unref(pkt.get());
    }
    if (!gotFrame) return false;

    if (frame->width <= 0 || frame->height <= 0) return false;

    std::unique_ptr<SwsContext, SwsCtxDeleter> sws(sws_getContext(
        frame->width, frame->height, (AVPixelFormat)frame->format, frame->width, frame->height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!sws) return false;

    RgbImage decoded;
    decoded.w = frame->width;
    decoded.h = frame->height;
    decoded.pixels.resize((size_t)decoded.w * decoded.h * 3);
    uint8_t *dstData[1] = {decoded.pixels.data()};
    int dstLinesize[1] = {decoded.w * 3};
    if (sws_scale(sws.get(), frame->data, frame->linesize, 0, frame->height, dstData, dstLinesize) <= 0) {
        return false;
    }

    const AVPacketSideData *displaySd = av_packet_side_data_get(
        stream->codecpar->coded_side_data, stream->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    if (displaySd) {
        applyOrientation(decoded, exifOrientationForRotation(av_display_rotation_get((int32_t *)displaySd->data)));
    }

    out = std::move(decoded);
    return true;
}

} // namespace pixet
