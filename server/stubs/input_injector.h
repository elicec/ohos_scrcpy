/* 桩头文件 - 模拟 input_injector.h 用于验证编译 */
#ifndef INPUT_INJECTOR_STUB_H
#define INPUT_INJECTOR_STUB_H

#include <stdint.h>
#include "../../common/protocol.h"

static inline int input_injector_init(void) { return 0; }
static inline int input_injector_inject_touch(const TouchEvent *e) { (void)e; return 0; }
static inline int input_injector_inject_key(const KeyEvent *e) { (void)e; return 0; }
static inline int input_injector_inject_scroll(const ScrollEvent *e) { (void)e; return 0; }
static inline int input_injector_inject_back_key(void) { return 0; }
static inline int input_injector_inject_home_key(void) { return 0; }
static inline int input_injector_inject_power_key(void) { return 0; }
static inline int input_injector_inject_volume_up_key(void) { return 0; }
static inline int input_injector_inject_volume_down_key(void) { return 0; }
static inline void input_injector_destroy(void) {}

#endif
