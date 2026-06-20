/*
 * OHOS Scrcpy 服务端 - 输入注入实现
 * 使用 OH_Input API 注入触摸和按键事件
 */

#include "input_injector.h"
#include "../../common/log.h"

#include <stdlib.h>
#include <string.h>

/* OpenHarmony 输入注入 API */
#include <input/oh_input_manager.h>
#include <input/oh_key_event.h>
#include <input/oh_touch_event.h>

#define INPUT_TAG "Input"

/* 系统按键码映射 */
#define OH_KEYCODE_BACK        1
#define OH_KEYCODE_HOME        2
#define OH_KEYCODE_POWER       10
#define OH_KEYCODE_VOLUME_UP   16
#define OH_KEYCODE_VOLUME_DOWN 17

static int inject_key_event(int32_t keyCode, int32_t keyAction)
{
    Input_KeyEvent *keyEvent = OH_Input_CreateKeyEvent();
    if (keyEvent == NULL) {
        LOG_TAG_E(INPUT_TAG, "Create key event failed");
        return -1;
    }

    OH_Input_SetKeyEventAction(keyEvent, keyAction);
    OH_Input_SetKeyEventKeyCode(keyEvent, keyCode);
    OH_Input_SetKeyEventActionTime(keyEvent, OH_Input_GetCurrentTimeMillis());

    int32_t ret = OH_Input_InjectEvent(keyEvent);
    OH_Input_DestroyKeyEvent(keyEvent);

    if (ret != 0) {
        LOG_TAG_E(INPUT_TAG, "Inject key event failed: %d", ret);
        return -1;
    }

    return 0;
}

int input_injector_init(void)
{
    LOG_TAG_I(INPUT_TAG, "Input injector initialized");
    return 0;
}

int input_injector_inject_touch(const TouchEvent *event)
{
    if (event == NULL) return -1;

    Input_TouchEvent *touchEvent = OH_Input_CreateTouchEvent();
    if (touchEvent == NULL) {
        LOG_TAG_E(INPUT_TAG, "Create touch event failed");
        return -1;
    }

    /* 映射触摸动作 */
    int32_t action;
    switch (event->action) {
        case TOUCH_ACTION_DOWN: action = OH_TOUCH_ACTION_DOWN; break;
        case TOUCH_ACTION_UP:   action = OH_TOUCH_ACTION_UP;   break;
        case TOUCH_ACTION_MOVE: action = OH_TOUCH_ACTION_MOVE; break;
        default:
            OH_Input_DestroyTouchEvent(touchEvent);
            return -1;
    }

    OH_Input_SetTouchEventAction(touchEvent, action);
    OH_Input_SetTouchEventFingerId(touchEvent, event->pointerId);
    OH_Input_SetTouchEventX(touchEvent, (float)event->x);
    OH_Input_SetTouchEventY(touchEvent, (float)event->y);
    OH_Input_SetTouchEventActionTime(touchEvent, OH_Input_GetCurrentTimeMillis());

    int32_t ret = OH_Input_InjectEvent(touchEvent);
    OH_Input_DestroyTouchEvent(touchEvent);

    if (ret != 0) {
        LOG_TAG_E(INPUT_TAG, "Inject touch event failed: %d", ret);
        return -1;
    }

    return 0;
}

int input_injector_inject_key(const KeyEvent *event)
{
    if (event == NULL) return -1;

    int32_t action;
    switch (event->action) {
        case KEY_ACTION_DOWN: action = OH_KEY_ACTION_DOWN; break;
        case KEY_ACTION_UP:   action = OH_KEY_ACTION_UP;   break;
        default: return -1;
    }

    return inject_key_event(event->keycode, action);
}

int input_injector_inject_scroll(const ScrollEvent *event)
{
    if (event == NULL) return -1;

    /* 模拟滚动: 在指定位置执行短距离滑动 */
    Input_TouchEvent *touchEvent = OH_Input_CreateTouchEvent();
    if (touchEvent == NULL) return -1;

    int32_t startX = (int32_t)event->x;
    int32_t startY = (int32_t)event->y;
    int32_t endY = startY - event->dy * 10; /* 放大滚动距离 */

    /* DOWN */
    OH_Input_SetTouchEventAction(touchEvent, OH_TOUCH_ACTION_DOWN);
    OH_Input_SetTouchEventFingerId(touchEvent, 0);
    OH_Input_SetTouchEventX(touchEvent, (float)startX);
    OH_Input_SetTouchEventY(touchEvent, (float)startY);
    OH_Input_SetTouchEventActionTime(touchEvent, OH_Input_GetCurrentTimeMillis());
    OH_Input_InjectEvent(touchEvent);

    /* MOVE */
    OH_Input_SetTouchEventAction(touchEvent, OH_TOUCH_ACTION_MOVE);
    OH_Input_SetTouchEventY(touchEvent, (float)endY);
    OH_Input_SetTouchEventActionTime(touchEvent, OH_Input_GetCurrentTimeMillis());
    OH_Input_InjectEvent(touchEvent);

    /* UP */
    OH_Input_SetTouchEventAction(touchEvent, OH_TOUCH_ACTION_UP);
    OH_Input_SetTouchEventY(touchEvent, (float)endY);
    OH_Input_SetTouchEventActionTime(touchEvent, OH_Input_GetCurrentTimeMillis());
    OH_Input_InjectEvent(touchEvent);

    OH_Input_DestroyTouchEvent(touchEvent);
    return 0;
}

int input_injector_inject_back_key(void)
{
    /* 按下再释放 */
    inject_key_event(OH_KEYCODE_BACK, OH_KEY_ACTION_DOWN);
    return inject_key_event(OH_KEYCODE_BACK, OH_KEY_ACTION_UP);
}

int input_injector_inject_home_key(void)
{
    inject_key_event(OH_KEYCODE_HOME, OH_KEY_ACTION_DOWN);
    return inject_key_event(OH_KEYCODE_HOME, OH_KEY_ACTION_UP);
}

int input_injector_inject_power_key(void)
{
    inject_key_event(OH_KEYCODE_POWER, OH_KEY_ACTION_DOWN);
    return inject_key_event(OH_KEYCODE_POWER, OH_KEY_ACTION_UP);
}

int input_injector_inject_volume_up_key(void)
{
    inject_key_event(OH_KEYCODE_VOLUME_UP, OH_KEY_ACTION_DOWN);
    return inject_key_event(OH_KEYCODE_VOLUME_UP, OH_KEY_ACTION_UP);
}

int input_injector_inject_volume_down_key(void)
{
    inject_key_event(OH_KEYCODE_VOLUME_DOWN, OH_KEY_ACTION_DOWN);
    return inject_key_event(OH_KEYCODE_VOLUME_DOWN, OH_KEY_ACTION_UP);
}

void input_injector_destroy(void)
{
    LOG_TAG_I(INPUT_TAG, "Input injector destroyed");
}
