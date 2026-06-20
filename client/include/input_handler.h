/*
 * OHOS Scrcpy 客户端 - 输入处理模块
 * 捕获鼠标/键盘事件，转换为控制消息发送到设备
 */

#ifndef INPUT_HANDLER_H
#define INPUT_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include "../../common/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 输入处理配置 */
typedef struct {
    uint16_t screenWidth;    /* 设备屏幕宽度 */
    uint16_t screenHeight;   /* 设备屏幕高度 */
    bool     captureMouse;   /* 是否捕获鼠标 */
    bool     captureKeyboard; /* 是否捕获键盘 */
} InputHandlerConfig;

/* 创建输入处理器 */
int input_handler_create(const InputHandlerConfig *config);

/* 更新设备屏幕尺寸(旋转时调用) */
void input_handler_update_screen_size(uint16_t width, uint16_t height);

/* 更新窗口尺寸 */
void input_handler_update_window_size(int windowWidth, int windowHeight);

/* 处理鼠标事件 */
int input_handler_process_mouse_event(int mouseX, int mouseY,
                                      int button, int action,
                                      int wheelX, int wheelY);

/* 处理键盘事件 */
int input_handler_process_key_event(int keycode, int scancode,
                                    int action, int mods);

/* 获取快捷键操作类型，返回 MSG_CTRL_HEARTBEAT 表示非快捷键 */
ControlMessageType input_handler_get_shortcut(int keycode, int mods);

/* 销毁输入处理器 */
void input_handler_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* INPUT_HANDLER_H */
