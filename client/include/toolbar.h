/*
 * OHOS Scrcpy 客户端 - 右侧快捷工具栏
 * 提供返回、主页、电源、音量、截图等快捷按键
 */

#ifndef TOOLBAR_H
#define TOOLBAR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TOOLBAR_WIDTH 64

typedef enum {
    TOOLBAR_ACTION_NONE = -1,
    TOOLBAR_ACTION_BACK = 0,
    TOOLBAR_ACTION_HOME,
    TOOLBAR_ACTION_POWER,
    TOOLBAR_ACTION_VOLUME_UP,
    TOOLBAR_ACTION_VOLUME_DOWN,
    TOOLBAR_ACTION_SCREENSHOT,
    TOOLBAR_ACTION_COUNT
} ToolbarAction;

/* 初始化工具栏（创建 OpenGL 资源） */
int toolbar_init(int window_width, int window_height);

/* 窗口大小变化时更新工具栏布局 */
void toolbar_resize(int window_width, int window_height);

/* 更新鼠标位置，用于 hover 高亮 */
void toolbar_update_mouse(int x, int y);

/* 处理鼠标点击事件，返回触发动作；action: 0=down, 1=up */
ToolbarAction toolbar_handle_mouse_click(int x, int y, int button, int action);

/* 渲染工具栏 */
void toolbar_render(void);

/* 获取工具栏宽度 */
int toolbar_get_width(void);

/* 销毁工具栏资源 */
void toolbar_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* TOOLBAR_H */
