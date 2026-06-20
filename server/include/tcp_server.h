/*
 * OHOS Scrcpy 服务端 - TCP 服务模块
 * 管理视频通道和控制通道的 TCP 连接
 */

#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include "../../common/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 服务端状态 */
typedef enum {
    SERVER_STATE_IDLE = 0,
    SERVER_STATE_LISTENING,
    SERVER_STATE_CONNECTED,
    SERVER_STATE_ERROR,
} ServerState;

/* 服务端配置 */
typedef struct {
    uint16_t videoPort;       /* 视频通道端口 */
    uint16_t controlPort;     /* 控制通道端口 */
} ServerConfig;

/* 控制消息回调 */
typedef void (*ControlMessageCallback)(const ControlHeader *header,
                                       const uint8_t *data,
                                       void *userData);

/* 创建 TCP 服务端 */
int tcp_server_create(const ServerConfig *config);

/* 启动监听(阻塞直到客户端连接) */
int tcp_server_start(void);

/* 发送视频帧数据 */
int tcp_server_send_video_frame(VideoMessageType type,
                                const uint8_t *data,
                                uint32_t length,
                                uint64_t timestamp);

/* 发送视频配置信息 */
int tcp_server_send_video_config(const VideoConfig *config);

/* 发送 SPS/PPS */
int tcp_server_send_sps_pps(const uint8_t *sps, uint32_t spsLen,
                            const uint8_t *pps, uint32_t ppsLen);

/* 设置控制消息回调 */
void tcp_server_set_control_callback(ControlMessageCallback callback,
                                     void *userData);

/* 获取服务端状态 */
ServerState tcp_server_get_state(void);

/* 停止服务端 */
int tcp_server_stop(void);

/* 销毁服务端 */
void tcp_server_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* TCP_SERVER_H */
