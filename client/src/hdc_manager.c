/*
 * OHOS Scrcpy 客户端 - HDC 管理实现
 * 封装 hdc 命令，管理设备连接和服务端部署
 * 跨平台实现 (Linux/macOS/Windows)
 */

#include "hdc_manager.h"
#include "../../common/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef _WIN32
#include <windows.h>
#define SLEEP_MS(ms) Sleep(ms)
#else
#include <sys/wait.h>
#define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

#define HDC_TAG "HDC"
#define MAX_OUTPUT_SIZE 4096
#define SERVER_REMOTE_PATH "/data/local/tmp/ohos_scrcpy_server"

static HdcManagerConfig g_hdcConfig = {0};

/* 执行 hdc 命令并获取输出 */
static int execute_hdc_command(const char *serial, const char *args,
                               char *output, int outputSize)
{
    char command[2048];

    if (serial && serial[0]) {
        snprintf(command, sizeof(command), "\"%s\" -s %s %s 2>&1",
                 g_hdcConfig.hdcPath, serial, args);
    } else {
        snprintf(command, sizeof(command), "\"%s\" %s 2>&1",
                 g_hdcConfig.hdcPath, args);
    }

    if (output && outputSize > 0) {
        FILE *fp = popen(command, "r");
        if (!fp) {
            LOG_TAG_E(HDC_TAG, "popen failed for: %s", command);
            return -1;
        }

        int totalRead = 0;
        while (totalRead < outputSize - 1) {
            int n = (int)fread(output + totalRead, 1, outputSize - 1 - totalRead, fp);
            if (n <= 0) break;
            totalRead += n;
        }
        output[totalRead] = '\0';

        int status = pclose(fp);
        int exitCode = WEXITSTATUS(status);
        return exitCode;
    } else {
        /* 不需要输出，重定向到 /dev/null */
        char fullCmd[2200];
        snprintf(fullCmd, sizeof(fullCmd), "%s > /dev/null 2>&1", command);
        int ret = system(fullCmd);
        return WEXITSTATUS(ret);
    }
}

int hdc_manager_init(const HdcManagerConfig *config)
{
    if (config) {
        memcpy(&g_hdcConfig, config, sizeof(HdcManagerConfig));
    } else {
        strncpy(g_hdcConfig.hdcPath, "hdc", sizeof(g_hdcConfig.hdcPath) - 1);
    }

    /* 验证 hdc 可用 */
    char output[256] = {0};
    int ret = execute_hdc_command(NULL, "version", output, sizeof(output));
    if (ret != 0) {
        LOG_TAG_E(HDC_TAG, "hdc not found or not working (exit=%d)", ret);
        /* 不阻断，允许后续手动操作 */
        return 0;
    }

    LOG_TAG_I(HDC_TAG, "HDC initialized: %s", output);
    return 0;
}

int hdc_manager_get_devices(DeviceInfo **devices, int *count)
{
    static DeviceInfo deviceList[16];
    char output[MAX_OUTPUT_SIZE] = {0};

    int ret = execute_hdc_command(NULL, "list targets", output, sizeof(output));
    if (ret != 0) {
        LOG_TAG_E(HDC_TAG, "Failed to list devices");
        return -1;
    }

    *count = 0;
    char *saveptr = NULL;
    char *line = strtok_r(output, "\r\n", &saveptr);
    while (line && *count < 16) {
        if (strlen(line) > 0 && strstr(line, "[Empty]") == NULL) {
            strncpy(deviceList[*count].serial, line,
                    sizeof(deviceList[*count].serial) - 1);
            deviceList[*count].connected = true;
            (*count)++;
        }
        line = strtok_r(NULL, "\r\n", &saveptr);
    }

    *devices = deviceList;
    return 0;
}

bool hdc_manager_is_device_connected(const char *serial)
{
    char output[256] = {0};
    execute_hdc_command(NULL, "list targets", output, sizeof(output));
    return strstr(output, serial) != NULL;
}

int hdc_manager_push_file(const char *serial, const char *localPath,
                          const char *remotePath)
{
    char args[512];
    snprintf(args, sizeof(args), "file send \"%s\" \"%s\"", localPath, remotePath);

    LOG_TAG_I(HDC_TAG, "Pushing %s -> %s", localPath, remotePath);
    int ret = execute_hdc_command(serial, args, NULL, 0);
    if (ret != 0) {
        LOG_TAG_E(HDC_TAG, "Push file failed");
        return -1;
    }

    LOG_TAG_I(HDC_TAG, "File pushed successfully");
    return 0;
}

int hdc_manager_shell(const char *serial, const char *command,
                      char *output, int outputSize)
{
    char args[1024];
    snprintf(args, sizeof(args), "shell \"%s\"", command);
    return execute_hdc_command(serial, args, output, outputSize);
}

int hdc_manager_start_server(const char *serial, uint16_t videoPort,
                             uint16_t controlPort, float scale,
                             uint32_t bitrate, uint32_t fps)
{
    /* 推送服务端二进制 - 从可执行文件同目录查找 */
    char serverPath[512];
    /* 尝试从当前目录查找 */
    snprintf(serverPath, sizeof(serverPath), "ohos_scrcpy_server");

    if (hdc_manager_push_file(serial, serverPath, SERVER_REMOTE_PATH) != 0) {
        LOG_TAG_E(HDC_TAG, "Failed to push server binary");
        return -1;
    }

    /* 设置可执行权限 */
    hdc_manager_shell(serial, "chmod +x " SERVER_REMOTE_PATH, NULL, 0);

    /* 启动服务端 */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             SERVER_REMOTE_PATH " -v %u -c %u -s %.2f -b %u -f %u &",
             videoPort, controlPort, scale, bitrate, fps);

    LOG_TAG_I(HDC_TAG, "Starting server: %s", cmd);
    int ret = hdc_manager_shell(serial, cmd, NULL, 0);
    if (ret != 0) {
        LOG_TAG_E(HDC_TAG, "Failed to start server");
        return -1;
    }

    /* 等待服务端启动 */
    SLEEP_MS(1000);

    LOG_TAG_I(HDC_TAG, "Server started on device");
    return 0;
}

int hdc_manager_stop_server(const char *serial)
{
    LOG_TAG_I(HDC_TAG, "Stopping server on device");
    return hdc_manager_shell(serial,
                             "pkill -f ohos_scrcpy_server || true",
                             NULL, 0);
}

int hdc_manager_forward_port(const char *serial, uint16_t localPort,
                             uint16_t remotePort)
{
    char args[256];
    snprintf(args, sizeof(args), "fport tcp:%u tcp:%u",
             localPort, remotePort);

    LOG_TAG_I(HDC_TAG, "Forwarding port: local=%u -> remote=%u",
              localPort, remotePort);

    int ret = execute_hdc_command(serial, args, NULL, 0);
    if (ret != 0) {
        LOG_TAG_E(HDC_TAG, "Port forward failed");
        return -1;
    }

    return 0;
}

int hdc_manager_remove_forward(const char *serial, uint16_t localPort)
{
    char args[256];
    snprintf(args, sizeof(args), "fport rm tcp:%u", localPort);
    return execute_hdc_command(serial, args, NULL, 0);
}

void hdc_manager_destroy(void)
{
    LOG_TAG_I(HDC_TAG, "HDC manager destroyed");
}
