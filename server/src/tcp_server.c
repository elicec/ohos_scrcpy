/*
 * OHOS Scrcpy 服务端 - TCP 服务实现
 * 管理视频通道和控制通道的 TCP 连接
 */

#include "tcp_server.h"
#include "screen_capture.h"
#include "../../common/log.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>

#define NET_TAG "Network"
#define LISTEN_BACKLOG 1
#define RECV_BUF_SIZE 4096

typedef struct {
    int videoFd;
    int controlFd;
    int videoListenFd;
    int controlListenFd;
    ServerConfig config;
    ServerState state;
    ControlMessageCallback ctrlCallback;
    void *ctrlUserData;
    pthread_t controlThread;
    bool controlThreadRunning;
} ServerContext;

static ServerContext g_serverCtx = {0};

/* 发送全部数据 */
static int send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t ret = send(fd, p, remaining, MSG_NOSIGNAL);
        if (ret <= 0) {
            if (ret < 0 && errno == EINTR) continue;
            LOG_TAG_E(NET_TAG, "Send failed: %s", strerror(errno));
            return -1;
        }
        p += ret;
        remaining -= ret;
    }
    return 0;
}

/* 接收全部数据 */
static int recv_all(int fd, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t ret = recv(fd, p, remaining, MSG_WAITALL);
        if (ret <= 0) {
            if (ret < 0 && errno == EINTR) continue;
            return -1;
        }
        p += ret;
        remaining -= ret;
    }
    return 0;
}

/* 等待客户端连接 */
static int wait_for_connection(int listenFd)
{
    struct sockaddr_in clientAddr;
    socklen_t addrLen = sizeof(clientAddr);

    int clientFd = accept(listenFd, (struct sockaddr *)&clientAddr, &addrLen);
    if (clientFd < 0) {
        LOG_TAG_E(NET_TAG, "Accept failed: %s", strerror(errno));
        return -1;
    }

    /* 设置 TCP_NODELAY 降低延迟 */
    int flag = 1;
    setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    LOG_TAG_I(NET_TAG, "Client connected from %s:%d",
              inet_ntoa(clientAddr.sin_addr), ntohs(clientAddr.sin_port));

    return clientFd;
}

/* 创建监听 socket */
static int create_listen_socket(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_TAG_E(NET_TAG, "Socket create failed: %s", strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_TAG_E(NET_TAG, "Bind port %u failed: %s", port, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, LISTEN_BACKLOG) < 0) {
        LOG_TAG_E(NET_TAG, "Listen failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    LOG_TAG_I(NET_TAG, "Listening on port %u", port);
    return fd;
}

/* 控制通道接收线程 */
static void *control_thread_func(void *arg)
{
    (void)arg;
    uint8_t *recvBuf = (uint8_t *)malloc(RECV_BUF_SIZE);
    if (!recvBuf) return NULL;

    while (g_serverCtx.controlThreadRunning) {
        /* 读取控制消息头部 */
        ControlHeader header;
        if (recv_all(g_serverCtx.controlFd, &header, sizeof(header)) != 0) {
            LOG_TAG_W(NET_TAG, "Control channel disconnected");
            break;
        }

        /* 读取消息数据 */
        uint16_t dataLen = header.length;
        uint8_t *data = NULL;
        if (dataLen > 0 && dataLen < RECV_BUF_SIZE) {
            data = recvBuf;
            if (recv_all(g_serverCtx.controlFd, data, dataLen) != 0) {
                LOG_TAG_W(NET_TAG, "Control data recv failed");
                break;
            }
        }

        /* 处理心跳 */
        if (header.type == MSG_CTRL_HEARTBEAT) {
            /* 回复心跳 */
            ControlHeader resp = { .type = MSG_CTRL_HEARTBEAT, .reserved = 0, .length = 0 };
            send_all(g_serverCtx.controlFd, &resp, sizeof(resp));
            continue;
        }

        /* 回调处理 */
        if (g_serverCtx.ctrlCallback) {
            g_serverCtx.ctrlCallback(&header, data, g_serverCtx.ctrlUserData);
        }
    }

    free(recvBuf);
    return NULL;
}

int tcp_server_create(const ServerConfig *config)
{
    memset(&g_serverCtx, 0, sizeof(g_serverCtx));
    g_serverCtx.config = *config;
    g_serverCtx.videoFd = -1;
    g_serverCtx.controlFd = -1;
    g_serverCtx.videoListenFd = -1;
    g_serverCtx.controlListenFd = -1;
    g_serverCtx.state = SERVER_STATE_IDLE;
    return 0;
}

int tcp_server_start(void)
{
    /* 创建监听 socket */
    g_serverCtx.videoListenFd = create_listen_socket(g_serverCtx.config.videoPort);
    if (g_serverCtx.videoListenFd < 0) return -1;

    g_serverCtx.controlListenFd = create_listen_socket(g_serverCtx.config.controlPort);
    if (g_serverCtx.controlListenFd < 0) {
        close(g_serverCtx.videoListenFd);
        return -1;
    }

    g_serverCtx.state = SERVER_STATE_LISTENING;
    LOG_TAG_I(NET_TAG, "Server listening on video=%u, control=%u",
              g_serverCtx.config.videoPort, g_serverCtx.config.controlPort);

    /* 等待视频通道连接 */
    LOG_TAG_I(NET_TAG, "Waiting for video channel connection...");
    g_serverCtx.videoFd = wait_for_connection(g_serverCtx.videoListenFd);
    if (g_serverCtx.videoFd < 0) return -1;

    /* 等待控制通道连接 */
    LOG_TAG_I(NET_TAG, "Waiting for control channel connection...");
    g_serverCtx.controlFd = wait_for_connection(g_serverCtx.controlListenFd);
    if (g_serverCtx.controlFd < 0) {
        close(g_serverCtx.videoFd);
        return -1;
    }

    /* 处理握手 */
    HandshakeRequest hsReq;
    if (recv_all(g_serverCtx.controlFd, &hsReq, sizeof(hsReq)) != 0) {
        LOG_TAG_E(NET_TAG, "Handshake recv failed");
        return -1;
    }

    if (hsReq.magic != HANDSHAKE_MAGIC) {
        LOG_TAG_E(NET_TAG, "Invalid handshake magic: 0x%08X", hsReq.magic);
        return -1;
    }

    if (hsReq.version != PROTOCOL_VERSION) {
        LOG_TAG_E(NET_TAG, "Version mismatch: client=%u, server=%u",
                  hsReq.version, PROTOCOL_VERSION);
    }

    /* 发送握手响应 */
    HandshakeResponse hsResp;
    memset(&hsResp, 0, sizeof(hsResp));
    hsResp.magic = HANDSHAKE_MAGIC;
    hsResp.version = PROTOCOL_VERSION;
    hsResp.result = HANDSHAKE_OK;
    hsResp.videoPort = g_serverCtx.config.videoPort;
    hsResp.controlPort = g_serverCtx.config.controlPort;

    /* 获取屏幕尺寸 */
    uint32_t w = 0, h = 0;
    screen_capture_get_display_size(&w, &h);
    hsResp.screenWidth = (uint16_t)w;
    hsResp.screenHeight = (uint16_t)h;

    if (send_all(g_serverCtx.controlFd, &hsResp, sizeof(hsResp)) != 0) {
        LOG_TAG_E(NET_TAG, "Handshake response send failed");
        return -1;
    }

    /* 启动控制通道接收线程 */
    g_serverCtx.controlThreadRunning = true;
    pthread_create(&g_serverCtx.controlThread, NULL, control_thread_func, NULL);

    g_serverCtx.state = SERVER_STATE_CONNECTED;
    LOG_TAG_I(NET_TAG, "Server connected, screen: %ux%u", w, h);

    return 0;
}

int tcp_server_send_video_frame(VideoMessageType type,
                                const uint8_t *data,
                                uint32_t length,
                                uint64_t timestamp)
{
    if (g_serverCtx.videoFd < 0 || data == NULL || length == 0) {
        return -1;
    }

    VideoFrameHeader header;
    header.type = (uint8_t)type;
    header.length = length;
    header.timestamp = timestamp;

    if (send_all(g_serverCtx.videoFd, &header, sizeof(header)) != 0) {
        return -1;
    }

    return send_all(g_serverCtx.videoFd, data, length);
}

int tcp_server_send_video_config(const VideoConfig *config)
{
    return tcp_server_send_video_frame(MSG_VIDEO_CONFIG,
                                       (const uint8_t *)config,
                                       sizeof(VideoConfig), 0);
}

int tcp_server_send_sps_pps(const uint8_t *sps, uint32_t spsLen,
                            const uint8_t *pps, uint32_t ppsLen)
{
    /* SPS 和 PPS 合并为一帧发送 */
    uint32_t totalLen = spsLen + ppsLen + 8; /* 4字节长度前缀各一个 */
    uint8_t *buf = (uint8_t *)malloc(totalLen);
    if (!buf) return -1;

    /* 写入 SPS (4字节大端长度 + 数据) */
    buf[0] = (spsLen >> 24) & 0xFF;
    buf[1] = (spsLen >> 16) & 0xFF;
    buf[2] = (spsLen >> 8) & 0xFF;
    buf[3] = spsLen & 0xFF;
    memcpy(buf + 4, sps, spsLen);

    /* 写入 PPS (4字节大端长度 + 数据) */
    buf[4 + spsLen] = (ppsLen >> 24) & 0xFF;
    buf[4 + spsLen + 1] = (ppsLen >> 16) & 0xFF;
    buf[4 + spsLen + 2] = (ppsLen >> 8) & 0xFF;
    buf[4 + spsLen + 3] = ppsLen & 0xFF;
    memcpy(buf + 4 + spsLen + 4, pps, ppsLen);

    int ret = tcp_server_send_video_frame(MSG_VIDEO_SPS_PPS, buf, totalLen, 0);
    free(buf);
    return ret;
}

void tcp_server_set_control_callback(ControlMessageCallback callback, void *userData)
{
    g_serverCtx.ctrlCallback = callback;
    g_serverCtx.ctrlUserData = userData;
}

ServerState tcp_server_get_state(void)
{
    return g_serverCtx.state;
}

int tcp_server_stop(void)
{
    g_serverCtx.controlThreadRunning = false;
    g_serverCtx.state = SERVER_STATE_IDLE;

    if (g_serverCtx.controlFd >= 0) {
        close(g_serverCtx.controlFd);
        g_serverCtx.controlFd = -1;
    }
    if (g_serverCtx.videoFd >= 0) {
        close(g_serverCtx.videoFd);
        g_serverCtx.videoFd = -1;
    }
    if (g_serverCtx.videoListenFd >= 0) {
        close(g_serverCtx.videoListenFd);
        g_serverCtx.videoListenFd = -1;
    }
    if (g_serverCtx.controlListenFd >= 0) {
        close(g_serverCtx.controlListenFd);
        g_serverCtx.controlListenFd = -1;
    }

    LOG_TAG_I(NET_TAG, "Server stopped");
    return 0;
}

void tcp_server_destroy(void)
{
    tcp_server_stop();
    memset(&g_serverCtx, 0, sizeof(g_serverCtx));
}
