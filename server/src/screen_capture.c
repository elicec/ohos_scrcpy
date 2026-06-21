/*
 * OHOS Scrcpy 服务端 - 屏幕捕获实现
 * 使用 AVScreenCapture C-API 捕获屏幕帧数据
 */

#include "screen_capture.h"
#include "../../common/log.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stdio.h>

#include <multimedia/player_framework/native_avscreen_capture.h>
#include <multimedia/player_framework/native_avscreen_capture_base.h>
#include <native_buffer/native_buffer.h>

#define CAPTURE_TAG "Capture"
#define DEFAULT_SCREEN_WIDTH  1080
#define DEFAULT_SCREEN_HEIGHT 1920

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

static void on_audio_buffer_available(OH_AVScreenCapture *capture, bool isReady,
                                      OH_AudioCaptureSourceType type)
{
    (void)capture;
    (void)isReady;
    (void)type;
}

static void on_video_buffer_available(OH_AVScreenCapture *capture, bool isReady)
{
    if (!g_captureCtx.running || !g_captureCtx.callback) {
        return;
    }

    if (!isReady) {
        return;
    }

    int32_t fence = 0;
    int64_t timestamp = 0;
    OH_Rect region = {0};
    OH_NativeBuffer *buffer = OH_AVScreenCapture_AcquireVideoBuffer(capture, &fence,
                                                                     &timestamp, &region);
    if (buffer == NULL) {
        return;
    }

    CapturedFrame frame = {0};
    frame.timestamp = (uint64_t)timestamp;

    /* 获取 NativeBuffer 的实际配置 */
    OH_NativeBuffer_Config bufConfig;
    OH_NativeBuffer_GetConfig(buffer, &bufConfig);
    LOG_TAG_I(CAPTURE_TAG, "NativeBuffer config: %dx%d stride=%d format=%d",
              bufConfig.width, bufConfig.height, bufConfig.stride, bufConfig.format);

    /* 使用 NativeBuffer 真实尺寸，而不是请求的缩放尺寸 */
    frame.width = (uint32_t)bufConfig.width;
    frame.height = (uint32_t)bufConfig.height;

    /* 映射 NativeBuffer。
     * 优先尝试 OH_NativeBuffer_MapPlanes（since 12），可获取真实 rowStride；
     * 旧版本系统没有该符号，则回退到 OH_NativeBuffer_Map（since 9）。 */
    void *virAddr = NULL;
    OH_NativeBuffer_Planes planes = {0};
    bool usedMapPlanes = false;

    typedef int32_t (*MapPlanesFunc)(OH_NativeBuffer *, void **, OH_NativeBuffer_Planes *);
    static MapPlanesFunc mapPlanes = NULL;
    static bool mapPlanesResolved = false;
    if (!mapPlanesResolved) {
        mapPlanesResolved = true;
        mapPlanes = (MapPlanesFunc)dlsym(RTLD_DEFAULT, "OH_NativeBuffer_MapPlanes");
        if (!mapPlanes) {
            /* RTLD_DEFAULT 找不到时，再尝试从 libnative_buffer.so 加载 */
            void *handle = dlopen("libnative_buffer.so", RTLD_LAZY);
            if (handle) {
                mapPlanes = (MapPlanesFunc)dlsym(handle, "OH_NativeBuffer_MapPlanes");
            }
        }
        if (mapPlanes) {
            LOG_TAG_I(CAPTURE_TAG, "OH_NativeBuffer_MapPlanes is available");
        } else {
            LOG_TAG_I(CAPTURE_TAG, "OH_NativeBuffer_MapPlanes not available, fallback to OH_NativeBuffer_Map");
        }
    }

    int mapRet = -1;
    if (mapPlanes) {
        mapRet = mapPlanes(buffer, &virAddr, &planes);
        usedMapPlanes = true;
    } else {
        mapRet = OH_NativeBuffer_Map(buffer, &virAddr);
    }
    if (mapRet != 0 || virAddr == NULL) {
        LOG_TAG_E(CAPTURE_TAG, "Failed to map NativeBuffer: ret=%d, addr=%p, mapPlanes=%d",
                  mapRet, virAddr, usedMapPlanes);
        OH_AVScreenCapture_ReleaseVideoBuffer(capture);
        return;
    }

    /* 优先使用 MapPlanes 返回的真实 rowStride（字节数），它比 bufConfig.stride 更可靠。
     * 若无法获取 MapPlanes，回退到紧凑 stride (width * 4)。
     * 实测发现 OH_AVScreenCapture 返回的 bufConfig.stride 可能大于实际像素行字节数
     * （例如紧凑数据仍报告 stride=2176），用其逐行拷贝反而会把下一行数据错当成
     * 当前行 padding，导致画面倾斜。紧凑提取在 padding 为重复像素或零时也是安全的。 */
    uint32_t widthBytes = frame.width * 4;
    uint32_t actualStride = widthBytes; /* 默认紧凑 */
    if (usedMapPlanes && planes.planeCount > 0 && planes.planes[0].rowStride > 0) {
        actualStride = planes.planes[0].rowStride;
    }
    frame.stride = actualStride;
    frame.data = (uint8_t *)virAddr;

    g_captureCtx.callback(&frame, g_captureCtx.userData);

    OH_NativeBuffer_Unmap(buffer);
    OH_AVScreenCapture_ReleaseVideoBuffer(capture);
}

static void on_error(OH_AVScreenCapture *capture, int32_t errorCode)
{
    (void)capture;
    LOG_TAG_E(CAPTURE_TAG, "Screen capture error: %d", errorCode);
}

int screen_capture_create(const ScreenCaptureConfig *config)
{
    memset(&g_captureCtx, 0, sizeof(g_captureCtx));
    memcpy(&g_captureCtx.config, config, sizeof(ScreenCaptureConfig));

    uint32_t dispW = 0, dispH = 0;
    if (screen_capture_get_display_size(&dispW, &dispH) != 0) {
        LOG_TAG_E(CAPTURE_TAG, "Failed to get display size");
        return -1;
    }

    if (config->scale > 0 && config->scale < 1.0f) {
        g_captureCtx.actualWidth = (uint32_t)(dispW * config->scale);
        g_captureCtx.actualHeight = (uint32_t)(dispH * config->scale);
    } else {
        g_captureCtx.actualWidth = dispW;
        g_captureCtx.actualHeight = dispH;
    }

    g_captureCtx.actualWidth &= ~1;
    g_captureCtx.actualHeight &= ~1;

    LOG_TAG_I(CAPTURE_TAG, "Screen capture: %ux%u -> %ux%u (scale=%.2f)",
              dispW, dispH, g_captureCtx.actualWidth, g_captureCtx.actualHeight,
              config->scale);

    return 0;
}

int screen_capture_get_display_size(uint32_t *width, uint32_t *height)
{
    void *lib = dlopen("libnative_display_manager.so", RTLD_LAZY);
    if (lib) {
        typedef int (*get_size_t)(int32_t *);
        get_size_t get_width = (get_size_t)dlsym(lib,
            "OH_NativeDisplayManager_GetDefaultDisplayWidth");
        get_size_t get_height = (get_size_t)dlsym(lib,
            "OH_NativeDisplayManager_GetDefaultDisplayHeight");

        if (get_width && get_height) {
            int32_t w = 0, h = 0;
            if (get_width(&w) == 0 && get_height(&h) == 0 && w > 0 && h > 0) {
                *width = (uint32_t)w;
                *height = (uint32_t)h;
                dlclose(lib);
                LOG_TAG_I(CAPTURE_TAG, "Display size from display manager: %ux%u", *width, *height);
                return 0;
            }
        }
        dlclose(lib);
    }

    /* 尝试多个 sysfs 路径获取屏幕分辨率 */
    const char *sysfsPaths[] = {
        "/sys/class/display/panel0/resolution",
        "/sys/class/display/master_display/primary/modes",
        "/sys/class/display/master_display/modes",
        NULL
    };

    for (int i = 0; sysfsPaths[i] != NULL; i++) {
        FILE *fp = fopen(sysfsPaths[i], "r");
        if (fp) {
            int w = 0, h = 0;
            if (fscanf(fp, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                *width = (uint32_t)w;
                *height = (uint32_t)h;
                fclose(fp);
                LOG_TAG_I(CAPTURE_TAG, "Display size from %s: %ux%u", sysfsPaths[i], *width, *height);
                return 0;
            }
            fclose(fp);
        }
    }

    LOG_TAG_W(CAPTURE_TAG, "Using default display size: %dx%d",
              DEFAULT_SCREEN_WIDTH, DEFAULT_SCREEN_HEIGHT);
    *width = DEFAULT_SCREEN_WIDTH;
    *height = DEFAULT_SCREEN_HEIGHT;
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

    g_captureCtx.capture = OH_AVScreenCapture_Create();
    if (g_captureCtx.capture == NULL) {
        LOG_TAG_E(CAPTURE_TAG, "Failed to create AVScreenCapture");
        return -1;
    }

    OH_AVScreenCaptureConfig captureConfig;
    memset(&captureConfig, 0, sizeof(captureConfig));
    captureConfig.captureMode = OH_CAPTURE_HOME_SCREEN;
    captureConfig.dataType = OH_ORIGINAL_STREAM;
    captureConfig.videoInfo.videoCapInfo.videoFrameWidth = (int32_t)g_captureCtx.actualWidth;
    captureConfig.videoInfo.videoCapInfo.videoFrameHeight = (int32_t)g_captureCtx.actualHeight;
    captureConfig.videoInfo.videoCapInfo.videoSource = OH_VIDEO_SOURCE_SURFACE_RGBA;

    /* 只配置 micCapInfo，不配 innerCapInfo，避免音频参数不兼容 */
    captureConfig.audioInfo.micCapInfo.audioSampleRate = 16000;
    captureConfig.audioInfo.micCapInfo.audioChannels = 2;
    captureConfig.audioInfo.micCapInfo.audioSource = OH_MIC;
    /* innerCapInfo 保持全零（不设置），避免 "set audioSampleRate is not support" 错误 */

    OH_AVScreenCaptureCallback callbackStruct;
    memset(&callbackStruct, 0, sizeof(callbackStruct));
    callbackStruct.onError = on_error;
    callbackStruct.onAudioBufferAvailable = on_audio_buffer_available;
    callbackStruct.onVideoBufferAvailable = on_video_buffer_available;

    OH_AVScreenCapture_SetCallback(g_captureCtx.capture, callbackStruct);

    int32_t ret = OH_AVScreenCapture_Init(g_captureCtx.capture, captureConfig);
    if (ret != 0) {
        LOG_TAG_E(CAPTURE_TAG, "AVScreenCapture init failed: %d", ret);
        OH_AVScreenCapture_Release(g_captureCtx.capture);
        g_captureCtx.capture = NULL;
        return -1;
    }

    /* 关闭麦克风，我们只需要视频画面 */
    OH_AVScreenCapture_SetMicrophoneEnabled(g_captureCtx.capture, false);

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
