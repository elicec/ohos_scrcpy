/*
 * OHOS Scrcpy 服务端 - 输入注入实现
 * 使用 dlopen 动态加载 OH_Input API
 */

#define TOUCH_ACTION_DOWN  PROTO_TOUCH_DOWN
#define TOUCH_ACTION_UP    PROTO_TOUCH_UP
#define TOUCH_ACTION_MOVE  PROTO_TOUCH_MOVE
#define KEY_ACTION_DOWN    PROTO_KEY_DOWN
#define KEY_ACTION_UP      PROTO_KEY_UP

#include "input_injector.h"
#include "../../common/log.h"

#undef TOUCH_ACTION_DOWN
#undef TOUCH_ACTION_UP
#undef TOUCH_ACTION_MOVE
#undef KEY_ACTION_DOWN
#undef KEY_ACTION_UP

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/time.h>
#include <dlfcn.h>

#define INPUT_TAG "Input"

typedef struct Input_KeyEvent Input_KeyEvent;
typedef struct Input_TouchEvent Input_TouchEvent;

typedef Input_KeyEvent*   (*create_key_event_t)(void);
typedef void              (*destroy_key_event_t)(Input_KeyEvent**);
typedef void              (*set_key_action_t)(Input_KeyEvent*, int32_t);
typedef void              (*set_key_code_t)(Input_KeyEvent*, int32_t);
typedef void              (*set_key_time_t)(Input_KeyEvent*, int64_t);
typedef int32_t           (*inject_key_t)(Input_KeyEvent*);

typedef Input_TouchEvent* (*create_touch_event_t)(void);
typedef void              (*destroy_touch_event_t)(Input_TouchEvent**);
typedef void              (*set_touch_action_t)(Input_TouchEvent*, int32_t);
typedef void              (*set_touch_finger_t)(Input_TouchEvent*, int32_t);
typedef void              (*set_touch_x_t)(Input_TouchEvent*, int32_t);
typedef void              (*set_touch_y_t)(Input_TouchEvent*, int32_t);
typedef void              (*set_touch_time_t)(Input_TouchEvent*, int64_t);
typedef int32_t           (*inject_touch_t)(Input_TouchEvent*);

static struct {
    void *lib;
    create_key_event_t   create_key;
    destroy_key_event_t  destroy_key;
    set_key_action_t     set_key_action;
    set_key_code_t       set_key_code;
    set_key_time_t       set_key_time;
    inject_key_t         inject_key;

    create_touch_event_t  create_touch;
    destroy_touch_event_t destroy_touch;
    set_touch_action_t    set_touch_action;
    set_touch_finger_t    set_touch_finger;
    set_touch_x_t         set_touch_x;
    set_touch_y_t         set_touch_y;
    set_touch_time_t      set_touch_time;
    inject_touch_t        inject_touch;

    bool available;
} g_input;

#define KEYCODE_BACK        2
#define KEYCODE_HOME        1
#define KEYCODE_POWER       18
#define KEYCODE_VOLUME_UP   16
#define KEYCODE_VOLUME_DOWN 17

#define NDK_TOUCH_DOWN  1
#define NDK_TOUCH_MOVE  2
#define NDK_TOUCH_UP    3
#define NDK_KEY_DOWN    1
#define NDK_KEY_UP      2

static int64_t get_current_time_millis(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void load_input_lib(void)
{
    g_input.lib = dlopen("libohinput.so", RTLD_LAZY);
    if (!g_input.lib) {
        LOG_TAG_W(INPUT_TAG, "libohinput.so not available, input injection disabled");
        g_input.available = false;
        return;
    }

    g_input.create_key   = (create_key_event_t)dlsym(g_input.lib, "OH_Input_CreateKeyEvent");
    g_input.destroy_key  = (destroy_key_event_t)dlsym(g_input.lib, "OH_Input_DestroyKeyEvent");
    g_input.set_key_action = (set_key_action_t)dlsym(g_input.lib, "OH_Input_SetKeyEventAction");
    g_input.set_key_code   = (set_key_code_t)dlsym(g_input.lib, "OH_Input_SetKeyEventKeyCode");
    g_input.set_key_time   = (set_key_time_t)dlsym(g_input.lib, "OH_Input_SetKeyEventActionTime");
    g_input.inject_key     = (inject_key_t)dlsym(g_input.lib, "OH_Input_InjectKeyEvent");

    g_input.create_touch   = (create_touch_event_t)dlsym(g_input.lib, "OH_Input_CreateTouchEvent");
    g_input.destroy_touch  = (destroy_touch_event_t)dlsym(g_input.lib, "OH_Input_DestroyTouchEvent");
    g_input.set_touch_action = (set_touch_action_t)dlsym(g_input.lib, "OH_Input_SetTouchEventAction");
    g_input.set_touch_finger = (set_touch_finger_t)dlsym(g_input.lib, "OH_Input_SetTouchEventFingerId");
    g_input.set_touch_x      = (set_touch_x_t)dlsym(g_input.lib, "OH_Input_SetTouchEventDisplayX");
    g_input.set_touch_y      = (set_touch_y_t)dlsym(g_input.lib, "OH_Input_SetTouchEventDisplayY");
    g_input.set_touch_time   = (set_touch_time_t)dlsym(g_input.lib, "OH_Input_SetTouchEventActionTime");
    g_input.inject_touch     = (inject_touch_t)dlsym(g_input.lib, "OH_Input_InjectTouchEvent");

    if (g_input.create_key && g_input.inject_key &&
        g_input.create_touch && g_input.inject_touch) {
        g_input.available = true;
        LOG_TAG_I(INPUT_TAG, "Input injection available");
    } else {
        g_input.available = false;
        LOG_TAG_W(INPUT_TAG, "Input API symbols not found, injection disabled");
    }
}

static int inject_key_event(int32_t keyCode, int32_t keyAction)
{
    if (!g_input.available) return -1;

    Input_KeyEvent *keyEvent = g_input.create_key();
    if (keyEvent == NULL) return -1;

    g_input.set_key_action(keyEvent, keyAction);
    g_input.set_key_code(keyEvent, keyCode);
    g_input.set_key_time(keyEvent, get_current_time_millis());

    int32_t ret = g_input.inject_key(keyEvent);
    g_input.destroy_key(&keyEvent);

    return ret == 0 ? 0 : -1;
}

int input_injector_init(void)
{
    load_input_lib();
    return 0;
}

int input_injector_inject_touch(const TouchEvent *event)
{
    if (event == NULL || !g_input.available) return -1;

    Input_TouchEvent *touchEvent = g_input.create_touch();
    if (touchEvent == NULL) return -1;

    int32_t action;
    switch (event->action) {
        case PROTO_TOUCH_DOWN: action = NDK_TOUCH_DOWN; break;
        case PROTO_TOUCH_UP:   action = NDK_TOUCH_UP;   break;
        case PROTO_TOUCH_MOVE: action = NDK_TOUCH_MOVE; break;
        default:
            g_input.destroy_touch(&touchEvent);
            return -1;
    }

    g_input.set_touch_action(touchEvent, action);
    g_input.set_touch_finger(touchEvent, event->pointerId);
    g_input.set_touch_x(touchEvent, (int32_t)event->x);
    g_input.set_touch_y(touchEvent, (int32_t)event->y);
    g_input.set_touch_time(touchEvent, get_current_time_millis());

    int32_t ret = g_input.inject_touch(touchEvent);
    g_input.destroy_touch(&touchEvent);

    return ret == 0 ? 0 : -1;
}

int input_injector_inject_key(const KeyEvent *event)
{
    if (event == NULL || !g_input.available) return -1;

    int32_t action;
    switch (event->action) {
        case PROTO_KEY_DOWN: action = NDK_KEY_DOWN; break;
        case PROTO_KEY_UP:   action = NDK_KEY_UP;   break;
        default: return -1;
    }

    return inject_key_event(event->keycode, action);
}

int input_injector_inject_scroll(const ScrollEvent *event)
{
    if (event == NULL || !g_input.available) return -1;

    Input_TouchEvent *touchEvent = g_input.create_touch();
    if (touchEvent == NULL) return -1;

    int32_t startX = (int32_t)event->x;
    int32_t startY = (int32_t)event->y;
    int32_t endY = startY - (int32_t)(event->dy * 10);

    g_input.set_touch_finger(touchEvent, 0);

    g_input.set_touch_action(touchEvent, NDK_TOUCH_DOWN);
    g_input.set_touch_x(touchEvent, startX);
    g_input.set_touch_y(touchEvent, startY);
    g_input.set_touch_time(touchEvent, get_current_time_millis());
    g_input.inject_touch(touchEvent);

    g_input.set_touch_action(touchEvent, NDK_TOUCH_MOVE);
    g_input.set_touch_y(touchEvent, endY);
    g_input.set_touch_time(touchEvent, get_current_time_millis());
    g_input.inject_touch(touchEvent);

    g_input.set_touch_action(touchEvent, NDK_TOUCH_UP);
    g_input.set_touch_y(touchEvent, endY);
    g_input.set_touch_time(touchEvent, get_current_time_millis());
    g_input.inject_touch(touchEvent);

    g_input.destroy_touch(&touchEvent);
    return 0;
}

int input_injector_inject_back_key(void)
{
    if (!g_input.available) return -1;
    inject_key_event(KEYCODE_BACK, NDK_KEY_DOWN);
    return inject_key_event(KEYCODE_BACK, NDK_KEY_UP);
}

int input_injector_inject_home_key(void)
{
    if (!g_input.available) return -1;
    inject_key_event(KEYCODE_HOME, NDK_KEY_DOWN);
    return inject_key_event(KEYCODE_HOME, NDK_KEY_UP);
}

int input_injector_inject_power_key(void)
{
    if (!g_input.available) return -1;
    inject_key_event(KEYCODE_POWER, NDK_KEY_DOWN);
    return inject_key_event(KEYCODE_POWER, NDK_KEY_UP);
}

int input_injector_inject_volume_up_key(void)
{
    if (!g_input.available) return -1;
    inject_key_event(KEYCODE_VOLUME_UP, NDK_KEY_DOWN);
    return inject_key_event(KEYCODE_VOLUME_UP, NDK_KEY_UP);
}

int input_injector_inject_volume_down_key(void)
{
    if (!g_input.available) return -1;
    inject_key_event(KEYCODE_VOLUME_DOWN, NDK_KEY_DOWN);
    return inject_key_event(KEYCODE_VOLUME_DOWN, NDK_KEY_UP);
}

void input_injector_destroy(void)
{
    if (g_input.lib) {
        dlclose(g_input.lib);
        g_input.lib = NULL;
    }
    g_input.available = false;
    LOG_TAG_I(INPUT_TAG, "Input injector destroyed");
}
