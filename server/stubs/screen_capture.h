/* 桩头文件 - 模拟 screen_capture.h 用于验证编译 */
#ifndef SCREEN_CAPTURE_STUB_H
#define SCREEN_CAPTURE_STUB_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t displayId;
    uint32_t width;
    uint32_t height;
    float    scale;
    uint32_t fps;
} ScreenCaptureConfig;

typedef struct {
    uint8_t  *data;
    uint32_t  width;
    uint32_t  height;
    uint32_t  stride;
    uint64_t  timestamp;
} CapturedFrame;

typedef void (*FrameCallback)(CapturedFrame *frame, void *userData);

static inline int screen_capture_create(const ScreenCaptureConfig *config) { (void)config; return 0; }
static inline int screen_capture_get_display_size(uint32_t *w, uint32_t *h) { *w=1080; *h=1920; return 0; }
static inline int screen_capture_start(FrameCallback cb, void *ud) { (void)cb; (void)ud; return 0; }
static inline int screen_capture_stop(void) { return 0; }
static inline void screen_capture_destroy(void) {}

#endif
