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
    int toolbarWidth;
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

void input_handler_update_toolbar_width(int toolbarWidth)
{
    g_inputCtx.toolbarWidth = toolbarWidth;
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

    /* 计算保持宽高比的渲染区域，排除右侧工具栏 */
    int effectiveWinW = g_inputCtx.windowWidth - g_inputCtx.toolbarWidth;
    if (effectiveWinW <= 0) effectiveWinW = g_inputCtx.windowWidth;

    float winAspect = (float)effectiveWinW / g_inputCtx.windowHeight;
    float screenAspect = (float)g_inputCtx.config.screenWidth /
                         g_inputCtx.config.screenHeight;

    int renderW, renderH, offsetX, offsetY;
    if (winAspect > screenAspect) {
        renderH = g_inputCtx.windowHeight;
        renderW = (int)(renderH * screenAspect);
        offsetX = (effectiveWinW - renderW) / 2;
        offsetY = 0;
    } else {
        renderW = effectiveWinW;
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

/* 将 SDL 键码映射为 Linux input 键码（uinput 需要 Linux 键码） */
static int sdl_keycode_to_linux_keycode(int keycode, int scancode)
{
    (void)scancode;

    /* 字母 a-z */
    if (keycode >= SDLK_a && keycode <= SDLK_z) {
        switch (keycode) {
            case SDLK_a: return 30;
            case SDLK_b: return 48;
            case SDLK_c: return 46;
            case SDLK_d: return 32;
            case SDLK_e: return 18;
            case SDLK_f: return 33;
            case SDLK_g: return 34;
            case SDLK_h: return 35;
            case SDLK_i: return 23;
            case SDLK_j: return 36;
            case SDLK_k: return 37;
            case SDLK_l: return 38;
            case SDLK_m: return 50;
            case SDLK_n: return 49;
            case SDLK_o: return 24;
            case SDLK_p: return 25;
            case SDLK_q: return 16;
            case SDLK_r: return 19;
            case SDLK_s: return 31;
            case SDLK_t: return 20;
            case SDLK_u: return 22;
            case SDLK_v: return 47;
            case SDLK_w: return 17;
            case SDLK_x: return 45;
            case SDLK_y: return 21;
            case SDLK_z: return 44;
        }
    }

    /* 数字 0-9 */
    if (keycode >= SDLK_0 && keycode <= SDLK_9) {
        switch (keycode) {
            case SDLK_1: return 2;
            case SDLK_2: return 3;
            case SDLK_3: return 4;
            case SDLK_4: return 5;
            case SDLK_5: return 6;
            case SDLK_6: return 7;
            case SDLK_7: return 8;
            case SDLK_8: return 9;
            case SDLK_9: return 10;
            case SDLK_0: return 11;
        }
    }

    /* 常用功能键 */
    switch (keycode) {
        case SDLK_RETURN:    return 28;
        case SDLK_ESCAPE:    return 1;
        case SDLK_BACKSPACE: return 14;
        case SDLK_TAB:       return 15;
        case SDLK_SPACE:     return 57;
        case SDLK_UP:        return 103;
        case SDLK_DOWN:      return 108;
        case SDLK_LEFT:      return 105;
        case SDLK_RIGHT:     return 106;
        case SDLK_LSHIFT:    return 42;
        case SDLK_RSHIFT:    return 54;
        case SDLK_LCTRL:     return 29;
        case SDLK_RCTRL:     return 97;
        case SDLK_LALT:      return 56;
        case SDLK_RALT:      return 100;
        case SDLK_PAGEUP:    return 104;
        case SDLK_PAGEDOWN:  return 109;
        case SDLK_HOME:      return 102;
        case SDLK_END:       return 107;
        case SDLK_INSERT:    return 110;
        case SDLK_DELETE:    return 111;
    }

    return 0;
}

int input_handler_process_key_event(int keycode, int scancode,
                                    int action, int mods)
{
    if (!g_inputCtx.config.captureKeyboard) return 0;

    LOG_TAG_I(INPUT_TAG, "key event: keycode=%d scancode=%d action=%d mods=0x%x",
              keycode, scancode, action, mods);

    /* 检查快捷键 */
    ControlMessageType shortcut = input_handler_get_shortcut(keycode, mods);
    if (shortcut != MSG_CTRL_HEARTBEAT) {
        LOG_TAG_I(INPUT_TAG, "shortcut matched: keycode=%d -> msg=%d", keycode, shortcut);
        return tcp_client_send_system_key(shortcut);
    }

    /* 普通按键事件 */
    KeyAction keyAction;
    switch (action) {
        case 0: keyAction = KEY_ACTION_DOWN; break;  /* SDL_KEYDOWN */
        case 1: keyAction = KEY_ACTION_UP;   break;  /* SDL_KEYUP */
        default: return -1;
    }

    int linuxKeycode = sdl_keycode_to_linux_keycode(keycode, scancode);
    if (linuxKeycode == 0) {
        LOG_TAG_W(INPUT_TAG, "Unmapped key: sdl keycode=%d scancode=%d", keycode, scancode);
        return 0;
    }

    KeyEvent keyEvent = {
        .action = (uint8_t)keyAction,
        .reserved = 0,
        .keycode = (uint16_t)linuxKeycode,
    };

    return tcp_client_send_key(&keyEvent);
}

ControlMessageType input_handler_get_shortcut(int keycode, int mods)
{
    /* Alt+b = 返回 */
    if (keycode == SDLK_b && (mods & KMOD_ALT)) {
        return MSG_CTRL_BACK;
    }
    /* Alt+h / Alt+m / Alt+Application = 主页 */
    if ((keycode == SDLK_h || keycode == SDLK_m || keycode == SDLK_APPLICATION) &&
        (mods & KMOD_ALT)) {
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
