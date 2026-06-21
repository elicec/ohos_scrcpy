/*
 * OHOS Scrcpy 客户端 - 输入处理实现
 * 捕获鼠标/键盘事件，转换为控制消息发送到设备
 */

#include "input_handler.h"
#include "tcp_client.h"
#include "../../common/log.h"

#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>

#define INPUT_TAG "Input"

typedef struct {
    InputHandlerConfig config;
    int windowWidth;
    int windowHeight;
} InputHandlerContext;

static InputHandlerContext g_inputCtx = {0};

int input_handler_create(const InputHandlerConfig *config)
{
    memset(&g_inputCtx, 0, sizeof(g_inputCtx));
    if (config) {
        g_inputCtx.config = *config;
    }
    LOG_TAG_I(INPUT_TAG, "Input handler created (screen: %ux%u)",
              config->screenWidth, config->screenHeight);
    return 0;
}

void input_handler_update_screen_size(uint16_t width, uint16_t height)
{
    g_inputCtx.config.screenWidth = width;
    g_inputCtx.config.screenHeight = height;
}

void input_handler_update_window_size(int windowWidth, int windowHeight)
{
    g_inputCtx.windowWidth = windowWidth;
    g_inputCtx.windowHeight = windowHeight;
}

/* 将窗口坐标转换为设备屏幕坐标 */
static void window_to_screen_coord(int winX, int winY,
                                   uint32_t *screenX, uint32_t *screenY)
{
    if (g_inputCtx.windowWidth <= 0 || g_inputCtx.windowHeight <= 0) {
        *screenX = 0;
        *screenY = 0;
        return;
    }

    /* 计算保持宽高比的渲染区域 */
    float winAspect = (float)g_inputCtx.windowWidth / g_inputCtx.windowHeight;
    float screenAspect = (float)g_inputCtx.config.screenWidth /
                         g_inputCtx.config.screenHeight;

    int renderW, renderH, offsetX, offsetY;
    if (winAspect > screenAspect) {
        renderH = g_inputCtx.windowHeight;
        renderW = (int)(renderH * screenAspect);
        offsetX = (g_inputCtx.windowWidth - renderW) / 2;
        offsetY = 0;
    } else {
        renderW = g_inputCtx.windowWidth;
        renderH = (int)(renderW / screenAspect);
        offsetX = 0;
        offsetY = (g_inputCtx.windowHeight - renderH) / 2;
    }

    /* 转换坐标 */
    float relX = (float)(winX - offsetX) / renderW;
    float relY = (float)(winY - offsetY) / renderH;

    /* 限制在 [0, 1] 范围内 */
    if (relX < 0) relX = 0;
    if (relX > 1) relX = 1;
    if (relY < 0) relY = 0;
    if (relY > 1) relY = 1;

    *screenX = (uint32_t)(relX * g_inputCtx.config.screenWidth);
    *screenY = (uint32_t)(relY * g_inputCtx.config.screenHeight);
    LOG_TAG_I(INPUT_TAG, "coord: win(%d,%d) -> screen(%u,%u)",
              winX, winY, *screenX, *screenY);
}

int input_handler_process_mouse_event(int mouseX, int mouseY,
                                      int button, int action,
                                      int wheelX, int wheelY)
{
    if (!g_inputCtx.config.captureMouse) return 0;

    /* 滚轮事件 */
    if (wheelX != 0 || wheelY != 0) {
        uint32_t sx, sy;
        window_to_screen_coord(mouseX, mouseY, &sx, &sy);

        ScrollEvent scrollEvent = {
            .x = sx,
            .y = sy,
            .dx = wheelX,
            .dy = -wheelY,  /* 方向反转 */
        };
        return tcp_client_send_scroll(&scrollEvent);
    }

    /* 触摸事件 */
    TouchAction touchAction;
    switch (action) {
        case 0: touchAction = TOUCH_ACTION_DOWN; break;  /* SDL_MOUSEBUTTONDOWN */
        case 1: touchAction = TOUCH_ACTION_UP;   break;  /* SDL_MOUSEBUTTONUP */
        case 2: touchAction = TOUCH_ACTION_MOVE; break;  /* SDL_MOUSEMOTION */
        default: return -1;
    }

    uint32_t sx, sy;
    window_to_screen_coord(mouseX, mouseY, &sx, &sy);

    TouchEvent touchEvent = {
        .action = (uint8_t)touchAction,
        .pointerId = 0,
        .reserved = 0,
        .x = sx,
        .y = sy,
        .timestamp = 0,
    };

    return tcp_client_send_touch(&touchEvent);
}

int input_handler_process_key_event(int keycode, int scancode,
                                    int action, int mods)
{
    if (!g_inputCtx.config.captureKeyboard) return 0;

    /* 检查快捷键 */
    ControlMessageType shortcut = input_handler_get_shortcut(keycode, mods);
    if (shortcut != MSG_CTRL_HEARTBEAT) {
        return tcp_client_send_system_key(shortcut);
    }

    /* 普通按键事件 */
    KeyAction keyAction;
    switch (action) {
        case 0: keyAction = KEY_ACTION_DOWN; break;  /* SDL_KEYDOWN */
        case 1: keyAction = KEY_ACTION_UP;   break;  /* SDL_KEYUP */
        default: return -1;
    }

    KeyEvent keyEvent = {
        .action = (uint8_t)keyAction,
        .reserved = 0,
        .keycode = (uint16_t)keycode,
    };

    return tcp_client_send_key(&keyEvent);
}

ControlMessageType input_handler_get_shortcut(int keycode, int mods)
{
    /* Alt+b = 返回 */
    if (keycode == SDLK_b && (mods & KMOD_ALT)) {
        return MSG_CTRL_BACK;
    }
    /* Alt+h = 主页 */
    if (keycode == SDLK_h && (mods & KMOD_ALT)) {
        return MSG_CTRL_HOME;
    }
    /* Alt+p = 电源 */
    if (keycode == SDLK_p && (mods & KMOD_ALT)) {
        return MSG_CTRL_POWER;
    }
    /* Alt+↑ = 音量+ */
    if (keycode == SDLK_UP && (mods & KMOD_ALT)) {
        return MSG_CTRL_VOLUME_UP;
    }
    /* Alt+↓ = 音量- */
    if (keycode == SDLK_DOWN && (mods & KMOD_ALT)) {
        return MSG_CTRL_VOLUME_DOWN;
    }

    return MSG_CTRL_HEARTBEAT;  /* 非快捷键 */
}

void input_handler_destroy(void)
{
    memset(&g_inputCtx, 0, sizeof(g_inputCtx));
}
