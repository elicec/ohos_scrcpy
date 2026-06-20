/*
 * OHOS Scrcpy 服务端 - 输入注入模块
 * 使用 OH_Input API 注入触摸和按键事件
 */

#ifndef INPUT_INJECTOR_H
#define INPUT_INJECTOR_H

#include <stdint.h>
#include "../../common/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化输入注入模块 */
int input_injector_init(void);

/* 注入触摸事件 */
int input_injector_inject_touch(const TouchEvent *event);

/* 注入按键事件 */
int input_injector_inject_key(const KeyEvent *event);

/* 注入滚动事件 */
int input_injector_inject_scroll(const ScrollEvent *event);

/* 注入返回键 */
int input_injector_inject_back_key(void);

/* 注入主页键 */
int input_injector_inject_home_key(void);

/* 注入电源键 */
int input_injector_inject_power_key(void);

/* 注入音量+键 */
int input_injector_inject_volume_up_key(void);

/* 注入音量-键 */
int input_injector_inject_volume_down_key(void);

/* 销毁输入注入模块 */
void input_injector_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* INPUT_INJECTOR_H */
