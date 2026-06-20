/*
 * OHOS Scrcpy 服务端 - 屏幕捕获模块
 * 使用 AVScreenCapture C-API 捕获屏幕帧数据
 */

#ifndef SCREEN_CAPTURE_H
#define SCREEN_CAPTURE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 屏幕捕获配置 */
typedef struct {
    uint32_t displayId;       /* 显示器ID，默认0(主屏) */
    uint32_t width;           /* 捕获宽度，0=自动 */
    uint32_t height;          /* 捕获高度，0=自动 */
    float    scale;           /* 缩放比例，0.5=半分辨率 */
    uint32_t fps;             /* 目标帧率 */
} ScreenCaptureConfig;

/* 捕获的帧数据 */
typedef struct {
    uint8_t  *data;           /* 帧数据(RGBA格式) */
    uint32_t  width;          /* 帧宽度 */
    uint32_t  height;         /* 帧高度 */
    uint32_t  stride;         /* 行字节数 */
    uint64_t  timestamp;      /* 时间戳(微秒) */
} CapturedFrame;

typedef void (*FrameCallback)(CapturedFrame *frame, void *userData);

/* 创建屏幕捕获器 */
int screen_capture_create(const ScreenCaptureConfig *config);

/* 获取实际屏幕尺寸 */
int screen_capture_get_display_size(uint32_t *width, uint32_t *height);

/* 启动屏幕捕获(异步，通过回调返回帧数据) */
int screen_capture_start(FrameCallback callback, void *userData);

/* 停止屏幕捕获 */
int screen_capture_stop(void);

/* 销毁屏幕捕获器 */
void screen_capture_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* SCREEN_CAPTURE_H */
