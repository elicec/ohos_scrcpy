/*
 * OHOS Scrcpy - 通用协议定义
 * 定义设备端与PC端之间的通信协议
 */

#ifndef OHOS_SCRCPY_PROTOCOL_H
#define OHOS_SCRCPY_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 协议版本 */
#define PROTOCOL_VERSION 1

/* 默认端口 */
#define DEFAULT_SERVER_PORT 9900

/* 视频通道默认端口 */
#define DEFAULT_VIDEO_PORT 9901

/* 控制通道默认端口 */
#define DEFAULT_CONTROL_PORT 9902

/* 最大帧尺寸 */
#define MAX_FRAME_SIZE (4 * 1024 * 1024)  /* 4MB */

/* 握手魔数 */
#define HANDSHAKE_MAGIC 0x4F534352  /* "OSCR" */

/* ============== 视频通道消息类型 ============== */

typedef enum {
    MSG_VIDEO_SPS_PPS    = 0x01,  /* SPS/PPS 参数集 */
    MSG_VIDEO_IDR_FRAME  = 0x02,  /* IDR 关键帧 */
    MSG_VIDEO_P_FRAME    = 0x03,  /* P帧 */
    MSG_VIDEO_CONFIG     = 0x04,  /* 视频配置信息 */
    MSG_VIDEO_RAW_RGBA   = 0x05,  /* Raw RGBA frame */
} VideoMessageType;

/* 原始帧头部（用于无编码器时直接传输 RGBA 数据） */
typedef struct __attribute__((packed)) {
    uint16_t width;
    uint16_t height;
} RawFrameHeader;

/* 视频帧头部 */
typedef struct __attribute__((packed)) {
    uint8_t  type;           /* VideoMessageType */
    uint32_t length;         /* 数据长度(不含头部) */
    uint64_t timestamp;      /* 时间戳(微秒) */
} VideoFrameHeader;

/* 视频配置信息 */
typedef struct __attribute__((packed)) {
    uint16_t width;          /* 视频宽度 */
    uint16_t height;         /* 视频高度 */
    uint8_t  fps;            /* 帧率 */
    uint8_t  reserved;       /* 保留 */
    uint32_t bitrate;        /* 码率(bps) */
} VideoConfig;

/* ============== 控制通道消息类型 ============== */

typedef enum {
    MSG_CTRL_TOUCH       = 0x10,  /* 触摸事件 */
    MSG_CTRL_KEY         = 0x20,  /* 按键事件 */
    MSG_CTRL_SCROLL      = 0x30,  /* 滚动事件 */
    MSG_CTRL_BACK        = 0x40,  /* 返回键 */
    MSG_CTRL_HOME        = 0x41,  /* 主页键 */
    MSG_CTRL_POWER       = 0x42,  /* 电源键 */
    MSG_CTRL_VOLUME_UP   = 0x43,  /* 音量+ */
    MSG_CTRL_VOLUME_DOWN = 0x44,  /* 音量- */
    MSG_CTRL_SET_SCREEN_SIZE = 0x50, /* 设置屏幕尺寸 */
    MSG_CTRL_HEARTBEAT  = 0xFF,  /* 心跳 */
} ControlMessageType;

/* 控制消息头部 */
typedef struct __attribute__((packed)) {
    uint8_t  type;           /* ControlMessageType */
    uint8_t  reserved;       /* 保留对齐 */
    uint16_t length;         /* 数据长度(不含头部) */
} ControlHeader;

/* 触摸事件 */
typedef enum {
    TOUCH_ACTION_DOWN   = 0,
    TOUCH_ACTION_UP     = 1,
    TOUCH_ACTION_MOVE   = 2,
} TouchAction;

typedef struct __attribute__((packed)) {
    uint8_t  action;         /* TouchAction */
    uint8_t  pointerId;      /* 触摸点ID */
    uint16_t reserved;       /* 保留对齐 */
    uint32_t x;              /* X坐标(设备像素) */
    uint32_t y;              /* Y坐标(设备像素) */
    uint64_t timestamp;      /* 时间戳(微秒) */
} TouchEvent;

/* 按键事件 */
typedef enum {
    KEY_ACTION_DOWN = 0,
    KEY_ACTION_UP   = 1,
} KeyAction;

typedef struct __attribute__((packed)) {
    uint8_t  action;         /* KeyAction */
    uint8_t  reserved;       /* 保留对齐 */
    uint16_t keycode;        /* 键码 */
} KeyEvent;

/* 滚动事件 */
typedef struct __attribute__((packed)) {
    uint32_t x;              /* X坐标 */
    uint32_t y;              /* Y坐标 */
    int32_t  dx;             /* 水平滚动量 */
    int32_t  dy;             /* 垂直滚动量 */
} ScrollEvent;

/* ============== 握手协议 ============== */

/* 客户端→服务端 握手请求 */
typedef struct __attribute__((packed)) {
    uint32_t magic;          /* HANDSHAKE_MAGIC */
    uint32_t version;        /* 协议版本 */
    uint16_t videoPort;      /* 期望的视频端口 */
    uint16_t controlPort;    /* 期望的控制端口 */
} HandshakeRequest;

/* 服务端→客户端 握手响应 */
typedef struct __attribute__((packed)) {
    uint32_t magic;          /* HANDSHAKE_MAGIC */
    uint32_t version;        /* 协议版本 */
    int32_t  result;         /* 0=成功, 负数=错误码 */
    uint16_t videoPort;      /* 实际视频端口 */
    uint16_t controlPort;    /* 实际控制端口 */
    uint16_t screenWidth;    /* 设备屏幕宽度 */
    uint16_t screenHeight;   /* 设备屏幕高度 */
} HandshakeResponse;

/* 握手结果码 */
typedef enum {
    HANDSHAKE_OK              = 0,
    HANDSHAKE_VERSION_MISMATCH = -1,
    HANDSHAKE_PORT_IN_USE      = -2,
    HANDSHAKE_CAPTURE_FAILED   = -3,
    HANDSHAKE_ENCODER_FAILED   = -4,
} HandshakeResult;

#ifdef __cplusplus
}
#endif

#endif /* OHOS_SCRCPY_PROTOCOL_H */
