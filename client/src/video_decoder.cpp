/*
 * OHOS Scrcpy 客户端 - 视频解码实现
 * 使用 FFmpeg 解码 H.264 (支持硬件加速)
 */

#include "video_decoder.h"
#include "../../common/log.h"

#include <stdlib.h>
#include <string.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#define DECODER_TAG "Decoder"

typedef struct {
    AVCodecContext      *codecCtx;
    AVCodecParameters   *codecPar;
    AVFrame             *frame;
    AVFrame             *swFrame;
    AVPacket            *packet;
    DecodedFrameCallback callback;
    void                *userData;
    bool                 hwAccel;
    int                  width;
    int                  height;
    bool                 spsPpsSet;
} DecoderContext;

static DecoderContext g_decoderCtx = {0};

/* 查找最佳解码器 */
static const AVCodec *find_best_decoder(void)
{
    /* 尝试硬件加速解码 (VA-API for Linux, DXVA2/D3D11VA for Windows) */
    const char *hwDecoders[] = {
        "h264_vaapi",       /* Linux VA-API */
        "h264_vdpau",       /* Linux VDPAU */
        "h264_dxva2",       /* Windows DXVA2 */
        "h264_d3d11va",     /* Windows D3D11VA */
        "h264_videotoolbox", /* macOS VideoToolbox */
        NULL
    };

    for (int i = 0; hwDecoders[i]; i++) {
        const AVCodec *codec = avcodec_find_decoder_by_name(hwDecoders[i]);
        if (codec) {
            LOG_TAG_I(DECODER_TAG, "Using hardware decoder: %s", hwDecoders[i]);
            g_decoderCtx.hwAccel = true;
            return codec;
        }
    }

    /* 回退到软件解码 */
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (codec) {
        LOG_TAG_I(DECODER_TAG, "Using software H.264 decoder");
        g_decoderCtx.hwAccel = false;
    }

    return codec;
}

int video_decoder_create(void)
{
    memset(&g_decoderCtx, 0, sizeof(g_decoderCtx));

    const AVCodec *codec = find_best_decoder();
    if (!codec) {
        LOG_TAG_E(DECODER_TAG, "No H.264 decoder found");
        return -1;
    }

    g_decoderCtx.codecCtx = avcodec_alloc_context3(codec);
    if (!g_decoderCtx.codecCtx) {
        LOG_TAG_E(DECODER_TAG, "Failed to allocate codec context");
        return -1;
    }

    /* 低延迟配置 */
    g_decoderCtx.codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    g_decoderCtx.codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    av_opt_set(g_decoderCtx.codecCtx->priv_data, "tune", "zerolatency", 0);
    av_opt_set(g_decoderCtx.codecCtx->priv_data, "threads", "4", 0);

    g_decoderCtx.frame = av_frame_alloc();
    g_decoderCtx.swFrame = av_frame_alloc();
    g_decoderCtx.packet = av_packet_alloc();

    if (!g_decoderCtx.frame || !g_decoderCtx.swFrame || !g_decoderCtx.packet) {
        LOG_TAG_E(DECODER_TAG, "Failed to allocate frame/packet");
        video_decoder_destroy();
        return -1;
    }

    LOG_TAG_I(DECODER_TAG, "Decoder created: %s", codec->name);
    return 0;
}

void video_decoder_set_callback(DecodedFrameCallback callback, void *userData)
{
    g_decoderCtx.callback = callback;
    g_decoderCtx.userData = userData;
}

int video_decoder_set_sps_pps(const uint8_t *sps, uint32_t spsLen,
                              const uint8_t *pps, uint32_t ppsLen)
{
    if (!sps || !pps || spsLen == 0 || ppsLen == 0) {
        return -1;
    }

    /* 构建 AVCodecParameters */
    uint8_t *extradata = (uint8_t *)av_malloc(spsLen + ppsLen + 8);
    if (!extradata) return -1;

    /* 构建 Annex-B 格式的 extradata */
    int pos = 0;
    /* SPS */
    extradata[pos++] = 0x00; extradata[pos++] = 0x00;
    extradata[pos++] = 0x00; extradata[pos++] = 0x01;
    memcpy(extradata + pos, sps, spsLen);
    pos += spsLen;
    /* PPS */
    extradata[pos++] = 0x00; extradata[pos++] = 0x00;
    extradata[pos++] = 0x00; extradata[pos++] = 0x01;
    memcpy(extradata + pos, pps, ppsLen);
    pos += ppsLen;

    g_decoderCtx.codecCtx->extradata = extradata;
    g_decoderCtx.codecCtx->extradata_size = pos;

    /* 打开解码器 */
    int ret = avcodec_open2(g_decoderCtx.codecCtx, NULL, NULL);
    if (ret < 0) {
        char errBuf[128];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_TAG_E(DECODER_TAG, "Failed to open codec: %s", errBuf);
        return -1;
    }

    g_decoderCtx.spsPpsSet = true;
    LOG_TAG_I(DECODER_TAG, "SPS/PPS set, codec opened");
    return 0;
}

int video_decoder_decode(const uint8_t *data, uint32_t length,
                         uint64_t timestamp, bool isKeyFrame)
{
    if (!g_decoderCtx.spsPpsSet) {
        return -1;
    }

    AVPacket *pkt = g_decoderCtx.packet;
    av_packet_unref(pkt);

    pkt->data = (uint8_t *)data;
    pkt->size = (int)length;
    pkt->pts = (int64_t)timestamp;
    pkt->dts = (int64_t)timestamp;

    if (isKeyFrame) {
        pkt->flags |= AV_PKT_FLAG_KEY;
    }

    /* 发送数据包到解码器 */
    int ret = avcodec_send_packet(g_decoderCtx.codecCtx, pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        char errBuf[128];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_TAG_E(DECODER_TAG, "Send packet failed: %s", errBuf);
        return -1;
    }

    /* 接收解码后的帧 */
    while (1) {
        ret = avcodec_receive_frame(g_decoderCtx.codecCtx, g_decoderCtx.frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            LOG_TAG_E(DECODER_TAG, "Receive frame failed");
            break;
        }

        AVFrame *displayFrame = g_decoderCtx.frame;

        /* 如果是硬件解码，需要转到系统内存 */
        if (g_decoderCtx.hwAccel && g_decoderCtx.frame->format != AV_PIX_FMT_YUV420P &&
            g_decoderCtx.frame->format != AV_PIX_FMT_YUVJ420P) {
            ret = av_hwframe_transfer_data(g_decoderCtx.swFrame,
                                           g_decoderCtx.frame, 0);
            if (ret < 0) {
                av_frame_unref(g_decoderCtx.frame);
                continue;
            }
            displayFrame = g_decoderCtx.swFrame;
        }

        g_decoderCtx.width = displayFrame->width;
        g_decoderCtx.height = displayFrame->height;

        /* 回调 */
        if (g_decoderCtx.callback) {
            DecodedFrame df = {0};
            for (int i = 0; i < 4; i++) {
                df.data[i] = displayFrame->data[i];
                df.linesize[i] = displayFrame->linesize[i];
            }
            df.width = displayFrame->width;
            df.height = displayFrame->height;
            df.timestamp = (uint64_t)displayFrame->pts;

            g_decoderCtx.callback(&df, g_decoderCtx.userData);
        }

        av_frame_unref(g_decoderCtx.frame);
        if (g_decoderCtx.hwAccel) {
            av_frame_unref(g_decoderCtx.swFrame);
        }
    }

    return 0;
}

int video_decoder_flush(void)
{
    if (!g_decoderCtx.codecCtx) return -1;

    avcodec_send_packet(g_decoderCtx.codecCtx, NULL);

    while (1) {
        int ret = avcodec_receive_frame(g_decoderCtx.codecCtx, g_decoderCtx.frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        av_frame_unref(g_decoderCtx.frame);
    }

    return 0;
}

void video_decoder_get_size(int *width, int *height)
{
    if (width) *width = g_decoderCtx.width;
    if (height) *height = g_decoderCtx.height;
}

void video_decoder_destroy(void)
{
    if (g_decoderCtx.packet) {
        av_packet_free(&g_decoderCtx.packet);
    }
    if (g_decoderCtx.frame) {
        av_frame_free(&g_decoderCtx.frame);
    }
    if (g_decoderCtx.swFrame) {
        av_frame_free(&g_decoderCtx.swFrame);
    }
    if (g_decoderCtx.codecCtx) {
        if (g_decoderCtx.codecCtx->extradata) {
            av_free(g_decoderCtx.codecCtx->extradata);
        }
        avcodec_free_context(&g_decoderCtx.codecCtx);
    }

    memset(&g_decoderCtx, 0, sizeof(g_decoderCtx));
}
