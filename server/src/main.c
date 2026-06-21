/*
 * OHOS Scrcpy 服务端 - 主入口
 * 协调屏幕捕获、视频编码、输入注入和网络传输
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>

#include "screen_capture.h"
#include "video_encoder.h"
#include "input_injector.h"
#include "tcp_server.h"
#include "../common/log.h"
#include "../common/protocol.h"

/* 编译时检查 RawFrameHeader 大小 */
_Static_assert(sizeof(RawFrameHeader) == 12, "RawFrameHeader must be 12 bytes");

#define MAIN_TAG "Server"

/* 全局运行标志 */
static volatile bool g_running = true;

/* 编码器是否可用 */
static bool g_encoderAvailable = false;

/* 服务端配置 */
typedef struct {
    uint16_t videoPort;
    uint16_t controlPort;
    float    scale;
    uint32_t bitrate;
    uint32_t fps;
    bool     lowLatency;
} ServerAppConfig;

static ServerAppConfig g_appConfig = {
    .videoPort   = DEFAULT_VIDEO_PORT,
    .controlPort = DEFAULT_CONTROL_PORT,
    .scale       = 0.5f,
    .bitrate     = 4000000,
    .fps         = 60,
    .lowLatency  = true,
};

/* 信号处理 */
static void signal_handler(int sig)
{
    (void)sig;
    g_running = false;
    LOG_TAG_I(MAIN_TAG, "Received signal, shutting down...");
}

/* 控制消息处理 */
static void on_control_message(const ControlHeader *header,
                               const uint8_t *data,
                               void *userData)
{
    (void)userData;

    switch (header->type) {
        case MSG_CTRL_TOUCH: {
            if (header->length >= sizeof(TouchEvent)) {
                TouchEvent event;
                memcpy(&event, data, sizeof(event));
                input_injector_inject_touch(&event);
            }
            break;
        }
        case MSG_CTRL_KEY: {
            if (header->length >= sizeof(KeyEvent)) {
                KeyEvent event;
                memcpy(&event, data, sizeof(event));
                input_injector_inject_key(&event);
            }
            break;
        }
        case MSG_CTRL_SCROLL: {
            if (header->length >= sizeof(ScrollEvent)) {
                ScrollEvent event;
                memcpy(&event, data, sizeof(event));
                input_injector_inject_scroll(&event);
            }
            break;
        }
        case MSG_CTRL_BACK:
            input_injector_inject_back_key();
            break;
        case MSG_CTRL_HOME:
            input_injector_inject_home_key();
            break;
        case MSG_CTRL_POWER:
            input_injector_inject_power_key();
            break;
        case MSG_CTRL_VOLUME_UP:
            input_injector_inject_volume_up_key();
            break;
        case MSG_CTRL_VOLUME_DOWN:
            input_injector_inject_volume_down_key();
            break;
        case MSG_CTRL_HEARTBEAT:
            /* 心跳已在 tcp_server 中处理 */
            break;
        default:
            LOG_TAG_W(MAIN_TAG, "Unknown control message type: 0x%02X", header->type);
            break;
    }
}

/* SPS/PPS 回调 - 发送到客户端 */
static void on_sps_pps(const SpsPpsData *spsPps, void *userData)
{
    (void)userData;
    tcp_server_send_sps_pps(spsPps->spsData, spsPps->spsLength,
                            spsPps->ppsData, spsPps->ppsLength);
    LOG_TAG_I(MAIN_TAG, "SPS/PPS sent to client");
}

/* 编码帧回调 - 发送到客户端 */
static void on_encoded_frame(EncodedFrame *frame, void *userData)
{
    (void)userData;

    VideoMessageType type = frame->isKeyFrame ? MSG_VIDEO_IDR_FRAME : MSG_VIDEO_P_FRAME;
    tcp_server_send_video_frame(type, frame->data, frame->length, frame->timestamp);
}

/* 屏幕帧回调 - 送入编码器或直接发送原始 RGBA */
static void on_screen_frame(CapturedFrame *frame, void *userData)
{
    (void)userData;

    if (g_encoderAvailable) {
        video_encoder_encode_frame(frame);
    } else {
        /* 无编码器可用，直接发送原始 RGBA 帧 */
        RawFrameHeader rawHeader;
        rawHeader.width = (uint16_t)frame->width;
        rawHeader.height = (uint16_t)frame->height;
        rawHeader.bufWidth = frame->width;

        /* NativeBuffer 的实际 stride 可能包含对齐 padding（如 540 宽时 stride=544），
         * 直接发送会导致客户端渲染倾斜。此处去除 padding，发送紧凑 RGBA 数据。 */
        uint32_t compactRowBytes = frame->width * 4;
        uint32_t srcRowBytes = frame->stride > 0 ? frame->stride : compactRowBytes;
        uint32_t pixelDataLen = compactRowBytes * frame->height;
        uint32_t totalLen = sizeof(RawFrameHeader) + pixelDataLen;
        rawHeader.stride = compactRowBytes;

        uint8_t *sendBuf = (uint8_t *)malloc(totalLen);
        if (sendBuf == NULL) {
            LOG_TAG_E(MAIN_TAG, "Failed to allocate raw frame buffer");
            return;
        }

        memcpy(sendBuf, &rawHeader, sizeof(RawFrameHeader));
        uint8_t *dst = sendBuf + sizeof(RawFrameHeader);
        const uint8_t *src = frame->data;
        if (srcRowBytes == compactRowBytes) {
            memcpy(dst, src, pixelDataLen);
        } else {
            for (uint32_t row = 0; row < frame->height; row++) {
                memcpy(dst + row * compactRowBytes,
                       src + row * srcRowBytes,
                       compactRowBytes);
            }
        }

        tcp_server_send_video_frame(MSG_VIDEO_RAW_RGBA, sendBuf, totalLen, frame->timestamp);

        free(sendBuf);
    }
}

/* 解析命令行参数 */
static void parse_args(int argc, char *argv[])
{
    static struct option longOptions[] = {
        {"video-port",   required_argument, 0, 'v'},
        {"control-port", required_argument, 0, 'c'},
        {"scale",        required_argument, 0, 's'},
        {"bitrate",      required_argument, 0, 'b'},
        {"fps",          required_argument, 0, 'f'},
        {"help",         no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    int optionIndex = 0;
    while ((opt = getopt_long(argc, argv, "v:c:s:b:f:h", longOptions, &optionIndex)) != -1) {
        switch (opt) {
            case 'v':
                g_appConfig.videoPort = (uint16_t)atoi(optarg);
                break;
            case 'c':
                g_appConfig.controlPort = (uint16_t)atoi(optarg);
                break;
            case 's':
                g_appConfig.scale = (float)atof(optarg);
                break;
            case 'b':
                g_appConfig.bitrate = (uint32_t)atoi(optarg);
                break;
            case 'f':
                g_appConfig.fps = (uint32_t)atoi(optarg);
                break;
            case 'h':
                printf("Usage: ohos_scrcpy_server [options]\n"
                       "Options:\n"
                       "  -v, --video-port PORT    Video channel port (default: %d)\n"
                       "  -c, --control-port PORT  Control channel port (default: %d)\n"
                       "  -s, --scale SCALE        Screen scale factor (default: %.2f)\n"
                       "  -b, --bitrate BPS        Video bitrate (default: %u)\n"
                       "  -f, --fps FPS            Target FPS (default: %u)\n"
                       "  -h, --help               Show this help\n",
                       DEFAULT_VIDEO_PORT, DEFAULT_CONTROL_PORT,
                       g_appConfig.scale, g_appConfig.bitrate, g_appConfig.fps);
                exit(0);
            default:
                break;
        }
    }
}

int main(int argc, char *argv[])
{
    LOG_TAG_I(MAIN_TAG, "OHOS Scrcpy Server starting...");

    parse_args(argc, argv);

    /* 注册信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* 1. 初始化输入注入 */
    if (input_injector_init() != 0) {
        LOG_TAG_E(MAIN_TAG, "Input injector init failed");
        return 1;
    }

    /* 设置输入注入的屏幕坐标范围 */
    {
        uint32_t displayW = 0, displayH = 0;
        screen_capture_get_display_size(&displayW, &displayH);
        input_injector_set_screen_size(displayW, displayH);
    }

    /* 2. 创建屏幕捕获器 */
    ScreenCaptureConfig captureConfig = {
        .displayId = 0,
        .width = 0,
        .height = 0,
        .scale = g_appConfig.scale,
        .fps = g_appConfig.fps,
    };

    if (screen_capture_create(&captureConfig) != 0) {
        LOG_TAG_E(MAIN_TAG, "Screen capture create failed");
        return 1;
    }

    /* 3. 创建 TCP 服务端（先启动 TCP 等待客户端连接） */
    ServerConfig serverConfig = {
        .videoPort = g_appConfig.videoPort,
        .controlPort = g_appConfig.controlPort,
    };

    if (tcp_server_create(&serverConfig) != 0) {
        LOG_TAG_E(MAIN_TAG, "TCP server create failed");
        return 1;
    }

    /* 设置控制消息回调 */
    tcp_server_set_control_callback(on_control_message, NULL);

    /* 4. 启动 TCP 服务(等待客户端连接) - 阻塞直到客户端连接 */
    if (tcp_server_start() != 0) {
        LOG_TAG_E(MAIN_TAG, "TCP server start failed");
        return 1;
    }

    /* 5. 客户端已连接，现在创建视频编码器 */
    VideoEncoderConfig encoderConfig = {
        .width = 0,
        .height = 0,
        .bitrate = g_appConfig.bitrate,
        .fps = g_appConfig.fps,
        .gopSize = g_appConfig.fps,
        .profile = 2,
        .lowLatency = g_appConfig.lowLatency,
    };

    /* 使用捕获器获取的实际尺寸 */
    uint32_t actualW, actualH;
    screen_capture_get_display_size(&actualW, &actualH);
    encoderConfig.width = (uint32_t)(actualW * g_appConfig.scale) & ~1;
    encoderConfig.height = (uint32_t)(actualH * g_appConfig.scale) & ~1;

    if (video_encoder_create(&encoderConfig) != 0) {
        LOG_TAG_W(MAIN_TAG, "Video encoder create failed, falling back to raw RGBA mode");
        g_encoderAvailable = false;
    } else {
        /* 6. 设置编码器回调 */
        video_encoder_set_frame_callback(on_encoded_frame, NULL);
        video_encoder_set_sps_pps_callback(on_sps_pps, NULL);

        /* 7. 启动编码器 */
        if (video_encoder_start() != 0) {
            LOG_TAG_W(MAIN_TAG, "Video encoder start failed, falling back to raw RGBA mode");
            g_encoderAvailable = false;
        } else {
            g_encoderAvailable = true;
        }
    }

    /* 8. 发送视频配置信息 */
    VideoConfig vConfig = {
        .width = (uint16_t)encoderConfig.width,
        .height = (uint16_t)encoderConfig.height,
        .fps = (uint8_t)encoderConfig.fps,
        .reserved = 0,
        .bitrate = encoderConfig.bitrate,
    };
    tcp_server_send_video_config(&vConfig);

    /* 9. 启动屏幕捕获 */
    if (screen_capture_start(on_screen_frame, NULL) != 0) {
        LOG_TAG_W(MAIN_TAG, "Screen capture start failed, running without capture");
    }

    LOG_TAG_I(MAIN_TAG, "Server running... (Ctrl+C to stop)");

    /* 主循环 */
    while (g_running && tcp_server_get_state() == SERVER_STATE_CONNECTED) {
        usleep(100000); /* 100ms 检查一次 */
    }

    /* 清理 */
    LOG_TAG_I(MAIN_TAG, "Shutting down...");
    screen_capture_stop();
    video_encoder_stop();
    tcp_server_stop();

    screen_capture_destroy();
    video_encoder_destroy();
    tcp_server_destroy();
    input_injector_destroy();

    LOG_TAG_I(MAIN_TAG, "Server stopped");
    return 0;
}
