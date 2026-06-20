/*
 * OHOS Scrcpy 客户端 - HDC 管理模块
 * 封装 hdc 命令，管理设备连接、服务端部署
 */

#ifndef HDC_MANAGER_H
#define HDC_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 设备信息 */
typedef struct {
    char serial[128];       /* 设备序列号 */
    char model[128];        /* 设备型号 */
    bool connected;         /* 是否已连接 */
} DeviceInfo;

/* HDC 管理器配置 */
typedef struct {
    char hdcPath[512];      /* hdc 可执行文件路径 */
} HdcManagerConfig;

/* 初始化 HDC 管理器 */
int hdc_manager_init(const HdcManagerConfig *config);

/* 获取已连接设备列表 */
int hdc_manager_get_devices(DeviceInfo **devices, int *count);

/* 检查设备是否连接 */
bool hdc_manager_is_device_connected(const char *serial);

/* 推送文件到设备 */
int hdc_manager_push_file(const char *serial, const char *localPath,
                          const char *remotePath);

/* 在设备上执行 shell 命令 */
int hdc_manager_shell(const char *serial, const char *command,
                      char *output, int outputSize);

/* 启动设备端服务 */
int hdc_manager_start_server(const char *serial, uint16_t videoPort,
                             uint16_t controlPort, float scale,
                             uint32_t bitrate, uint32_t fps);

/* 停止设备端服务 */
int hdc_manager_stop_server(const char *serial);

/* 设置端口转发 */
int hdc_manager_forward_port(const char *serial, uint16_t localPort,
                             uint16_t remotePort);

/* 移除端口转发 */
int hdc_manager_remove_forward(const char *serial, uint16_t localPort);

/* 销毁 HDC 管理器 */
void hdc_manager_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* HDC_MANAGER_H */
