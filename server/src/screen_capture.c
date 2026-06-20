/*
 * OHOS Scrcpy 服务端 - 屏幕捕获实现
 * 使用 AVScreenCapture C-API 捕获屏幕帧数据
 */

#include "screen_capture.h"
#include "../../common/log.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* OpenHarmony AVScreenCapture C-API */
#include <multimedia/player_framework/av_screen_capture.h>
#include <multimedia/player_framework/av_screen_capture_base.h>
#include <window_manager/oh_display_capture.h>
#include <window_manager/oh_display_manager.h>

#define CAPTURE_TAG "Capture"

typedef struct {
    ScreenCaptureConfig  config;
    OH_AVScreenCapture  *capture;
    FrameCallback        callback;
    void                *userData;
    uint32_t             actualWidth;
    uint32_t             actualHeight;
    bool                 running;
} CaptureContext;

static CaptureContext g_captureCtx = {0};

/* AVScreenCapture 数据回调 */
static void on_audio_buffer_callback(OH_AVScreenCapture *capture,
                                     OH_AVScreenCaptureBuffer *buffer,
                                     void *userData)
{
    /* 不需要音频，忽略 */
    (void)capture;
    (void)buffer;
    (void)userData;
}

/* AVScreenCapture 视频帧回调 - 使用录屏取码流模式 */
static void on_video_buffer_callback(OH_AVScreenCapture *capture,
                                     OH_AVScreenCaptureBuffer *buffer,
                                     void *userData)
{
    (void)capture;
    (void)userData;

    if (!g_captureCtx.running || !g_captureCtx.callback) {
        return;
    }

    if (buffer == NULL || buffer->buffer == NULL) {
        return;
    }

    CapturedFrame frame = {0};
    frame.data = buffer->buffer;
    frame.width = g_captureCtx.actualWidth;
    frame.height = g_captureCtx.actualHeight;
    frame.stride = g_captureCtx.actualWidth * 4; /* RGBA */
    frame.timestamp = (uint64_t)buffer->timeStamp;

    g_captureCtx.callback(&frame, g_captureCtx.userData);
}

int screen_capture_create(const ScreenCaptureConfig *config)
{
    memset(&g_captureCtx, 0, sizeof(g_captureCtx));
    memcpy(&g_captureCtx.config, config, sizeof(ScreenCaptureConfig));

    /* 获取屏幕实际尺寸 */
    uint32_t dispW = 0, dispH = 0;
    if (screen_capture_get_display_size(&dispW, &dispH) != 0) {
        LOG_TAG_E(CAPTURE_TAG, "Failed to get display size");
        return -1;
    }

    /* 应用缩放 */
    if (config->scale > 0 && config->scale < 1.0f) {
        g_captureCtx.actualWidth = (uint32_t)(dispW * config->scale);
        g_captureCtx.actualHeight = (uint32_t)(dispH * config->scale);
    } else {
        g_captureCtx.actualWidth = dispW;
        g_captureCtx.actualHeight = dispH;
    }

    /* 确保宽高为偶数(H.264要求) */
    g_captureCtx.actualWidth &= ~1;
    g_captureCtx.actualHeight &= ~1;

    LOG_TAG_I(CAPTURE_TAG, "Screen capture: %ux%u -> %ux%u (scale=%.2f)",
              dispW, dispH, g_captureCtx.actualWidth, g_captureCtx.actualHeight,
              config->scale);

    return 0;
}

int screen_capture_get_display_size(uint32_t *width, uint32_t *height)
{
    /* 使用 OH_NativeDisplayManager 获取屏幕尺寸 */
    OH_DisplayManager_ErrorCode err =
        OH_NativeDisplayManager_GetDefaultDisplaySize(width, height);
    if (err != DISPLAY_MANAGER_OK) {
        LOG_TAG_E(CAPTURE_TAG, "Get display size failed: %d", err);
        return -1;
    }
    return 0;
}

int screen_capture_start(FrameCallback callback, void *userData)
{
    if (g_captureCtx.running) {
        LOG_TAG_W(CAPTURE_TAG, "Already running");
        return 0;
    }

    g_captureCtx.callback = callback;
    g_captureCtx.userData = userData;

    /* 创建 AVScreenCapture 实例 */
    g_captureCtx.capture = OH_AVScreenCapture_Create();
    if (g_captureCtx.capture == NULL) {
        LOG_TAG_E(CAPTURE_TAG, "Failed to create AVScreenCapture");
        return -1;
    }

    /* 配置录屏参数 */
    OH_AVScreenCaptureConfig captureConfig;
    memset(&captureConfig, 0, sizeof(captureConfig));

    /* 视频捕获配置 */
    captureConfig.videoCapInfo.videoFrameWidth = g_captureCtx.actualWidth;
    captureConfig.videoCapInfo.videoFrameHeight = g_captureCtx.actualHeight;
    captureConfig.videoCapInfo.videoSource = OH_VIDEO_SOURCE_SURFACE_RGBA;

    /* 音频捕获配置 - 不需要音频 */
    captureConfig.audioCapInfo.audioSource = OH_AUDIO_SOURCE_DEFAULT;
    captureConfig.audioCapInfo.audioSampleRate = 0;
    captureConfig.audioCapInfo.audioChannelCount = 0;

    /* 设置回调 */
    OH_AVScreenCapture_SetCallback(g_captureCtx.capture, on_audio_buffer_callback,
                                   on_video_buffer_callback, NULL);

    /* 初始化 */
    int32_t ret = OH_AVScreenCapture_Init(g_captureCtx.capture, &captureConfig);
    if (ret != 0) {
        LOG_TAG_E(CAPTURE_TAG, "AVScreenCapture init failed: %d", ret);
        OH_AVScreenCapture_Release(g_captureCtx.capture);
        g_captureCtx.capture = NULL;
        return -1;
    }

    /* 启动录屏 */
    ret = OH_AVScreenCapture_StartScreenCapture(g_captureCtx.capture);
    if (ret != 0) {
        LOG_TAG_E(CAPTURE_TAG, "Start screen capture failed: %d", ret);
        OH_AVScreenCapture_Release(g_captureCtx.capture);
        g_captureCtx.capture = NULL;
        return -1;
    }

    g_captureCtx.running = true;
    LOG_TAG_I(CAPTURE_TAG, "Screen capture started (%ux%u)",
              g_captureCtx.actualWidth, g_captureCtx.actualHeight);

    return 0;
}

int screen_capture_stop(void)
{
    if (!g_captureCtx.running) {
        return 0;
    }

    g_captureCtx.running = false;

    if (g_captureCtx.capture) {
        OH_AVScreenCapture_StopScreenCapture(g_captureCtx.capture);
        OH_AVScreenCapture_Release(g_captureCtx.capture);
        g_captureCtx.capture = NULL;
    }

    LOG_TAG_I(CAPTURE_TAG, "Screen capture stopped");
    return 0;
}

void screen_capture_destroy(void)
{
    screen_capture_stop();
    memset(&g_captureCtx, 0, sizeof(g_captureCtx));
}
