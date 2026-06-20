/*
 * OHOS Scrcpy 服务端 - 视频编码实现
 * 使用 OH_VideoEncoder H.264 硬件编码
 */

#include "video_encoder.h"
#include "../../common/log.h"

#include <stdlib.h>
#include <string.h>

/* OpenHarmony 视频编码 C-API */
#include <multimedia/player_framework/video_encoder.h>

#define ENCODER_TAG "Encoder"

typedef struct {
    VideoEncoderConfig    config;
    OH_VideoEncoder      *encoder;
    OH_AVCodecAsyncCallback asyncCallback;
    EncodedFrameCallback  frameCallback;
    void                 *frameUserData;
    SpsPpsCallback        spsPpsCallback;
    void                 *spsPpsUserData;
    bool                  running;
    bool                  firstFrame;
    SpsPpsData            spsPps;
} EncoderContext;

static EncoderContext g_encoderCtx = {0};

/* 查找 H.264 编码器名称 */
static const char* find_h264_encoder_name(void)
{
    /* OpenHarmony 支持的 H.264 硬件编码器名称 */
    return "OH.Media.Codec.Encoder.Video.AVC";
}

/* 编码器错误回调 */
static void on_error(OH_VideoEncoder *codec, int32_t errorCode, void *userData)
{
    (void)codec;
    (void)userData;
    LOG_TAG_E(ENCODER_TAG, "Encoder error: %d", errorCode);
}

/* 编码器输出格式变化回调 - 获取 SPS/PPS */
static void on_output_format_changed(OH_VideoEncoder *codec, OH_AVFormat *format,
                                     void *userData)
{
    (void)codec;
    (void)userData;

    if (format == NULL) return;

    /* 从输出格式中提取 SPS 和 PPS */
    uint8_t *spsData = NULL;
    int32_t spsSize = 0;
    uint8_t *ppsData = NULL;
    int32_t ppsSize = 0;

    OH_AVFormat_GetBufferValue(format, "sps", &spsData, &spsSize);
    OH_AVFormat_GetBufferValue(format, "pps", &ppsData, &ppsSize);

    if (spsData && spsSize > 0 && ppsData && ppsSize > 0) {
        /* 保存 SPS/PPS */
        if (g_encoderCtx.spsPps.spsData) free(g_encoderCtx.spsPps.spsData);
        if (g_encoderCtx.spsPps.ppsData) free(g_encoderCtx.spsPps.ppsData);

        g_encoderCtx.spsPps.spsData = (uint8_t *)malloc(spsSize);
        g_encoderCtx.spsPps.spsLength = spsSize;
        memcpy(g_encoderCtx.spsPps.spsData, spsData, spsSize);

        g_encoderCtx.spsPps.ppsData = (uint8_t *)malloc(ppsSize);
        g_encoderCtx.spsPps.ppsLength = ppsSize;
        memcpy(g_encoderCtx.spsPps.ppsData, ppsData, ppsSize);

        LOG_TAG_I(ENCODER_TAG, "Got SPS(%d) PPS(%d)", spsSize, ppsSize);

        if (g_encoderCtx.spsPpsCallback) {
            g_encoderCtx.spsPpsCallback(&g_encoderCtx.spsPps,
                                        g_encoderCtx.spsPpsUserData);
        }
    }
}

/* 编码器输入缓冲区就绪回调 */
static void on_input_buffer_available(OH_VideoEncoder *codec, uint32_t index,
                                      OH_AVMemory *buffer, void *userData)
{
    (void)codec;
    (void)index;
    (void)buffer;
    (void)userData;
    /* 输入由 video_encoder_encode_frame 主动处理 */
}

/* 编码器输出缓冲区就绪回调 - 获取编码帧 */
static void on_output_buffer_available(OH_VideoEncoder *codec, uint32_t index,
                                       OH_AVMemory *buffer, OH_AVCodecBufferInfo *info,
                                       OH_AVCodecBufferFlag flag, void *userData)
{
    (void)userData;

    if (buffer == NULL || info == NULL) {
        OH_VideoEncoder_FreeOutputBuffer(codec, index);
        return;
    }

    /* 跳过 codec 配置数据(已通过 format changed 回调获取) */
    if (flag & AVCODEC_BUFFER_FLAG_CODEC_DATA) {
        OH_VideoEncoder_FreeOutputBuffer(codec, index);
        return;
    }

    uint8_t *outData = OH_AVMemory_GetAddr(buffer);
    if (outData == NULL) {
        OH_VideoEncoder_FreeOutputBuffer(codec, index);
        return;
    }

    EncodedFrame frame = {0};
    frame.data = outData;
    frame.length = info->size;
    frame.timestamp = (uint64_t)info->presentationTimeUs;
    frame.isKeyFrame = (flag & AVCODEC_BUFFER_FLAG_KEYFRAME) != 0;

    if (g_encoderCtx.frameCallback) {
        g_encoderCtx.frameCallback(&frame, g_encoderCtx.frameUserData);
    }

    /* 第一帧必须是关键帧 */
    if (g_encoderCtx.firstFrame && frame.isKeyFrame) {
        g_encoderCtx.firstFrame = false;
    }

    OH_VideoEncoder_FreeOutputBuffer(codec, index);
}

int video_encoder_create(const VideoEncoderConfig *config)
{
    memset(&g_encoderCtx, 0, sizeof(g_encoderCtx));
    memcpy(&g_encoderCtx.config, config, sizeof(VideoEncoderConfig));

    /* 设置默认值 */
    if (g_encoderCtx.config.bitrate == 0)
        g_encoderCtx.config.bitrate = 4000000;
    if (g_encoderCtx.config.fps == 0)
        g_encoderCtx.config.fps = 60;
    if (g_encoderCtx.config.gopSize == 0)
        g_encoderCtx.config.gopSize = 60;
    if (g_encoderCtx.config.profile == 0)
        g_encoderCtx.config.profile = 2; /* High */

    /* 创建编码器 */
    const char *codecName = find_h264_encoder_name();
    g_encoderCtx.encoder = OH_VideoEncoder_CreateByName(codecName);
    if (g_encoderCtx.encoder == NULL) {
        LOG_TAG_E(ENCODER_TAG, "Failed to create encoder: %s", codecName);
        return -1;
    }

    LOG_TAG_I(ENCODER_TAG, "Encoder created: %s (%ux%u)", codecName,
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

    /* 配置编码参数 */
    OH_AVFormat *format = OH_AVFormat_Create();
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_WIDTH, g_encoderCtx.config.width);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_HEIGHT, g_encoderCtx.config.height);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_RGBA_8888);
    OH_AVFormat_SetLongValue(format, OH_MD_KEY_BIT_RATE, g_encoderCtx.config.bitrate);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_FRAME_RATE, g_encoderCtx.config.fps);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_I_FRAME_INTERVAL, g_encoderCtx.config.gopSize);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_PROFILE, g_encoderCtx.config.profile);

    /* 低延迟优化 */
    if (g_encoderCtx.config.lowLatency) {
        OH_AVFormat_SetIntValue(format, "low_latency", 1);
        OH_AVFormat_SetIntValue(format, OH_MD_KEY_MAX_B_FRAMES, 0);
    }

    /* 配置编码器 */
    int32_t ret = OH_VideoEncoder_Configure(g_encoderCtx.encoder, format);
    OH_AVFormat_Destroy(format);

    if (ret != 0) {
        LOG_TAG_E(ENCODER_TAG, "Configure failed: %d", ret);
        return -1;
    }

    /* 设置异步回调 */
    g_encoderCtx.asyncCallback.onError = on_error;
    g_encoderCtx.asyncCallback.onOutputFormatChanged = on_output_format_changed;
    g_encoderCtx.asyncCallback.onInputBufferAvailable = on_input_buffer_available;
    g_encoderCtx.asyncCallback.onOutputBufferAvailable = on_output_buffer_available;

    ret = OH_VideoEncoder_SetCallback(g_encoderCtx.encoder, &g_encoderCtx.asyncCallback, NULL);
    if (ret != 0) {
        LOG_TAG_E(ENCODER_TAG, "Set callback failed: %d", ret);
        return -1;
    }

    /* 启动编码器 */
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

    /* 获取输入缓冲区 */
    int32_t inputIndex = OH_VideoEncoder_GetInputBuffer(g_encoderCtx.encoder);
    if (inputIndex < 0) {
        return -1;
    }

    OH_AVMemory *inputBuffer = OH_VideoEncoder_GetInputBufferAt(g_encoderCtx.encoder,
                                                                  (uint32_t)inputIndex);
    if (inputBuffer == NULL) {
        return -1;
    }

    /* 拷贝帧数据到输入缓冲区 */
    uint8_t *dst = OH_AVMemory_GetAddr(inputBuffer);
    if (dst == NULL) {
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

    /* 提交输入缓冲区 */
    OH_AVCodecBufferInfo info = {
        .offset = 0,
        .size = dstStride * g_encoderCtx.config.height,
        .presentationTimeUs = (int64_t)frame->timestamp,
    };

    OH_VideoEncoder_PushInputBuffer(g_encoderCtx.encoder, (uint32_t)inputIndex, &info,
                                     AVCODEC_BUFFER_FLAG_NONE);

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
    OH_AVFormat_SetLongValue(format, OH_MD_KEY_BIT_RATE, bitrate);
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
