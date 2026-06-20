/*
 * OHOS Scrcpy 客户端 - 渲染模块
 * 使用 SDL2 + OpenGL 渲染视频帧
 */

#ifndef RENDERER_H
#define RENDERER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 渲染器配置 */
typedef struct {
    int      windowX;       /* 窗口X位置，-1=居中 */
    int      windowY;       /* 窗口Y位置，-1=居中 */
    int      windowWidth;   /* 窗口宽度，0=自适应 */
    int      windowHeight;  /* 窗口高度，0=自适应 */
    bool     fullscreen;    /* 是否全屏 */
    bool     alwaysOnTop;   /* 是否置顶 */
    bool     borderless;    /* 无边框模式 */
} RendererConfig;

/* 渲染统计 */
typedef struct {
    uint32_t fps;           /* 当前FPS */
    uint32_t frameCount;    /* 总帧数 */
    uint32_t dropFrames;    /* 丢帧数 */
    float    avgLatency;    /* 平均延迟(ms) */
} RenderStats;

/* 创建渲染器 */
int renderer_create(const RendererConfig *config);

/* 更新视频尺寸 */
int renderer_update_video_size(int width, int height);

/* 渲染一帧(YUV420P) */
int renderer_render_frame(const uint8_t *yData, int yStride,
                          const uint8_t *uData, int uStride,
                          const uint8_t *vData, int vStride,
                          int width, int height);

/* 处理 SDL 事件(非阻塞) */
bool renderer_poll_events(void);

/* 切换全屏 */
void renderer_toggle_fullscreen(void);

/* 请求窗口关闭 */
bool renderer_should_close(void);

/* 获取窗口尺寸 */
void renderer_get_window_size(int *width, int *height);

/* 获取渲染统计 */
void renderer_get_stats(RenderStats *stats);

/* 截图 */
int renderer_take_screenshot(const char *path);

/* 销毁渲染器 */
void renderer_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* RENDERER_H */
