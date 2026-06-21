/*
 * OHOS Scrcpy 服务端 - 输入注入实现
 * 优先使用 OH_Input API（如果可用），否则回退到 Linux uinput
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
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <dlfcn.h>
#include <linux/input.h>
#include <linux/uinput.h>

#define INPUT_TAG "Input"

/* ============== OH_Input API 类型与状态 ============== */

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
} g_ohInput = {0};

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

/* ============== uinput 状态 ============== */

static struct {
    int touchFd;
    int keyFd;
    bool touchAvailable;
    bool keyAvailable;
    int32_t trackingId;
    uint32_t screenWidth;
    uint32_t screenHeight;
    bool touching;
} g_uinput = {
    .touchFd = -1,
    .keyFd = -1,
    .touchAvailable = false,
    .keyAvailable = false,
    .trackingId = 1,
    .screenWidth = 1080,
    .screenHeight = 1920,
    .touching = false,
};

static InputMode g_inputMode = INPUT_MODE_NONE;

static int64_t get_current_time_millis(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ============== OH_Input 加载 ============== */

static void load_ohinput_lib(void)
{
    /* 某些系统上库名可能是 libohinput.so 或 libinput.so */
    const char *candidates[] = {
        "libohinput.so",
        "libinput.so",
        "/system/lib64/libohinput.so",
        "/system/lib64/libinput.so",
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        g_ohInput.lib = dlopen(candidates[i], RTLD_LAZY);
        if (g_ohInput.lib) {
            LOG_TAG_I(INPUT_TAG, "Found input library: %s", candidates[i]);
            break;
        }
    }

    if (!g_ohInput.lib) {
        LOG_TAG_W(INPUT_TAG, "OH_Input library not available, will try uinput");
        g_ohInput.available = false;
        return;
    }

    g_ohInput.create_key   = (create_key_event_t)dlsym(g_ohInput.lib, "OH_Input_CreateKeyEvent");
    g_ohInput.destroy_key  = (destroy_key_event_t)dlsym(g_ohInput.lib, "OH_Input_DestroyKeyEvent");
    g_ohInput.set_key_action = (set_key_action_t)dlsym(g_ohInput.lib, "OH_Input_SetKeyEventAction");
    g_ohInput.set_key_code   = (set_key_code_t)dlsym(g_ohInput.lib, "OH_Input_SetKeyEventKeyCode");
    g_ohInput.set_key_time   = (set_key_time_t)dlsym(g_ohInput.lib, "OH_Input_SetKeyEventActionTime");
    g_ohInput.inject_key     = (inject_key_t)dlsym(g_ohInput.lib, "OH_Input_InjectKeyEvent");

    g_ohInput.create_touch   = (create_touch_event_t)dlsym(g_ohInput.lib, "OH_Input_CreateTouchEvent");
    g_ohInput.destroy_touch  = (destroy_touch_event_t)dlsym(g_ohInput.lib, "OH_Input_DestroyTouchEvent");
    g_ohInput.set_touch_action = (set_touch_action_t)dlsym(g_ohInput.lib, "OH_Input_SetTouchEventAction");
    g_ohInput.set_touch_finger = (set_touch_finger_t)dlsym(g_ohInput.lib, "OH_Input_SetTouchEventFingerId");
    g_ohInput.set_touch_x      = (set_touch_x_t)dlsym(g_ohInput.lib, "OH_Input_SetTouchEventDisplayX");
    g_ohInput.set_touch_y      = (set_touch_y_t)dlsym(g_ohInput.lib, "OH_Input_SetTouchEventDisplayY");
    g_ohInput.set_touch_time   = (set_touch_time_t)dlsym(g_ohInput.lib, "OH_Input_SetTouchEventActionTime");
    g_ohInput.inject_touch     = (inject_touch_t)dlsym(g_ohInput.lib, "OH_Input_InjectTouchEvent");

    if (g_ohInput.create_key && g_ohInput.inject_key &&
        g_ohInput.create_touch && g_ohInput.inject_touch) {
        g_ohInput.available = true;
        LOG_TAG_I(INPUT_TAG, "OH_Input API available");
    } else {
        g_ohInput.available = false;
        LOG_TAG_W(INPUT_TAG, "OH_Input symbols incomplete, will try uinput");
    }
}

static int ohinput_inject_key_event(int32_t keyCode, int32_t keyAction)
{
    if (!g_ohInput.available) return -1;

    Input_KeyEvent *keyEvent = g_ohInput.create_key();
    if (keyEvent == NULL) return -1;

    g_ohInput.set_key_action(keyEvent, keyAction);
    g_ohInput.set_key_code(keyEvent, keyCode);
    g_ohInput.set_key_time(keyEvent, get_current_time_millis());

    int32_t ret = g_ohInput.inject_key(keyEvent);
    g_ohInput.destroy_key(&keyEvent);

    return ret == 0 ? 0 : -1;
}

/* ============== uinput 实现 ============== */

static void uinput_emit(int fd, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    gettimeofday(&ev.time, NULL);
    ssize_t ret = write(fd, &ev, sizeof(ev));
    if (ret < 0) {
        LOG_TAG_E(INPUT_TAG, "uinput emit failed: %s", strerror(errno));
    } else if ((size_t)ret != sizeof(ev)) {
        LOG_TAG_E(INPUT_TAG, "uinput emit short write: %zd/%zu", ret, sizeof(ev));
    }
}

/* 将一帧内的所有 input_event 打包到一次 write() 中写入，
 * 避免分多次 write 时内核 input 缓冲区被输入服务部分读取，
 * 导致“界面响应上一次坐标”的一帧延迟问题。 */
static int uinput_write_events(int fd, struct input_event *events, size_t count)
{
    if (count == 0) return 0;
    size_t totalLen = count * sizeof(struct input_event);
    ssize_t ret = write(fd, events, totalLen);
    if (ret < 0) {
        LOG_TAG_E(INPUT_TAG, "uinput write_events failed: %s", strerror(errno));
        return -1;
    } else if ((size_t)ret != totalLen) {
        LOG_TAG_E(INPUT_TAG, "uinput write_events short write: %zd/%zu", ret, totalLen);
        return -1;
    }
    return 0;
}

static inline void fill_event(struct input_event *ev,
                              uint16_t type, uint16_t code, int32_t value,
                              const struct timeval *tv)
{
    memset(ev, 0, sizeof(*ev));
    ev->type = type;
    ev->code = code;
    ev->value = value;
    ev->time = *tv;
}

static void uinput_setup_touch_device(void)
{
    int fd = g_uinput.touchFd;

    /* 启用事件类型 */
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);

    /* 设置设备属性为直接触摸设备 */
    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

    /* 按键 - 只声明触摸屏必需的 BTN_TOUCH */
    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);

    /* ABS 坐标轴 - 仅声明多点触摸(MT)坐标轴，与真实触摸屏设备配置一致。
     * 不声明 ABS_X/ABS_Y/ABS_MT_TOOL_TYPE，避免 OHOS 多模输入服务因检测到
     * 单点坐标轴而走单点触摸处理路径，与 MT 协议冲突导致坐标延迟。 */
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_TOUCH_MAJOR);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_PRESSURE);

    struct uinput_setup setup;
    memset(&setup, 0, sizeof(setup));
    /* 使用 BUS_I2C 匹配真实触摸屏设备，避免 OHOS 多模输入服务
     * 对 BUS_VIRTUAL 设备走特殊/延迟处理路径 */
    setup.id.bustype = BUS_I2C;
    setup.id.vendor  = 0x0000;
    setup.id.product = 0x0000;
    setup.id.version = 1;
    strncpy(setup.name, "ohos-scrcpy touch", UINPUT_MAX_NAME_SIZE - 1);

    /* 设置 ABS 范围 */
    struct uinput_abs_setup absSetup;
    memset(&absSetup, 0, sizeof(absSetup));

    absSetup.code = ABS_MT_SLOT;
    absSetup.absinfo.maximum = 9;
    ioctl(fd, UI_ABS_SETUP, &absSetup);

    absSetup.code = ABS_MT_TRACKING_ID;
    absSetup.absinfo.minimum = 0;
    absSetup.absinfo.maximum = 65535;
    ioctl(fd, UI_ABS_SETUP, &absSetup);

    absSetup.code = ABS_MT_POSITION_X;
    absSetup.absinfo.minimum = 0;
    absSetup.absinfo.maximum = g_uinput.screenWidth > 0 ? g_uinput.screenWidth : 1080;
    ioctl(fd, UI_ABS_SETUP, &absSetup);

    absSetup.code = ABS_MT_POSITION_Y;
    absSetup.absinfo.minimum = 0;
    absSetup.absinfo.maximum = g_uinput.screenHeight > 0 ? g_uinput.screenHeight : 1920;
    ioctl(fd, UI_ABS_SETUP, &absSetup);

    absSetup.code = ABS_MT_TOUCH_MAJOR;
    absSetup.absinfo.minimum = 0;
    absSetup.absinfo.maximum = 255;
    ioctl(fd, UI_ABS_SETUP, &absSetup);

    absSetup.code = ABS_MT_PRESSURE;
    absSetup.absinfo.minimum = 0;
    absSetup.absinfo.maximum = 255;
    ioctl(fd, UI_ABS_SETUP, &absSetup);

    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) {
        LOG_TAG_E(INPUT_TAG, "uinput UI_DEV_SETUP failed: %s", strerror(errno));
        return;
    }

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        LOG_TAG_E(INPUT_TAG, "uinput UI_DEV_CREATE failed: %s", strerror(errno));
        return;
    }

    g_uinput.touchAvailable = true;
    LOG_TAG_I(INPUT_TAG, "uinput touch device created (%ux%u)",
              g_uinput.screenWidth, g_uinput.screenHeight);
}

static void uinput_setup_keyboard_device(void)
{
    int fd = g_uinput.keyFd;

    /* 启用事件类型 */
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);

    /* 声明支持常见按键（0-255），避免按键事件被内核丢弃 */
    for (int code = 0; code < 256; code++) {
        ioctl(fd, UI_SET_KEYBIT, code);
    }

    struct uinput_setup setup;
    memset(&setup, 0, sizeof(setup));
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor  = 0x0000;
    setup.id.product = 0x0000;
    setup.id.version = 1;
    strncpy(setup.name, "ohos-scrcpy keyboard", UINPUT_MAX_NAME_SIZE - 1);

    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) {
        LOG_TAG_E(INPUT_TAG, "uinput keyboard UI_DEV_SETUP failed: %s", strerror(errno));
        return;
    }

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        LOG_TAG_E(INPUT_TAG, "uinput keyboard UI_DEV_CREATE failed: %s", strerror(errno));
        return;
    }

    g_uinput.keyAvailable = true;
    LOG_TAG_I(INPUT_TAG, "uinput keyboard device created");
}

static int uinput_open_device(void)
{
    int fd = open("/dev/uinput", O_WRONLY);
    if (fd < 0) {
        fd = open("/dev/input/uinput", O_WRONLY);
    }
    if (fd < 0) {
        LOG_TAG_W(INPUT_TAG, "Cannot open uinput device: %s", strerror(errno));
    }
    return fd;
}

static int uinput_init(void)
{
    if (g_uinput.touchFd >= 0 || g_uinput.keyFd >= 0) return 0;

    /* 使用阻塞写，避免 O_NONBLOCK 下 write 返回 EAGAIN 导致事件/SYN_REPORT 丢失，
     * 进而出现“旧坐标”被再次上报的现象。 */
    int touchFd = uinput_open_device();
    if (touchFd < 0) {
        return -1;
    }

    int keyFd = uinput_open_device();
    if (keyFd < 0) {
        close(touchFd);
        return -1;
    }

    g_uinput.touchFd = touchFd;
    g_uinput.keyFd = keyFd;

    uinput_setup_touch_device();
    uinput_setup_keyboard_device();

    if (!g_uinput.touchAvailable && !g_uinput.keyAvailable) {
        close(g_uinput.touchFd);
        close(g_uinput.keyFd);
        g_uinput.touchFd = -1;
        g_uinput.keyFd = -1;
        return -1;
    }

    /* 等待设备注册完成 */
    usleep(200000);
    return 0;
}

static int uinput_inject_touch(const TouchEvent *event)
{
    if (!g_uinput.touchAvailable || g_uinput.touchFd < 0) return -1;

    int fd = g_uinput.touchFd;
    int x = (int)event->x;
    int y = (int)event->y;
    if (x < 0) x = 0;
    if (x > (int)g_uinput.screenWidth) x = (int)g_uinput.screenWidth;
    if (y < 0) y = 0;
    if (y > (int)g_uinput.screenHeight) y = (int)g_uinput.screenHeight;

    LOG_TAG_I(INPUT_TAG, "inject touch action=%u x=%d y=%d", event->action, x, y);

    /* 一帧内所有事件使用同一时间戳，并打包到一次 write() 中写入，
     * 确保内核 input 子系统能原子地看到本帧全部事件，避免输入服务
     * 读取到“半成品”帧而回退使用上一帧坐标。 */
    struct timeval tv;
    gettimeofday(&tv, NULL);

    struct input_event ev[20];
    size_t n = 0;

    switch (event->action) {
        case PROTO_TOUCH_DOWN:
            g_uinput.touching = true;
            /* DOWN 前先发一帧重置 slot 的所有 ABS 值（TRACKING_ID=-1 + 坐标归零 + SYN_REPORT）。
             * 内核 input 子系统会对未变化的 ABS 值去重（input_handle_abs_event），
             * 导致用户空间 getevent 看不到该事件。若上一次触摸的某个坐标与本次相同，
             * 该坐标事件会被丢弃，OHOS 多模输入服务拿不到当前帧坐标，回退使用上一帧坐标，
             * 表现为"界面响应上一次坐标位置"的一帧延迟。
             * 将坐标归零后，下一次 DOWN 设置实际坐标时值必然变化，内核会重新上报。 */
            fill_event(&ev[n++], EV_ABS, ABS_MT_SLOT, 0, &tv);
            fill_event(&ev[n++], EV_ABS, ABS_MT_TRACKING_ID, -1, &tv);
            fill_event(&ev[n++], EV_ABS, ABS_MT_POSITION_X, 0, &tv);
            fill_event(&ev[n++], EV_ABS, ABS_MT_POSITION_Y, 0, &tv);
            fill_event(&ev[n++], EV_ABS, ABS_MT_TOUCH_MAJOR, 0, &tv);
            fill_event(&ev[n++], EV_ABS, ABS_MT_PRESSURE, 0, &tv);
            fill_event(&ev[n++], EV_SYN, SYN_REPORT, 0, &tv);
            /* 新触摸序列开始 */
            fill_event(&ev[n++], EV_ABS, ABS_MT_SLOT, 0, &tv);
            fill_event(&ev[n++], EV_ABS, ABS_MT_TRACKING_ID, g_uinput.trackingId++, &tv);
            fill_event(&ev[n++], EV_ABS, ABS_MT_POSITION_X, x, &tv);
            fill_event(&ev[n++], EV_ABS, ABS_MT_POSITION_Y, y, &tv);
            fill_event(&ev[n++], EV_ABS, ABS_MT_TOUCH_MAJOR, 40, &tv);
            fill_event(&ev[n++], EV_ABS, ABS_MT_PRESSURE, 80, &tv);
            fill_event(&ev[n++], EV_KEY, BTN_TOUCH, 1, &tv);
            break;
        case PROTO_TOUCH_MOVE:
            if (!g_uinput.touching) return 0;
            fill_event(&ev[n++], EV_ABS, ABS_MT_SLOT, 0, &tv);
            fill_event(&ev[n++], EV_ABS, ABS_MT_POSITION_X, x, &tv);
            fill_event(&ev[n++], EV_ABS, ABS_MT_POSITION_Y, y, &tv);
            fill_event(&ev[n++], EV_ABS, ABS_MT_PRESSURE, 80, &tv);
            break;
        case PROTO_TOUCH_UP:
            g_uinput.touching = false;
            /* UP 时将坐标/触摸面积归零，确保下一次 DOWN 时这些值必然变化，
             * 内核不会因去重而丢弃坐标事件。TRACKING_ID=-1 表示 slot 无触摸，
             * 输入服务会忽略 POSITION 等值，归零不会产生误触摸。 */
            fill_event(&ev[n++], EV_ABS, ABS_MT_SLOT, 0, &tv);
            fill_event(&ev[n++], EV_ABS, ABS_MT_TRACKING_ID, -1, &tv);
            fill_event(&ev[n++], EV_ABS, ABS_MT_POSITION_X, 0, &tv);
            fill_event(&ev[n++], EV_ABS, ABS_MT_POSITION_Y, 0, &tv);
            fill_event(&ev[n++], EV_ABS, ABS_MT_TOUCH_MAJOR, 0, &tv);
            fill_event(&ev[n++], EV_ABS, ABS_MT_PRESSURE, 0, &tv);
            fill_event(&ev[n++], EV_KEY, BTN_TOUCH, 0, &tv);
            break;
        default:
            return -1;
    }

    fill_event(&ev[n++], EV_SYN, SYN_REPORT, 0, &tv);

    return uinput_write_events(fd, ev, n);
}

static int uinput_inject_key(int32_t keyCode, int32_t keyAction)
{
    if (!g_uinput.keyAvailable || g_uinput.keyFd < 0) return -1;

    int value = (keyAction == NDK_KEY_DOWN) ? 1 : 0;

    struct timeval tv;
    gettimeofday(&tv, NULL);

    struct input_event ev[2];
    fill_event(&ev[0], EV_KEY, keyCode, value, &tv);
    fill_event(&ev[1], EV_SYN, SYN_REPORT, 0, &tv);

    int ret = uinput_write_events(g_uinput.keyFd, ev, 2);
    LOG_TAG_I(INPUT_TAG, "inject key code=%d action=%d", keyCode, value);
    return ret;
}

static int uinput_inject_scroll(const ScrollEvent *event)
{
    if (!g_uinput.touchAvailable || g_uinput.touchFd < 0) return -1;

    int startX = (int)event->x;
    int startY = (int)event->y;
    int endY = startY - (int)(event->dy * 50);
    if (endY < 0) endY = 0;
    if (endY > (int)g_uinput.screenHeight) endY = (int)g_uinput.screenHeight;

    int fd = g_uinput.touchFd;
    struct timeval tv;
    gettimeofday(&tv, NULL);

    /* DOWN 帧 - 先 reset slot 的所有 ABS 值再开始新触摸，强制内核刷新坐标 */
    struct input_event evDown[20];
    size_t nDown = 0;
    fill_event(&evDown[nDown++], EV_ABS, ABS_MT_SLOT, 0, &tv);
    fill_event(&evDown[nDown++], EV_ABS, ABS_MT_TRACKING_ID, -1, &tv);
    fill_event(&evDown[nDown++], EV_ABS, ABS_MT_POSITION_X, 0, &tv);
    fill_event(&evDown[nDown++], EV_ABS, ABS_MT_POSITION_Y, 0, &tv);
    fill_event(&evDown[nDown++], EV_ABS, ABS_MT_TOUCH_MAJOR, 0, &tv);
    fill_event(&evDown[nDown++], EV_ABS, ABS_MT_PRESSURE, 0, &tv);
    fill_event(&evDown[nDown++], EV_SYN, SYN_REPORT, 0, &tv);
    fill_event(&evDown[nDown++], EV_ABS, ABS_MT_SLOT, 0, &tv);
    fill_event(&evDown[nDown++], EV_ABS, ABS_MT_TRACKING_ID, g_uinput.trackingId++, &tv);
    fill_event(&evDown[nDown++], EV_ABS, ABS_MT_POSITION_X, startX, &tv);
    fill_event(&evDown[nDown++], EV_ABS, ABS_MT_POSITION_Y, startY, &tv);
    fill_event(&evDown[nDown++], EV_ABS, ABS_MT_TOUCH_MAJOR, 40, &tv);
    fill_event(&evDown[nDown++], EV_ABS, ABS_MT_PRESSURE, 80, &tv);
    fill_event(&evDown[nDown++], EV_KEY, BTN_TOUCH, 1, &tv);
    fill_event(&evDown[nDown++], EV_SYN, SYN_REPORT, 0, &tv);
    if (uinput_write_events(fd, evDown, nDown) != 0) return -1;

    /* MOVE 帧 */
    struct input_event evMove[6];
    size_t nMove = 0;
    fill_event(&evMove[nMove++], EV_ABS, ABS_MT_SLOT, 0, &tv);
    fill_event(&evMove[nMove++], EV_ABS, ABS_MT_POSITION_Y, endY, &tv);
    fill_event(&evMove[nMove++], EV_ABS, ABS_MT_PRESSURE, 80, &tv);
    fill_event(&evMove[nMove++], EV_SYN, SYN_REPORT, 0, &tv);
    if (uinput_write_events(fd, evMove, nMove) != 0) return -1;

    /* UP 帧 - 坐标归零，避免下次触摸坐标被去重 */
    struct input_event evUp[10];
    size_t nUp = 0;
    fill_event(&evUp[nUp++], EV_ABS, ABS_MT_SLOT, 0, &tv);
    fill_event(&evUp[nUp++], EV_ABS, ABS_MT_TRACKING_ID, -1, &tv);
    fill_event(&evUp[nUp++], EV_ABS, ABS_MT_POSITION_X, 0, &tv);
    fill_event(&evUp[nUp++], EV_ABS, ABS_MT_POSITION_Y, 0, &tv);
    fill_event(&evUp[nUp++], EV_ABS, ABS_MT_TOUCH_MAJOR, 0, &tv);
    fill_event(&evUp[nUp++], EV_ABS, ABS_MT_PRESSURE, 0, &tv);
    fill_event(&evUp[nUp++], EV_KEY, BTN_TOUCH, 0, &tv);
    fill_event(&evUp[nUp++], EV_SYN, SYN_REPORT, 0, &tv);
    if (uinput_write_events(fd, evUp, nUp) != 0) return -1;

    return 0;
}

/* ============== 公共接口 ============== */

int input_injector_init(void)
{
    load_ohinput_lib();
    if (g_ohInput.available) {
        g_inputMode = INPUT_MODE_OHINPUT;
        return 0;
    }

    if (uinput_init() == 0) {
        g_inputMode = INPUT_MODE_UINPUT;
        /* 只要触摸或键盘有一个成功，就认为 uinput 可用 */
        return 0;
    }

    g_inputMode = INPUT_MODE_NONE;
    LOG_TAG_E(INPUT_TAG, "No input injection method available");
    return -1;
}

void input_injector_set_screen_size(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) {
        LOG_TAG_W(INPUT_TAG, "Invalid screen size %ux%u, ignoring", width, height);
        return;
    }

    if (g_uinput.screenWidth == width && g_uinput.screenHeight == height) {
        return;
    }

    g_uinput.screenWidth = width;
    g_uinput.screenHeight = height;
    LOG_TAG_I(INPUT_TAG, "Screen size set to %ux%u", width, height);

    /* uinput 触摸设备的 ABS 范围在创建时固定，尺寸变化后需要重建 */
    if (g_inputMode == INPUT_MODE_UINPUT && g_uinput.touchFd >= 0) {
        LOG_TAG_I(INPUT_TAG, "Recreating uinput touch device with new screen size");
        if (g_uinput.touchAvailable) {
            ioctl(g_uinput.touchFd, UI_DEV_DESTROY);
            g_uinput.touchAvailable = false;
        }
        close(g_uinput.touchFd);
        g_uinput.touchFd = -1;
        g_uinput.touching = false;

        int touchFd = uinput_open_device();
        if (touchFd >= 0) {
            g_uinput.touchFd = touchFd;
            uinput_setup_touch_device();
        } else {
            LOG_TAG_E(INPUT_TAG, "Failed to recreate uinput touch device");
        }
    }
}

int input_injector_inject_touch(const TouchEvent *event)
{
    if (event == NULL) return -1;

    if (g_ohInput.available) {
        Input_TouchEvent *touchEvent = g_ohInput.create_touch();
        if (touchEvent == NULL) return -1;

        int32_t action;
        switch (event->action) {
            case PROTO_TOUCH_DOWN: action = NDK_TOUCH_DOWN; break;
            case PROTO_TOUCH_UP:   action = NDK_TOUCH_UP;   break;
            case PROTO_TOUCH_MOVE: action = NDK_TOUCH_MOVE; break;
            default:
                g_ohInput.destroy_touch(&touchEvent);
                return -1;
        }

        g_ohInput.set_touch_action(touchEvent, action);
        g_ohInput.set_touch_finger(touchEvent, event->pointerId);
        g_ohInput.set_touch_x(touchEvent, (int32_t)event->x);
        g_ohInput.set_touch_y(touchEvent, (int32_t)event->y);
        g_ohInput.set_touch_time(touchEvent, get_current_time_millis());

        int32_t ret = g_ohInput.inject_touch(touchEvent);
        g_ohInput.destroy_touch(&touchEvent);
        return ret == 0 ? 0 : -1;
    }

    if (g_uinput.touchAvailable) {
        return uinput_inject_touch(event);
    }

    return -1;
}

int input_injector_inject_key(const KeyEvent *event)
{
    if (event == NULL) return -1;

    int32_t action;
    switch (event->action) {
        case PROTO_KEY_DOWN: action = NDK_KEY_DOWN; break;
        case PROTO_KEY_UP:   action = NDK_KEY_UP;   break;
        default: return -1;
    }

    if (g_ohInput.available) {
        return ohinput_inject_key_event(event->keycode, action);
    }

    if (g_uinput.keyAvailable) {
        return uinput_inject_key(event->keycode, action);
    }

    return -1;
}

int input_injector_inject_scroll(const ScrollEvent *event)
{
    if (event == NULL) return -1;

    if (g_ohInput.available) {
        Input_TouchEvent *touchEvent = g_ohInput.create_touch();
        if (touchEvent == NULL) return -1;

        int32_t startX = (int32_t)event->x;
        int32_t startY = (int32_t)event->y;
        int32_t endY = startY - (int32_t)(event->dy * 10);

        g_ohInput.set_touch_finger(touchEvent, 0);

        g_ohInput.set_touch_action(touchEvent, NDK_TOUCH_DOWN);
        g_ohInput.set_touch_x(touchEvent, startX);
        g_ohInput.set_touch_y(touchEvent, startY);
        g_ohInput.set_touch_time(touchEvent, get_current_time_millis());
        g_ohInput.inject_touch(touchEvent);

        g_ohInput.set_touch_action(touchEvent, NDK_TOUCH_MOVE);
        g_ohInput.set_touch_y(touchEvent, endY);
        g_ohInput.set_touch_time(touchEvent, get_current_time_millis());
        g_ohInput.inject_touch(touchEvent);

        g_ohInput.set_touch_action(touchEvent, NDK_TOUCH_UP);
        g_ohInput.set_touch_y(touchEvent, endY);
        g_ohInput.set_touch_time(touchEvent, get_current_time_millis());
        g_ohInput.inject_touch(touchEvent);

        g_ohInput.destroy_touch(&touchEvent);
        return 0;
    }

    if (g_uinput.touchAvailable) {
        return uinput_inject_scroll(event);
    }

    return -1;
}

int input_injector_inject_back_key(void)
{
    if (g_ohInput.available) {
        ohinput_inject_key_event(KEYCODE_BACK, NDK_KEY_DOWN);
        return ohinput_inject_key_event(KEYCODE_BACK, NDK_KEY_UP);
    }
    if (g_uinput.keyAvailable) {
        uinput_inject_key(KEY_BACK, NDK_KEY_DOWN);
        return uinput_inject_key(KEY_BACK, NDK_KEY_UP);
    }
    return -1;
}

int input_injector_inject_home_key(void)
{
    if (g_ohInput.available) {
        ohinput_inject_key_event(KEYCODE_HOME, NDK_KEY_DOWN);
        return ohinput_inject_key_event(KEYCODE_HOME, NDK_KEY_UP);
    }
    if (g_uinput.keyAvailable) {
        uinput_inject_key(KEY_HOME, NDK_KEY_DOWN);
        return uinput_inject_key(KEY_HOME, NDK_KEY_UP);
    }
    return -1;
}

int input_injector_inject_power_key(void)
{
    if (g_ohInput.available) {
        ohinput_inject_key_event(KEYCODE_POWER, NDK_KEY_DOWN);
        return ohinput_inject_key_event(KEYCODE_POWER, NDK_KEY_UP);
    }
    if (g_uinput.keyAvailable) {
        uinput_inject_key(KEY_POWER, NDK_KEY_DOWN);
        return uinput_inject_key(KEY_POWER, NDK_KEY_UP);
    }
    return -1;
}

int input_injector_inject_volume_up_key(void)
{
    if (g_ohInput.available) {
        ohinput_inject_key_event(KEYCODE_VOLUME_UP, NDK_KEY_DOWN);
        return ohinput_inject_key_event(KEYCODE_VOLUME_UP, NDK_KEY_UP);
    }
    if (g_uinput.keyAvailable) {
        uinput_inject_key(KEY_VOLUMEUP, NDK_KEY_DOWN);
        return uinput_inject_key(KEY_VOLUMEUP, NDK_KEY_UP);
    }
    return -1;
}

int input_injector_inject_volume_down_key(void)
{
    if (g_ohInput.available) {
        ohinput_inject_key_event(KEYCODE_VOLUME_DOWN, NDK_KEY_DOWN);
        return ohinput_inject_key_event(KEYCODE_VOLUME_DOWN, NDK_KEY_UP);
    }
    if (g_uinput.keyAvailable) {
        uinput_inject_key(KEY_VOLUMEDOWN, NDK_KEY_DOWN);
        return uinput_inject_key(KEY_VOLUMEDOWN, NDK_KEY_UP);
    }
    return -1;
}

void input_injector_destroy(void)
{
    if (g_ohInput.lib) {
        dlclose(g_ohInput.lib);
        g_ohInput.lib = NULL;
    }
    g_ohInput.available = false;

    if (g_uinput.touchFd >= 0) {
        if (g_uinput.touchAvailable) {
            ioctl(g_uinput.touchFd, UI_DEV_DESTROY);
        }
        close(g_uinput.touchFd);
        g_uinput.touchFd = -1;
        g_uinput.touchAvailable = false;
    }

    if (g_uinput.keyFd >= 0) {
        if (g_uinput.keyAvailable) {
            ioctl(g_uinput.keyFd, UI_DEV_DESTROY);
        }
        close(g_uinput.keyFd);
        g_uinput.keyFd = -1;
        g_uinput.keyAvailable = false;
    }

    g_inputMode = INPUT_MODE_NONE;
    LOG_TAG_I(INPUT_TAG, "Input injector destroyed");
}
