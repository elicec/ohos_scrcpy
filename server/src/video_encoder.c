/*
 * OHOS Scrcpy 服务端 - 视频编码实现
 * 使用 OH_VideoEncoder H.264 硬件编码
 */

#include "video_encoder.h"
#include "../../common/log.h"

#include <stdlib.h>
#include <string.h>

#include <multimedia/player_framework/native_avcodec_videoencoder.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avformat.h>
#include <multimedia/player_framework/native_avmemory.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avbuffer_info.h>
#include <multimedia/player_framework/native_avcapability.h>

#ifdef WITH_OPENH264
/* OpenH264 软编后端：设备无 AVC 编码器组件时兜底（如 OHarm OS 2.0 老设备）。
 * 实现在 sw_encoder.cpp（C++ 封装，导出 C 接口），此处仅作声明。 */
int swenc_create(uint32_t width, uint32_t height,
                 uint32_t bitrate, uint32_t fps, uint32_t gop);
int swenc_encode(const uint8_t *rgba, uint64_t timestampUs,
                 void (*onFrame)(const uint8_t *data, uint32_t len,
                                 int isKeyFrame, uint64_t tsUs, void *userData),
                 void *userData);
void swenc_destroy(void);
#endif

#define ENCODER_TAG "Encoder"

typedef struct {
    VideoEncoderConfig    config;
    OH_AVCodec           *encoder;
    OH_AVCodecAsyncCallback asyncCallback;
    EncodedFrameCallback  frameCallback;
    void                 *frameUserData;
    SpsPpsCallback        spsPpsCallback;
    void                 *spsPpsUserData;
    bool                  running;
    bool                  firstFrame;
    SpsPpsData            spsPps;
    uint32_t              inputIndex;
    OH_AVMemory          *inputData;
    bool                  inputReady;
#ifdef WITH_OPENH264
    /* OpenH264 软编后端（编码器实例与 I420 缓冲由 sw_encoder.cpp 管理） */
    bool                  useSw;
    bool                  swSpsPpsSent;  /* 是否已向客户端发送过 SPS/PPS */
#endif
} EncoderContext;

static EncoderContext g_encoderCtx = {0};

static const char* find_h264_encoder_name(void)
{
    /* 先尝试通过 Capability API 动态查询系统推荐的 AVC 编码器 */
    OH_AVCapability *cap = OH_AVCodec_GetCapability(OH_AVCODEC_MIMETYPE_VIDEO_AVC, true);
    if (cap != NULL) {
        const char *name = OH_AVCapability_GetName(cap);
        if (name != NULL && name[0] != '\0') {
            LOG_TAG_I(ENCODER_TAG, "Found H.264 encoder via GetCapability: %s", name);
            return name;
        }
        LOG_TAG_W(ENCODER_TAG, "GetCapability returned cap but name is empty");
    } else {
        LOG_TAG_W(ENCODER_TAG, "OH_AVCodec_GetCapability(AVC, encoder) returned NULL");
    }

    /* fallback 1: 尝试硬件类别查询 */
    cap = OH_AVCodec_GetCapabilityByCategory(OH_AVCODEC_MIMETYPE_VIDEO_AVC, true, HARDWARE);
    if (cap != NULL) {
        const char *name = OH_AVCapability_GetName(cap);
        if (name != NULL && name[0] != '\0') {
            LOG_TAG_I(ENCODER_TAG, "Found H.264 HW encoder via GetCapabilityByCategory: %s", name);
            return name;
        }
        LOG_TAG_W(ENCODER_TAG, "HW category cap returned but name is empty");
    } else {
        LOG_TAG_W(ENCODER_TAG, "No H.264 HARDWARE encoder available");
    }

    /* fallback 2: 尝试软件类别查询 */
    cap = OH_AVCodec_GetCapabilityByCategory(OH_AVCODEC_MIMETYPE_VIDEO_AVC, true, SOFTWARE);
    if (cap != NULL) {
        const char *name = OH_AVCapability_GetName(cap);
        if (name != NULL && name[0] != '\0') {
            LOG_TAG_I(ENCODER_TAG, "Found H.264 SW encoder via GetCapabilityByCategory: %s", name);
            return name;
        }
        LOG_TAG_W(ENCODER_TAG, "SW category cap returned but name is empty");
    } else {
        LOG_TAG_W(ENCODER_TAG, "No H.264 SOFTWARE encoder available");
    }

    /* 所有 Capability 查询均失败，返回 NULL 让上层走 CreateByMime 兜底 */
    LOG_TAG_E(ENCODER_TAG, "All H.264 capability queries failed, will try CreateByMime fallback");
    return NULL;
}

#ifdef WITH_OPENH264

/* 从 Annex-B 码流中提取首个 SPS/PPS，通过回调发送给客户端初始化解码器。
 * 返回 0 成功，-1 失败（失败时调用方可对下一个关键帧重试）。 */
static int sw_extract_and_send_sps_pps(const uint8_t *data, uint32_t length)
{
    const uint8_t *sps = NULL, *pps = NULL;
    uint32_t spsLen = 0, ppsLen = 0;
    uint32_t i = 0;

    while (i + 4 < length) {
        uint32_t scLen = 0;
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) {
                scLen = 3;
            } else if (data[i + 2] == 0 && i + 3 < length && data[i + 3] == 1) {
                scLen = 4;
            }
        }
        if (scLen == 0) {
            i++;
            continue;
        }

        uint32_t nalStart = i + scLen;
        uint32_t nalEnd = length;
        uint32_t j = nalStart;
        while (j + 2 < length) {
            if (data[j] == 0 && data[j + 1] == 0 &&
                (data[j + 2] == 1 || (data[j + 2] == 0 && j + 3 < length && data[j + 3] == 1))) {
                nalEnd = j;
                break;
            }
            j++;
        }

        uint32_t nalLen = nalEnd - nalStart;
        if (nalLen > 0) {
            uint8_t type = data[nalStart] & 0x1F;
            if (type == 7 && sps == NULL) {
                sps = data + nalStart;
                spsLen = nalLen;
            } else if (type == 8 && pps == NULL) {
                pps = data + nalStart;
                ppsLen = nalLen;
            }
        }
        if (sps && pps) break;
        i = nalEnd;
    }

    if (sps == NULL || pps == NULL || spsLen == 0 || ppsLen == 0) {
        LOG_TAG_W(ENCODER_TAG, "No SPS/PPS found in first keyframe (%u bytes)", length);
        return -1;
    }

    if (g_encoderCtx.spsPps.spsData) free(g_encoderCtx.spsPps.spsData);
    if (g_encoderCtx.spsPps.ppsData) free(g_encoderCtx.spsPps.ppsData);
    g_encoderCtx.spsPps.spsData = (uint8_t *)malloc(spsLen);
    g_encoderCtx.spsPps.ppsData = (uint8_t *)malloc(ppsLen);
    if (g_encoderCtx.spsPps.spsData == NULL || g_encoderCtx.spsPps.ppsData == NULL) {
        return -1;
    }
    memcpy(g_encoderCtx.spsPps.spsData, sps, spsLen);
    g_encoderCtx.spsPps.spsLength = spsLen;
    memcpy(g_encoderCtx.spsPps.ppsData, pps, ppsLen);
    g_encoderCtx.spsPps.ppsLength = ppsLen;

    LOG_TAG_I(ENCODER_TAG, "OpenH264 SPS(%u) PPS(%u) extracted", spsLen, ppsLen);
    if (g_encoderCtx.spsPpsCallback) {
        g_encoderCtx.spsPpsCallback(&g_encoderCtx.spsPps,
                                    g_encoderCtx.spsPpsUserData);
    }
    return 0;
}

/* swenc_encode 的编码输出回调：首个关键帧提取 SPS/PPS 发给客户端，随后转发编码帧 */
static void on_sw_encoded(const uint8_t *data, uint32_t len,
                          int isKeyFrame, uint64_t tsUs, void *userData)
{
    EncoderContext *ctx = &g_encoderCtx;
    (void)userData;

    if (isKeyFrame && !ctx->swSpsPpsSent) {
        if (sw_extract_and_send_sps_pps(data, len) == 0) {
            ctx->swSpsPpsSent = true;
        }
    }

    if (ctx->frameCallback) {
        EncodedFrame out = {0};
        out.data = (uint8_t *)data; /* 回调数据仅用于发送，不会修改 */
        out.length = len;
        out.timestamp = tsUs;
        out.isKeyFrame = isKeyFrame != 0;
        ctx->frameCallback(&out, ctx->frameUserData);
    }
}

/* 创建 OpenH264 软件编码器（实际工作由 sw_encoder.cpp 完成） */
static int sw_encoder_create(void)
{
    EncoderContext *ctx = &g_encoderCtx;

    if (swenc_create(ctx->config.width, ctx->config.height,
                     ctx->config.bitrate, ctx->config.fps,
                     ctx->config.gopSize) != 0) {
        LOG_TAG_E(ENCODER_TAG, "OpenH264 swenc_create failed");
        return -1;
    }

    ctx->useSw = true;
    LOG_TAG_I(ENCODER_TAG, "OpenH264 software encoder ready (%ux%u, %ufps, %ubps)",
              ctx->config.width, ctx->config.height,
              ctx->config.fps, ctx->config.bitrate);
    return 0;
}

/* OpenH264 软编一帧 RGBA（RGBA->I420 转换与编码均在 sw_encoder.cpp 内完成） */
static int sw_encode_frame(CapturedFrame *frame)
{
    return swenc_encode(frame->data, frame->timestamp, on_sw_encoded, NULL);
}

#endif /* WITH_OPENH264 */

static void on_error(OH_AVCodec *codec, int32_t errorCode, void *userData)
{
    (void)codec;
    (void)userData;
    LOG_TAG_E(ENCODER_TAG, "Encoder error: %d", errorCode);
}

static void on_stream_changed(OH_AVCodec *codec, OH_AVFormat *format,
                              void *userData)
{
    (void)codec;
    (void)userData;

    if (format == NULL) return;

    uint8_t *spsData = NULL;
    size_t spsSize = 0;
    uint8_t *ppsData = NULL;
    size_t ppsSize = 0;

    OH_AVFormat_GetBuffer(format, "sps", &spsData, &spsSize);
    OH_AVFormat_GetBuffer(format, "pps", &ppsData, &ppsSize);

    if (spsData && spsSize > 0 && ppsData && ppsSize > 0) {
        if (g_encoderCtx.spsPps.spsData) free(g_encoderCtx.spsPps.spsData);
        if (g_encoderCtx.spsPps.ppsData) free(g_encoderCtx.spsPps.ppsData);

        g_encoderCtx.spsPps.spsData = (uint8_t *)malloc(spsSize);
        g_encoderCtx.spsPps.spsLength = (uint32_t)spsSize;
        memcpy(g_encoderCtx.spsPps.spsData, spsData, spsSize);

        g_encoderCtx.spsPps.ppsData = (uint8_t *)malloc(ppsSize);
        g_encoderCtx.spsPps.ppsLength = (uint32_t)ppsSize;
        memcpy(g_encoderCtx.spsPps.ppsData, ppsData, ppsSize);

        LOG_TAG_I(ENCODER_TAG, "Got SPS(%zu) PPS(%zu)", spsSize, ppsSize);

        if (g_encoderCtx.spsPpsCallback) {
            g_encoderCtx.spsPpsCallback(&g_encoderCtx.spsPps,
                                        g_encoderCtx.spsPpsUserData);
        }
    }
}

static void on_need_input_data(OH_AVCodec *codec, uint32_t index,
                               OH_AVMemory *data, void *userData)
{
    (void)codec;
    (void)userData;
    g_encoderCtx.inputIndex = index;
    g_encoderCtx.inputData = data;
    g_encoderCtx.inputReady = true;
}

static void on_need_output_data(OH_AVCodec *codec, uint32_t index,
                                OH_AVMemory *data,
                                OH_AVCodecBufferAttr *attr, void *userData)
{
    (void)userData;

    if (attr == NULL) {
        OH_VideoEncoder_FreeOutputBuffer(codec, index);
        return;
    }

    if (attr->flags & AVCODEC_BUFFER_FLAGS_CODEC_DATA) {
        OH_VideoEncoder_FreeOutputBuffer(codec, index);
        return;
    }

    uint8_t *outData = NULL;
    if (data != NULL) {
        outData = OH_AVMemory_GetAddr(data);
    }

    if (outData == NULL) {
        OH_VideoEncoder_FreeOutputBuffer(codec, index);
        return;
    }

    EncodedFrame frame = {0};
    frame.data = outData;
    frame.length = (uint32_t)attr->size;
    frame.timestamp = (uint64_t)attr->pts;
    frame.isKeyFrame = (attr->flags & AVCODEC_BUFFER_FLAGS_SYNC_FRAME) != 0;

    if (g_encoderCtx.frameCallback) {
        g_encoderCtx.frameCallback(&frame, g_encoderCtx.frameUserData);
    }

    if (g_encoderCtx.firstFrame && frame.isKeyFrame) {
        g_encoderCtx.firstFrame = false;
    }

    OH_VideoEncoder_FreeOutputBuffer(codec, index);
}

int video_encoder_create(const VideoEncoderConfig *config)
{
    memset(&g_encoderCtx, 0, sizeof(g_encoderCtx));
    memcpy(&g_encoderCtx.config, config, sizeof(VideoEncoderConfig));

    if (g_encoderCtx.config.bitrate == 0)
        g_encoderCtx.config.bitrate = 4000000;
    if (g_encoderCtx.config.fps == 0)
        g_encoderCtx.config.fps = 60;
    if (g_encoderCtx.config.gopSize == 0)
        g_encoderCtx.config.gopSize = 60;
    if (g_encoderCtx.config.profile == 0)
        g_encoderCtx.config.profile = 2;

    /* 优先通过 Capability API 动态获取编码器名称，再创建 */
    const char *codecName = find_h264_encoder_name();
    if (codecName != NULL) {
        g_encoderCtx.encoder = OH_VideoEncoder_CreateByName(codecName);
        if (g_encoderCtx.encoder != NULL) {
            LOG_TAG_I(ENCODER_TAG, "Encoder created by name: %s (%ux%u)", codecName,
                      config->width, config->height);
            return 0;
        }
        LOG_TAG_W(ENCODER_TAG, "CreateByName(%s) failed, trying CreateByMime", codecName);
    }

    /* fallback: 使用 mime type 创建 */
    g_encoderCtx.encoder = OH_VideoEncoder_CreateByMime(OH_AVCODEC_MIMETYPE_VIDEO_AVC);
    if (g_encoderCtx.encoder == NULL) {
        LOG_TAG_E(ENCODER_TAG, "Failed to create encoder by mime AVC");
#ifdef WITH_OPENH264
        /* 系统 AVC 编码器不可用，回退到 OpenH264 软件编码 */
        LOG_TAG_W(ENCODER_TAG, "Falling back to OpenH264 software encoder");
        if (sw_encoder_create() == 0) {
            return 0;
        }
        LOG_TAG_E(ENCODER_TAG, "OpenH264 software encoder create failed");
#endif
        return -1;
    }
    LOG_TAG_I(ENCODER_TAG, "Encoder created by mime AVC (%ux%u)",
              config->width, config->height);

    return 0;
}

void video_encoder_set_frame_callback(EncodedFrameCallback callback, void *userData)
{
    g_encoderCtx.frameCallback = callback;
    g_encoderCtx.frameUserData = userData;
}

void video_encoder_set_sps_pps_callback(SpsPpsCallback callback, void *userData)
{
    g_encoderCtx.spsPpsCallback = callback;
    g_encoderCtx.spsPpsUserData = userData;
}

int video_encoder_start(void)
{
    if (g_encoderCtx.running) {
        return 0;
    }

#ifdef WITH_OPENH264
    if (g_encoderCtx.useSw) {
        /* OpenH264 软编无需 configure/prepare，直接标记运行 */
        g_encoderCtx.running = true;
        g_encoderCtx.firstFrame = true;
        LOG_TAG_I(ENCODER_TAG, "OpenH264 software encoder started");
        return 0;
    }
#endif

    OH_AVFormat *format = OH_AVFormat_Create();
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_WIDTH, (int32_t)g_encoderCtx.config.width);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_HEIGHT, (int32_t)g_encoderCtx.config.height);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_RGBA);
    OH_AVFormat_SetLongValue(format, OH_MD_KEY_BITRATE, (int64_t)g_encoderCtx.config.bitrate);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_FRAME_RATE, (int32_t)g_encoderCtx.config.fps);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_I_FRAME_INTERVAL, (int32_t)g_encoderCtx.config.gopSize);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_PROFILE, (int32_t)g_encoderCtx.config.profile);

    if (g_encoderCtx.config.lowLatency) {
        OH_AVFormat_SetIntValue(format, "low_latency", 1);
    }

    int32_t ret = OH_VideoEncoder_Configure(g_encoderCtx.encoder, format);
    OH_AVFormat_Destroy(format);

    if (ret != 0) {
        LOG_TAG_E(ENCODER_TAG, "Configure failed: %d", ret);
        return -1;
    }

    g_encoderCtx.asyncCallback.onError = on_error;
    g_encoderCtx.asyncCallback.onStreamChanged = on_stream_changed;
    g_encoderCtx.asyncCallback.onNeedInputData = on_need_input_data;
    g_encoderCtx.asyncCallback.onNeedOutputData = on_need_output_data;

    ret = OH_VideoEncoder_SetCallback(g_encoderCtx.encoder, g_encoderCtx.asyncCallback, NULL);
    if (ret != 0) {
        LOG_TAG_E(ENCODER_TAG, "Set callback failed: %d", ret);
        return -1;
    }

    ret = OH_VideoEncoder_Prepare(g_encoderCtx.encoder);
    if (ret != 0) {
        LOG_TAG_E(ENCODER_TAG, "Prepare failed: %d", ret);
        return -1;
    }

    ret = OH_VideoEncoder_Start(g_encoderCtx.encoder);
    if (ret != 0) {
        LOG_TAG_E(ENCODER_TAG, "Start failed: %d", ret);
        return -1;
    }

    g_encoderCtx.running = true;
    g_encoderCtx.firstFrame = true;

    LOG_TAG_I(ENCODER_TAG, "Encoder started (%ux%u, %ufps, %ubps)",
              g_encoderCtx.config.width, g_encoderCtx.config.height,
              g_encoderCtx.config.fps, g_encoderCtx.config.bitrate);

    return 0;
}

int video_encoder_encode_frame(CapturedFrame *frame)
{
    if (!g_encoderCtx.running || frame == NULL) {
        return -1;
    }

#ifdef WITH_OPENH264
    if (g_encoderCtx.useSw) {
        return sw_encode_frame(frame);
    }
#endif

    if (!g_encoderCtx.inputReady || g_encoderCtx.inputData == NULL) {
        return -1;
    }

    uint8_t *dst = OH_AVMemory_GetAddr(g_encoderCtx.inputData);
    if (dst == NULL) {
        g_encoderCtx.inputReady = false;
        return -1;
    }

    uint32_t srcStride = frame->width * 4;
    uint32_t dstStride = g_encoderCtx.config.width * 4;
    uint32_t copyStride = srcStride < dstStride ? srcStride : dstStride;
    uint32_t copyHeight = frame->height < g_encoderCtx.config.height ?
                          frame->height : g_encoderCtx.config.height;

    for (uint32_t i = 0; i < copyHeight; i++) {
        memcpy(dst + i * dstStride, frame->data + i * srcStride, copyStride);
    }

    OH_AVCodecBufferAttr info;
    info.pts = (int64_t)frame->timestamp;
    info.size = (size_t)(dstStride * g_encoderCtx.config.height);
    info.offset = 0;
    info.flags = AVCODEC_BUFFER_FLAGS_NONE;

    OH_VideoEncoder_PushInputData(g_encoderCtx.encoder, g_encoderCtx.inputIndex, info);
    g_encoderCtx.inputReady = false;

    return 0;
}

int video_encoder_request_key_frame(void)
{
    if (!g_encoderCtx.running) return -1;

    OH_AVFormat *format = OH_AVFormat_Create();
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_REQUEST_I_FRAME, 1);
    int32_t ret = OH_VideoEncoder_SetParameter(g_encoderCtx.encoder, format);
    OH_AVFormat_Destroy(format);

    return ret == 0 ? 0 : -1;
}

int video_encoder_set_bitrate(uint32_t bitrate)
{
    if (!g_encoderCtx.running) return -1;

    OH_AVFormat *format = OH_AVFormat_Create();
    OH_AVFormat_SetLongValue(format, OH_MD_KEY_BITRATE, (int64_t)bitrate);
    int32_t ret = OH_VideoEncoder_SetParameter(g_encoderCtx.encoder, format);
    OH_AVFormat_Destroy(format);

    if (ret == 0) {
        g_encoderCtx.config.bitrate = bitrate;
    }

    return ret == 0 ? 0 : -1;
}

int video_encoder_stop(void)
{
    if (!g_encoderCtx.running) return 0;

    g_encoderCtx.running = false;
#ifdef WITH_OPENH264
    if (g_encoderCtx.useSw) {
        LOG_TAG_I(ENCODER_TAG, "OpenH264 encoder stopped");
        return 0;
    }
#endif
    OH_VideoEncoder_Stop(g_encoderCtx.encoder);

    LOG_TAG_I(ENCODER_TAG, "Encoder stopped");
    return 0;
}

void video_encoder_destroy(void)
{
    video_encoder_stop();

#ifdef WITH_OPENH264
    if (g_encoderCtx.useSw) {
        swenc_destroy();
        g_encoderCtx.useSw = false;
    }
#endif

    if (g_encoderCtx.encoder) {
        OH_VideoEncoder_Destroy(g_encoderCtx.encoder);
        g_encoderCtx.encoder = NULL;
    }

    if (g_encoderCtx.spsPps.spsData) free(g_encoderCtx.spsPps.spsData);
    if (g_encoderCtx.spsPps.ppsData) free(g_encoderCtx.spsPps.ppsData);

    memset(&g_encoderCtx, 0, sizeof(g_encoderCtx));
}
