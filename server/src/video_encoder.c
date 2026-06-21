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
    OH_VideoEncoder_Stop(g_encoderCtx.encoder);

    LOG_TAG_I(ENCODER_TAG, "Encoder stopped");
    return 0;
}

void video_encoder_destroy(void)
{
    video_encoder_stop();

    if (g_encoderCtx.encoder) {
        OH_VideoEncoder_Destroy(g_encoderCtx.encoder);
        g_encoderCtx.encoder = NULL;
    }

    if (g_encoderCtx.spsPps.spsData) free(g_encoderCtx.spsPps.spsData);
    if (g_encoderCtx.spsPps.ppsData) free(g_encoderCtx.spsPps.ppsData);

    memset(&g_encoderCtx, 0, sizeof(g_encoderCtx));
}
