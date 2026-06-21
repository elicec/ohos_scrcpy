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
#ifndef WEXITSTATUS
#define WEXITSTATUS(status) ((int)(status) & 0xff)
#endif
#else
#include <sys/wait.h>
#define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

#define HDC_TAG "HDC"
#define MAX_OUTPUT_SIZE 4096
#define SERVER_REMOTE_PATH "/data/local/tmp/ohos_scrcpy_server"

static HdcManagerConfig g_hdcConfig = {0};
static char g_serverSerial[128] = {0};

#ifdef _WIN32
static int execute_hdc_command(const char *serial, const char *args,
                               char *output, int outputSize)
{
    char command[2048] = {0};

    if (serial && serial[0]) {
        snprintf(command, sizeof(command), "\"%s\" -t %s %s",
                 g_hdcConfig.hdcPath, serial, args);
    } else {
        snprintf(command, sizeof(command), "\"%s\" %s",
                 g_hdcConfig.hdcPath, args);
    }

    SECURITY_ATTRIBUTES sa = {0};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;

    HANDLE hReadPipe = NULL, hWritePipe = NULL;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        LOG_TAG_E(HDC_TAG, "CreatePipe failed");
        return -1;
    }
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {0};
    si.cb = sizeof(STARTUPINFOA);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = NULL;

    PROCESS_INFORMATION pi = {0};

    char cmdLine[2200];
    strncpy(cmdLine, command, sizeof(cmdLine) - 1);
    cmdLine[sizeof(cmdLine) - 1] = '\0';

    BOOL ok = CreateProcessA(NULL, cmdLine, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (!ok) {
        LOG_TAG_E(HDC_TAG, "CreateProcess failed: %lu", GetLastError());
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return -1;
    }

    CloseHandle(hWritePipe);

    int totalRead = 0;
    if (output && outputSize > 0) {
        DWORD avail = 0;
        DWORD bytesRead = 0;
        while (PeekNamedPipe(hReadPipe, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            DWORD toRead = (outputSize - 1 - totalRead < (int)avail) ?
                           (outputSize - 1 - totalRead) : avail;
            if (toRead == 0) break;
            if (!ReadFile(hReadPipe, output + totalRead, toRead, &bytesRead, NULL)) break;
            totalRead += bytesRead;
        }
        DWORD waitResult = WaitForSingleObject(pi.hProcess, 30000);
        if (waitResult == WAIT_TIMEOUT) {
            TerminateProcess(pi.hProcess, 1);
        }
        while (PeekNamedPipe(hReadPipe, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            DWORD toRead = (outputSize - 1 - totalRead < (int)avail) ?
                           (outputSize - 1 - totalRead) : avail;
            if (toRead == 0) break;
            if (!ReadFile(hReadPipe, output + totalRead, toRead, &bytesRead, NULL)) break;
            totalRead += bytesRead;
        }
        output[totalRead] = '\0';
    } else {
        char discardBuf[256];
        DWORD bytesRead = 0;
        WaitForSingleObject(pi.hProcess, 30000);
        while (ReadFile(hReadPipe, discardBuf, sizeof(discardBuf), &bytesRead, NULL) && bytesRead > 0) {}
    }

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(hReadPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return (int)exitCode;
}
#else
static int execute_hdc_command(const char *serial, const char *args,
                               char *output, int outputSize)
{
    char command[2048];

    if (serial && serial[0]) {
        snprintf(command, sizeof(command), "\"%s\" -t %s %s 2>&1",
                 g_hdcConfig.hdcPath, serial, args);
    } else {
        snprintf(command, sizeof(command), "\"%s\" %s 2>&1",
                 g_hdcConfig.hdcPath, args);
    }

    FILE *fp = popen(command, "r");
    if (!fp) {
        LOG_TAG_E(HDC_TAG, "popen failed for: %s", command);
        return -1;
    }

    if (output && outputSize > 0) {
        int totalRead = 0;
        while (totalRead < outputSize - 1) {
            int n = (int)fread(output + totalRead, 1, outputSize - 1 - totalRead, fp);
            if (n <= 0) break;
            totalRead += n;
        }
        output[totalRead] = '\0';
    } else {
        char discardBuf[256];
        while (fread(discardBuf, 1, sizeof(discardBuf), fp) > 0) {}
    }

    int status = pclose(fp);
    int exitCode = WEXITSTATUS(status);
    return exitCode;
}
#endif

int hdc_manager_init(const HdcManagerConfig *config)
{
    if (config) {
        memcpy(&g_hdcConfig, config, sizeof(HdcManagerConfig));
    } else {
        strncpy(g_hdcConfig.hdcPath, "hdc", sizeof(g_hdcConfig.hdcPath) - 1);
    }

    char output[256] = {0};
    int ret = execute_hdc_command(NULL, "version", output, sizeof(output));
    if (ret != 0) {
        LOG_TAG_E(HDC_TAG, "hdc not found or not working (exit=%d)", ret);
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
    char output[512] = {0};

    snprintf(args, sizeof(args), "file send \"%s\" \"%s\"", localPath, remotePath);

    LOG_TAG_I(HDC_TAG, "Pushing %s -> %s", localPath, remotePath);
    int ret = execute_hdc_command(serial, args, output, sizeof(output));
    if (ret != 0) {
        LOG_TAG_E(HDC_TAG, "Push file failed: %s", output);
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
    char serverPath[512] = {0};
    char exePath[512] = {0};

#ifdef _WIN32
    GetModuleFileNameA(NULL, exePath, sizeof(exePath));
    char *lastSlash = strrchr(exePath, '\\');
    if (lastSlash) {
        *lastSlash = '\0';
        snprintf(serverPath, sizeof(serverPath), "%s\\ohos_scrcpy_server", exePath);
    } else {
        strncpy(serverPath, "ohos_scrcpy_server", sizeof(serverPath) - 1);
    }
#else
    readlink("/proc/self/exe", exePath, sizeof(exePath));
    char *lastSlash = strrchr(exePath, '/');
    if (lastSlash) {
        *lastSlash = '\0';
        snprintf(serverPath, sizeof(serverPath), "%s/ohos_scrcpy_server", exePath);
    } else {
        strncpy(serverPath, "ohos_scrcpy_server", sizeof(serverPath) - 1);
    }
#endif

    LOG_TAG_I(HDC_TAG, "Server path: %s", serverPath);

    if (hdc_manager_push_file(serial, serverPath, SERVER_REMOTE_PATH) != 0) {
        LOG_TAG_E(HDC_TAG, "Failed to push server binary");
        return -1;
    }

    hdc_manager_shell(serial, "chmod +x " SERVER_REMOTE_PATH, NULL, 0);

    /* 先 kill 旧的服务端进程，避免残留 */
    hdc_manager_stop_server(serial);

    /* 启动服务端，让进程将 PID 写入文件，便于后续 kill */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "nohup %s -v %u -c %u -s %.2f -b %u -f %u > /dev/null 2>&1 & echo $!",
             SERVER_REMOTE_PATH, videoPort, controlPort, scale, bitrate, fps);

    LOG_TAG_I(HDC_TAG, "Starting server: %s", cmd);

    char output[256] = {0};
    int ret = hdc_manager_shell(serial, cmd, output, sizeof(output));
    if (ret != 0) {
        LOG_TAG_E(HDC_TAG, "Failed to start server");
        return -1;
    }

    SLEEP_MS(2000);

    /* 保存当前 serial 供 stop_server 使用 */
    strncpy(g_serverSerial, serial, sizeof(g_serverSerial) - 1);

    LOG_TAG_I(HDC_TAG, "Server started on device (PID output: %s)", output);
    return 0;
}

int hdc_manager_stop_server(const char *serial)
{
    const char *targetSerial = serial;
    if (targetSerial == NULL || targetSerial[0] == '\0') {
        targetSerial = g_serverSerial;
    }
    if (targetSerial == NULL || targetSerial[0] == '\0') {
        return 0;
    }

    LOG_TAG_I(HDC_TAG, "Stopping server on device %s", targetSerial);

    /* 方式1：用 ps | grep 找到 PID 并 kill */
    char output[MAX_OUTPUT_SIZE] = {0};
    int ret = hdc_manager_shell(targetSerial,
                                 "ps -A 2>/dev/null | grep ohos_scrcpy_server | grep -v grep",
                                 output, sizeof(output));
    if (ret == 0 && output[0] != '\0') {
        LOG_TAG_I(HDC_TAG, "Found server processes: %s", output);
        /* 解析 PID 并 kill */
        char *saveptr = NULL;
        char *line = strtok_r(output, "\r\n", &saveptr);
        while (line) {
            /* ps 输出格式通常是: PID USER ... 或 PID ... */
            char pidStr[32] = {0};
            int i = 0;
            while (line[i] == ' ' || line[i] == '\t') i++;
            int j = 0;
            while (line[i] && line[i] != ' ' && line[i] != '\t' && j < 31) {
                pidStr[j++] = line[i++];
            }
            pidStr[j] = '\0';

            if (pidStr[0] != '\0') {
                char killCmd[128];
                snprintf(killCmd, sizeof(killCmd), "kill -9 %s 2>/dev/null", pidStr);
                hdc_manager_shell(targetSerial, killCmd, NULL, 0);
                LOG_TAG_I(HDC_TAG, "Sent kill -9 to PID %s", pidStr);
            }
            line = strtok_r(NULL, "\r\n", &saveptr);
        }
    }

    /* 方式2：尝试 pkill */
    hdc_manager_shell(targetSerial, "pkill -9 ohos_scrcpy_server 2>/dev/null || true",
                      NULL, 0);

    /* 方式3：尝试 killall */
    hdc_manager_shell(targetSerial, "killall -9 ohos_scrcpy_server 2>/dev/null || true",
                      NULL, 0);

    /* 方式4：通过 PID 文件 kill（如果有） */
    hdc_manager_shell(targetSerial,
                      "if [ -f /data/local/tmp/ohos_scrcpy.pid ]; then "
                      "kill -9 $(cat /data/local/tmp/ohos_scrcpy.pid) 2>/dev/null; "
                      "rm -f /data/local/tmp/ohos_scrcpy.pid; fi",
                      NULL, 0);

    SLEEP_MS(500);

    /* 确认是否还有残留 */
    char verify[256] = {0};
    hdc_manager_shell(targetSerial,
                      "ps -A 2>/dev/null | grep ohos_scrcpy_server | grep -v grep | wc -l",
                      verify, sizeof(verify));
    int remaining = atoi(verify);
    if (remaining > 0) {
        LOG_TAG_W(HDC_TAG, "Warning: %d server process(es) still running", remaining);
    } else {
        LOG_TAG_I(HDC_TAG, "Server stopped successfully");
    }

    /* 清除保存的 serial */
    if (targetSerial == g_serverSerial) {
        g_serverSerial[0] = '\0';
    }

    return 0;
}

int hdc_manager_forward_port(const char *serial, uint16_t localPort,
                             uint16_t remotePort)
{
    char args[256];
    char output[512] = {0};

    LOG_TAG_I(HDC_TAG, "Forwarding port: local=%u -> remote=%u",
              localPort, remotePort);

    snprintf(args, sizeof(args), "fport tcp:%u tcp:%u",
             localPort, remotePort);

    int ret = execute_hdc_command(serial, args, output, sizeof(output));
    if (ret == 0) {
        return 0;
    }

    LOG_TAG_I(HDC_TAG, "fport failed, trying fport add");
    snprintf(args, sizeof(args), "fport add tcp:%u tcp:%u",
             localPort, remotePort);

    ret = execute_hdc_command(serial, args, output, sizeof(output));
    if (ret == 0) {
        return 0;
    }

    LOG_TAG_E(HDC_TAG, "Port forward failed: %s", output);
    return -1;
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
