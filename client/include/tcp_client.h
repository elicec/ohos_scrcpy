/*
 * OHOS Scrcpy 客户端 - TCP 客户端模块
 * 管理与设备端视频通道和控制通道的 TCP 连接
 */

#ifndef TCP_CLIENT_H
#define TCP_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include "../../common/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLIENT_STATE_DISCONNECTED = 0,
    CLIENT_STATE_CONNECTING,
    CLIENT_STATE_CONNECTED,
    CLIENT_STATE_ERROR,
} ClientState;

/* 视频帧回调 */
typedef void (*VideoFrameCallback)(VideoMessageType type,
                                   const uint8_t *data,
                                   uint32_t length,
                                   uint64_t timestamp,
                                   void *userData);

/* 控制消息回调 */
typedef void (*ControlMessageCallback)(const ControlHeader *header,
                                       const uint8_t *data,
                                       void *userData);

/* 创建 TCP 客户端 */
int tcp_client_create(void);

/* 连接到设备端 */
int tcp_client_connect(const char *host, uint16_t videoPort, uint16_t controlPort);

/* 执行握手 */
int tcp_client_handshake(HandshakeResponse *response);

/* 启动视频接收线程 */
int tcp_client_start_video_receiver(VideoFrameCallback callback, void *userData);

/* 启动控制接收线程 */
int tcp_client_start_control_receiver(ControlMessageCallback callback, void *userData);

/* 发送触摸事件 */
int tcp_client_send_touch(const TouchEvent *event);

/* 发送按键事件 */
int tcp_client_send_key(const KeyEvent *event);

/* 发送滚动事件 */
int tcp_client_send_scroll(const ScrollEvent *event);

/* 发送系统按键(返回/主页/电源等) */
int tcp_client_send_system_key(ControlMessageType keyType);

/* 发送心跳 */
int tcp_client_send_heartbeat(void);

/* 获取客户端状态 */
ClientState tcp_client_get_state(void);

/* 断开连接 */
int tcp_client_disconnect(void);

/* 销毁客户端 */
void tcp_client_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* TCP_CLIENT_H */
