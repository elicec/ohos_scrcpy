/*
 * OHOS Scrcpy 客户端 - TCP 客户端实现
 * 管理与设备端的视频通道和控制通道 TCP 连接
 * 跨平台实现 (Linux/macOS/Windows)
 */

#include "tcp_client.h"
#include "../../common/log.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
#define INVALID_SOCK INVALID_SOCKET
#define CLOSE_SOCKET closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
typedef int socket_t;
#define INVALID_SOCK (-1)
#define CLOSE_SOCKET close
#endif

#define NET_TAG "Client"
#define RECV_BUF_SIZE (256 * 1024)

typedef struct {
    socket_t videoFd;
    socket_t controlFd;
    ClientState state;
    pthread_t videoThread;
    pthread_t controlThread;
    volatile bool videoThreadRunning;
    volatile bool controlThreadRunning;
    VideoFrameCallback videoCallback;
    void *videoUserData;
    ControlMessageCallback controlCallback;
    void *controlUserData;
} ClientContext;

static ClientContext g_clientCtx = {0};

/* 发送全部数据 */
static int send_all(socket_t fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
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
static int recv_all(socket_t fd, void *buf, size_t len)
{
    char *p = (char *)buf;
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

/* 连接到指定端口 */
static socket_t connect_to_server(const char *host, uint16_t port)
{
    socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_TAG_E(NET_TAG, "Socket create failed: %s", strerror(errno));
        return INVALID_SOCK;
    }

    /* 设置 TCP_NODELAY 降低延迟 */
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOG_TAG_E(NET_TAG, "Connect to %s:%u failed: %s",
                  host, port, strerror(errno));
        CLOSE_SOCKET(fd);
        return INVALID_SOCK;
    }

    LOG_TAG_I(NET_TAG, "Connected to %s:%u", host, port);
    return fd;
}

/* 视频接收线程 */
static void *video_thread_func(void *param)
{
    (void)param;
    uint8_t *recvBuf = (uint8_t *)malloc(RECV_BUF_SIZE);
    if (!recvBuf) return NULL;

    while (g_clientCtx.videoThreadRunning) {
        VideoFrameHeader header;
        if (recv_all(g_clientCtx.videoFd, &header, sizeof(header)) != 0) {
            LOG_TAG_W(NET_TAG, "Video channel disconnected");
            break;
        }

        if (header.length > 0 && header.length < RECV_BUF_SIZE) {
            if (recv_all(g_clientCtx.videoFd, recvBuf, header.length) != 0) {
                LOG_TAG_W(NET_TAG, "Video data recv failed");
                break;
            }

            if (g_clientCtx.videoCallback) {
                g_clientCtx.videoCallback((VideoMessageType)header.type,
                                          recvBuf, header.length,
                                          header.timestamp,
                                          g_clientCtx.videoUserData);
            }
        }
    }

    free(recvBuf);
    g_clientCtx.state = CLIENT_STATE_DISCONNECTED;
    return NULL;
}

/* 控制接收线程 */
static void *control_thread_func(void *param)
{
    (void)param;
    uint8_t recvBuf[4096];

    while (g_clientCtx.controlThreadRunning) {
        ControlHeader header;
        if (recv_all(g_clientCtx.controlFd, &header, sizeof(header)) != 0) {
            break;
        }

        uint8_t *data = NULL;
        if (header.length > 0 && header.length < sizeof(recvBuf)) {
            data = recvBuf;
            if (recv_all(g_clientCtx.controlFd, data, header.length) != 0) {
                break;
            }
        }

        if (g_clientCtx.controlCallback) {
            g_clientCtx.controlCallback(&header, data,
                                        g_clientCtx.controlUserData);
        }
    }

    return NULL;
}

int tcp_client_create(void)
{
    memset(&g_clientCtx, 0, sizeof(g_clientCtx));
    g_clientCtx.videoFd = INVALID_SOCK;
    g_clientCtx.controlFd = INVALID_SOCK;
    g_clientCtx.state = CLIENT_STATE_DISCONNECTED;
    return 0;
}

int tcp_client_connect(const char *host, uint16_t videoPort, uint16_t controlPort)
{
    g_clientCtx.state = CLIENT_STATE_CONNECTING;

    g_clientCtx.videoFd = connect_to_server(host, videoPort);
    if (g_clientCtx.videoFd == INVALID_SOCK) {
        g_clientCtx.state = CLIENT_STATE_ERROR;
        return -1;
    }

    g_clientCtx.controlFd = connect_to_server(host, controlPort);
    if (g_clientCtx.controlFd == INVALID_SOCK) {
        CLOSE_SOCKET(g_clientCtx.videoFd);
        g_clientCtx.videoFd = INVALID_SOCK;
        g_clientCtx.state = CLIENT_STATE_ERROR;
        return -1;
    }

    g_clientCtx.state = CLIENT_STATE_CONNECTED;
    LOG_TAG_I(NET_TAG, "Connected to device (video=%u, control=%u)",
              videoPort, controlPort);
    return 0;
}

int tcp_client_handshake(HandshakeResponse *response)
{
    HandshakeRequest hsReq;
    memset(&hsReq, 0, sizeof(hsReq));
    hsReq.magic = HANDSHAKE_MAGIC;
    hsReq.version = PROTOCOL_VERSION;
    hsReq.videoPort = DEFAULT_VIDEO_PORT;
    hsReq.controlPort = DEFAULT_CONTROL_PORT;

    if (send_all(g_clientCtx.controlFd, &hsReq, sizeof(hsReq)) != 0) {
        LOG_TAG_E(NET_TAG, "Handshake send failed");
        return -1;
    }

    if (recv_all(g_clientCtx.controlFd, response, sizeof(HandshakeResponse)) != 0) {
        LOG_TAG_E(NET_TAG, "Handshake recv failed");
        return -1;
    }

    if (response->magic != HANDSHAKE_MAGIC) {
        LOG_TAG_E(NET_TAG, "Invalid handshake response");
        return -1;
    }

    if (response->result != HANDSHAKE_OK) {
        LOG_TAG_E(NET_TAG, "Handshake failed: %d", response->result);
        return -1;
    }

    LOG_TAG_I(NET_TAG, "Handshake OK, screen: %ux%u",
              response->screenWidth, response->screenHeight);
    return 0;
}

int tcp_client_start_video_receiver(VideoFrameCallback callback, void *userData)
{
    g_clientCtx.videoCallback = callback;
    g_clientCtx.videoUserData = userData;
    g_clientCtx.videoThreadRunning = true;

    if (pthread_create(&g_clientCtx.videoThread, NULL, video_thread_func, NULL) != 0) {
        LOG_TAG_E(NET_TAG, "Video thread create failed");
        return -1;
    }

    return 0;
}

int tcp_client_start_control_receiver(ControlMessageCallback callback, void *userData)
{
    g_clientCtx.controlCallback = callback;
    g_clientCtx.controlUserData = userData;
    g_clientCtx.controlThreadRunning = true;

    if (pthread_create(&g_clientCtx.controlThread, NULL, control_thread_func, NULL) != 0) {
        LOG_TAG_E(NET_TAG, "Control thread create failed");
        return -1;
    }

    return 0;
}

int tcp_client_send_touch(const TouchEvent *event)
{
    if (g_clientCtx.controlFd == INVALID_SOCK) return -1;

    ControlHeader header = { .type = MSG_CTRL_TOUCH, .reserved = 0,
                             .length = sizeof(TouchEvent) };
    if (send_all(g_clientCtx.controlFd, &header, sizeof(header)) != 0) return -1;
    return send_all(g_clientCtx.controlFd, event, sizeof(TouchEvent));
}

int tcp_client_send_key(const KeyEvent *event)
{
    if (g_clientCtx.controlFd == INVALID_SOCK) return -1;

    ControlHeader header = { .type = MSG_CTRL_KEY, .reserved = 0,
                             .length = sizeof(KeyEvent) };
    if (send_all(g_clientCtx.controlFd, &header, sizeof(header)) != 0) return -1;
    return send_all(g_clientCtx.controlFd, event, sizeof(KeyEvent));
}

int tcp_client_send_scroll(const ScrollEvent *event)
{
    if (g_clientCtx.controlFd == INVALID_SOCK) return -1;

    ControlHeader header = { .type = MSG_CTRL_SCROLL, .reserved = 0,
                             .length = sizeof(ScrollEvent) };
    if (send_all(g_clientCtx.controlFd, &header, sizeof(header)) != 0) return -1;
    return send_all(g_clientCtx.controlFd, event, sizeof(ScrollEvent));
}

int tcp_client_send_system_key(ControlMessageType keyType)
{
    if (g_clientCtx.controlFd == INVALID_SOCK) return -1;

    ControlHeader header = { .type = (uint8_t)keyType, .reserved = 0,
                             .length = 0 };
    return send_all(g_clientCtx.controlFd, &header, sizeof(header));
}

int tcp_client_send_heartbeat(void)
{
    if (g_clientCtx.controlFd == INVALID_SOCK) return -1;

    ControlHeader header = { .type = MSG_CTRL_HEARTBEAT, .reserved = 0,
                             .length = 0 };
    return send_all(g_clientCtx.controlFd, &header, sizeof(header));
}

ClientState tcp_client_get_state(void)
{
    return g_clientCtx.state;
}

int tcp_client_disconnect(void)
{
    g_clientCtx.videoThreadRunning = false;
    g_clientCtx.controlThreadRunning = false;

    if (g_clientCtx.videoFd != INVALID_SOCK) {
        CLOSE_SOCKET(g_clientCtx.videoFd);
        g_clientCtx.videoFd = INVALID_SOCK;
    }
    if (g_clientCtx.controlFd != INVALID_SOCK) {
        CLOSE_SOCKET(g_clientCtx.controlFd);
        g_clientCtx.controlFd = INVALID_SOCK;
    }

    if (g_clientCtx.videoThread) {
        pthread_join(g_clientCtx.videoThread, NULL);
        g_clientCtx.videoThread = 0;
    }
    if (g_clientCtx.controlThread) {
        pthread_join(g_clientCtx.controlThread, NULL);
        g_clientCtx.controlThread = 0;
    }

    g_clientCtx.state = CLIENT_STATE_DISCONNECTED;
    LOG_TAG_I(NET_TAG, "Disconnected");
    return 0;
}

void tcp_client_destroy(void)
{
    tcp_client_disconnect();
    memset(&g_clientCtx, 0, sizeof(g_clientCtx));
}
