/*
 * OHOS Scrcpy 客户端 - 主入口
 * 协调设备管理、网络通信、视频解码、渲染和输入处理
 * 跨平台实现 (Linux/macOS/Windows)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>

#include "hdc_manager.h"
#include "tcp_client.h"
#include "video_decoder.h"
#include "renderer.h"
#include "input_handler.h"
#include "../common/log.h"
#include "../common/protocol.h"

#include <SDL2/SDL.h>

#define MAIN_TAG "Main"

typedef struct {
    char     serial[128];
    float    scale;
    uint32_t bitrate;
    uint32_t fps;
    bool     fullscreen;
    bool     alwaysOnTop;
    bool     borderless;
    uint16_t videoPort;
    uint16_t controlPort;
    char     hdcPath[512];
} AppConfig;

static AppConfig g_appConfig = {
    .serial     = "",
    .scale      = 0.5f,
    .bitrate    = 4000000,
    .fps        = 60,
    .fullscreen = false,
    .alwaysOnTop = false,
    .borderless = false,
    .videoPort  = DEFAULT_VIDEO_PORT,
    .controlPort = DEFAULT_CONTROL_PORT,
    .hdcPath    = "hdc",
};

static volatile bool g_running = true;

/* 是否处于原始 RGBA 模式（无编码器） */
static volatile bool g_rawRgbaMode = false;

/* RGBA 帧缓冲区（线程安全：视频接收线程写入，主线程读取渲染） */
static uint8_t *g_rgbaBuffer = NULL;
static int g_rgbaWidth = 0;
static int g_rgbaHeight = 0;
static volatile bool g_rgbaFrameReady = false;
static pthread_mutex_t g_rgbaMutex = PTHREAD_MUTEX_INITIALIZER;

/* 待处理的视频配置（视频接收线程写入，主线程读取并应用）
 * 避免在视频线程调用 renderer_update_video_size / SDL_SetWindowSize，
 * 这些操作必须在主线程执行，否则会破坏 GL 上下文。 */
static volatile bool g_pendingVideoConfig = false;
static uint16_t g_pendingWidth = 0;
static uint16_t g_pendingHeight = 0;

static void signal_handler(int sig)
{
    (void)sig;
    g_running = false;
    LOG_TAG_I(MAIN_TAG, "Shutting down...");
}

static void parse_args(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            strncpy(g_appConfig.serial, argv[++i], sizeof(g_appConfig.serial) - 1);
        } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            g_appConfig.scale = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) {
            g_appConfig.bitrate = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            g_appConfig.fps = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fullscreen") == 0 || strcmp(argv[i], "-f") == 0) {
            g_appConfig.fullscreen = true;
        } else if (strcmp(argv[i], "--always-on-top") == 0) {
            g_appConfig.alwaysOnTop = true;
        } else if (strcmp(argv[i], "--borderless") == 0) {
            g_appConfig.borderless = true;
        } else if (strcmp(argv[i], "--video-port") == 0 && i + 1 < argc) {
            g_appConfig.videoPort = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--control-port") == 0 && i + 1 < argc) {
            g_appConfig.controlPort = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--hdc") == 0 && i + 1 < argc) {
            strncpy(g_appConfig.hdcPath, argv[++i], sizeof(g_appConfig.hdcPath) - 1);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("OHOS Scrcpy - OpenHarmony Screen Mirroring Tool\n\n"
                   "Usage: ohos_scrcpy [options]\n\n"
                   "Options:\n"
                   "  -s SERIAL           Device serial number\n"
                   "  --scale SCALE       Screen scale (default: 0.5)\n"
                   "  --bitrate BPS       Video bitrate (default: 4000000)\n"
                   "  --fps FPS           Target FPS (default: 60)\n"
                   "  -f, --fullscreen    Start in fullscreen\n"
                   "  --always-on-top     Window always on top\n"
                   "  --borderless        Borderless window\n"
                   "  --video-port PORT   Video port (default: 9901)\n"
                   "  --control-port PORT Control port (default: 9902)\n"
                   "  --hdc PATH          Path to hdc executable\n"
                   "  -h, --help          Show this help\n\n"
                   "Keyboard Shortcuts:\n"
                   "  Alt+B               Back key\n"
                   "  Alt+H               Home key\n"
                   "  Alt+P               Power key\n"
                   "  Alt+Up              Volume up\n"
                   "  Alt+Down            Volume down\n"
                   "  F11                 Toggle fullscreen\n"
                   "  ESC                 Exit fullscreen\n");
            exit(0);
        }
    }
}

/* 视频帧回调 */
static void on_video_frame(VideoMessageType type,
                           const uint8_t *data,
                           uint32_t length,
                           uint64_t timestamp,
                           void *userData)
{
    (void)userData;

    switch (type) {
        case MSG_VIDEO_SPS_PPS: {
            if (length < 8) return;
            uint32_t spsLen = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
                              ((uint32_t)data[2] << 8) | data[3];
            /* 越界检查：确保读取 ppsLen 时不会越界 */
            if (4 + spsLen + 4 > length) {
                LOG_TAG_W(MAIN_TAG, "SPS/PPS invalid: spsLen=%u, length=%u", spsLen, length);
                return;
            }
            uint32_t ppsLen = ((uint32_t)data[4 + spsLen] << 24) |
                              ((uint32_t)data[4 + spsLen + 1] << 16) |
                              ((uint32_t)data[4 + spsLen + 2] << 8) |
                              data[4 + spsLen + 3];
            const uint8_t *spsData = data + 4;
            const uint8_t *ppsData = data + 4 + spsLen + 4;

            if (spsLen + ppsLen + 8 <= length) {
                video_decoder_set_sps_pps(spsData, spsLen, ppsData, ppsLen);
            }
            break;
        }
        case MSG_VIDEO_IDR_FRAME:
            video_decoder_decode(data, length, timestamp, true);
            break;
        case MSG_VIDEO_P_FRAME:
            video_decoder_decode(data, length, timestamp, false);
            break;
        case MSG_VIDEO_CONFIG: {
            if (length >= sizeof(VideoConfig)) {
                VideoConfig config;
                memcpy(&config, data, sizeof(config));
                /* 不在视频线程调用 renderer_update_video_size（内部会
                 * SDL_SetWindowSize，破坏 GL 上下文），改为通知主线程处理 */
                g_pendingWidth = config.width;
                g_pendingHeight = config.height;
                g_pendingVideoConfig = true;
                input_handler_update_screen_size(config.width, config.height);
                LOG_TAG_I(MAIN_TAG, "Video config: %ux%u @ %ufps, %ubps",
                          config.width, config.height, config.fps, config.bitrate);
            }
            break;
        }
        case MSG_VIDEO_RAW_RGBA: {
            if (length < sizeof(RawFrameHeader)) return;
            RawFrameHeader rawHeader;
            memcpy(&rawHeader, data, sizeof(RawFrameHeader));
            int w = rawHeader.width;
            int h = rawHeader.height;
            uint32_t pixelLen = length - sizeof(RawFrameHeader);
            const uint8_t *pixelData = data + sizeof(RawFrameHeader);

            if (pixelLen < (uint32_t)(w * h * 4)) {
                LOG_TAG_W(MAIN_TAG, "Raw RGBA frame too small: %u < %d",
                          pixelLen, w * h * 4);
                return;
            }

            if (!g_rawRgbaMode) {
                g_rawRgbaMode = true;
                LOG_TAG_I(MAIN_TAG, "Detected raw RGBA mode (no H.264 encoder on device)");
            }

            /* 首帧诊断：检查像素数据 */
            static int rgbaFrameCount = 0;
            rgbaFrameCount++;
            if (rgbaFrameCount <= 3) {
                uint32_t nonZero = 0;
                for (uint32_t i = 0; i < 256 && i < pixelLen; i++) {
                    if (pixelData[i] != 0) nonZero++;
                }
                LOG_TAG_I(MAIN_TAG, "RGBA frame #%d: %ux%u len=%u nonZero=%u/256 first4=[%02x %02x %02x %02x]",
                          rgbaFrameCount, w, h, pixelLen, nonZero,
                          pixelData[0], pixelData[1], pixelData[2], pixelData[3]);
            }

            /* 复制帧数据到缓冲区，由主线程渲染（避免 GL 上下文线程安全问题） */
            pthread_mutex_lock(&g_rgbaMutex);
            uint32_t neededSize = (uint32_t)(w * h * 4);
            if (g_rgbaBuffer == NULL ||
                (uint32_t)(g_rgbaWidth * g_rgbaHeight * 4) != neededSize) {
                free(g_rgbaBuffer);
                g_rgbaBuffer = (uint8_t *)malloc(neededSize);
                g_rgbaWidth = w;
                g_rgbaHeight = h;
            }
            if (g_rgbaBuffer) {
                memcpy(g_rgbaBuffer, pixelData, neededSize);
                g_rgbaFrameReady = true;
            }
            pthread_mutex_unlock(&g_rgbaMutex);
            break;
        }
    }
}

/* 解码帧回调 */
static void on_decoded_frame(DecodedFrame *frame, void *userData)
{
    (void)userData;
    renderer_render_frame(
        frame->data[0], frame->linesize[0],
        frame->data[1], frame->linesize[1],
        frame->data[2], frame->linesize[2],
        frame->width, frame->height);
}

/* 控制消息回调 */
static void on_control_message(const ControlHeader *header,
                               const uint8_t *data,
                               void *userData)
{
    (void)header; (void)data; (void)userData;
}

/* 心跳线程 */
static void *heartbeat_thread(void *param)
{
    (void)param;
    while (g_running) {
        tcp_client_send_heartbeat();
        usleep(3000000);  /* 3秒一次心跳 */
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    LOG_TAG_I(MAIN_TAG, "OHOS Scrcpy - OpenHarmony Screen Mirroring Tool");

    parse_args(argc, argv);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* 1. 初始化 HDC 管理器 */
    HdcManagerConfig hdcConfig = {0};
    strncpy(hdcConfig.hdcPath, g_appConfig.hdcPath, sizeof(hdcConfig.hdcPath) - 1);

    if (hdc_manager_init(&hdcConfig) != 0) {
        LOG_TAG_E(MAIN_TAG, "HDC init failed. Is hdc in PATH?");
        return 1;
    }

    /* 2. 查找设备 */
    DeviceInfo *devices = NULL;
    int deviceCount = 0;

    if (hdc_manager_get_devices(&devices, &deviceCount) != 0 || deviceCount == 0) {
        LOG_TAG_E(MAIN_TAG, "No devices found. Please connect a device via USB.");
        return 1;
    }

    const char *serial = g_appConfig.serial;
    if (serial[0] == '\0') {
        serial = devices[0].serial;
        LOG_TAG_I(MAIN_TAG, "Auto-selected device: %s", serial);
    } else {
        if (!hdc_manager_is_device_connected(serial)) {
            LOG_TAG_E(MAIN_TAG, "Device %s not found", serial);
            return 1;
        }
    }

    /* 3. 设置端口转发 */
    LOG_TAG_I(MAIN_TAG, "Setting up port forwarding...");
    if (hdc_manager_forward_port(serial, g_appConfig.videoPort,
                                  g_appConfig.videoPort) != 0) {
        LOG_TAG_E(MAIN_TAG, "Video port forward failed");
        return 1;
    }
    if (hdc_manager_forward_port(serial, g_appConfig.controlPort,
                                  g_appConfig.controlPort) != 0) {
        LOG_TAG_E(MAIN_TAG, "Control port forward failed");
        hdc_manager_remove_forward(serial, g_appConfig.videoPort);
        return 1;
    }

    /* 4. 启动设备端服务 */
    LOG_TAG_I(MAIN_TAG, "Starting server on device...");
    if (hdc_manager_start_server(serial, g_appConfig.videoPort,
                                  g_appConfig.controlPort, g_appConfig.scale,
                                  g_appConfig.bitrate, g_appConfig.fps) != 0) {
        LOG_TAG_E(MAIN_TAG, "Failed to start server on device");
        hdc_manager_remove_forward(serial, g_appConfig.videoPort);
        hdc_manager_remove_forward(serial, g_appConfig.controlPort);
        return 1;
    }

    /* 5. 创建 TCP 客户端并连接 */
    if (tcp_client_create() != 0) {
        LOG_TAG_E(MAIN_TAG, "TCP client create failed");
        goto cleanup;
    }

    LOG_TAG_I(MAIN_TAG, "Connecting to device...");
    int retryCount = 0;
    while (retryCount < 10) {
        if (tcp_client_connect("127.0.0.1", g_appConfig.videoPort,
                               g_appConfig.controlPort) == 0) {
            break;
        }
        usleep(500000);
        retryCount++;
    }

    if (retryCount >= 10) {
        LOG_TAG_E(MAIN_TAG, "Connection timeout");
        goto cleanup;
    }

    /* 6. 握手 */
    HandshakeResponse hsResp;
    if (tcp_client_handshake(&hsResp) != 0) {
        LOG_TAG_E(MAIN_TAG, "Handshake failed");
        goto cleanup;
    }

    /* 7. 创建视频解码器（RGBA 模式下可能不需要，但创建失败不退出） */
    if (video_decoder_create() != 0) {
        LOG_TAG_W(MAIN_TAG, "Decoder create failed (may use raw RGBA mode)");
    } else {
        video_decoder_set_callback(on_decoded_frame, NULL);
    }

    /* 8. 创建渲染器 */
    RendererConfig renderConfig = {
        .windowX = -1,
        .windowY = -1,
        .windowWidth = 0,
        .windowHeight = 0,
        .fullscreen = g_appConfig.fullscreen,
        .alwaysOnTop = g_appConfig.alwaysOnTop,
        .borderless = g_appConfig.borderless,
    };
    if (renderer_create(&renderConfig) != 0) {
        LOG_TAG_E(MAIN_TAG, "Renderer create failed");
        goto cleanup;
    }

    /* 9. 创建输入处理器 */
    InputHandlerConfig inputConfig = {
        .screenWidth = hsResp.screenWidth,
        .screenHeight = hsResp.screenHeight,
        .captureMouse = true,
        .captureKeyboard = true,
    };
    input_handler_create(&inputConfig);

    /* 10. 启动网络接收线程 */
    tcp_client_start_video_receiver(on_video_frame, NULL);
    tcp_client_start_control_receiver(on_control_message, NULL);

    /* 启动心跳线程 */
    pthread_t hbThread;
    pthread_create(&hbThread, NULL, heartbeat_thread, NULL);

    LOG_TAG_I(MAIN_TAG, "Ready! (Press Ctrl+C or close window to exit)");

    /* 11. 主循环 */
    while (g_running && !renderer_should_close()) {
        /* 处理所有 SDL 事件（窗口、输入等） */
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT:
                    g_running = false;
                    break;
                case SDL_WINDOWEVENT:
                    /* 窗口事件交给渲染器处理（resize、viewport 更新） */
                    renderer_handle_event(&event);
                    break;
                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEBUTTONUP: {
                    int action = (event.type == SDL_MOUSEBUTTONDOWN) ? 0 : 1;
                    input_handler_process_mouse_event(
                        event.button.x, event.button.y,
                        event.button.button, action, 0, 0);
                    break;
                }
                case SDL_MOUSEMOTION: {
                    if (event.motion.state & SDL_BUTTON_LMASK) {
                        input_handler_process_mouse_event(
                            event.motion.x, event.motion.y,
                            SDL_BUTTON_LEFT, 2, 0, 0);
                    }
                    break;
                }
                case SDL_MOUSEWHEEL: {
                    int mx, my;
                    SDL_GetMouseState(&mx, &my);
                    input_handler_process_mouse_event(
                        mx, my, 0, -1,
                        event.wheel.x, event.wheel.y);
                    break;
                }
                case SDL_KEYDOWN:
                case SDL_KEYUP: {
                    /* F11/ESC 全屏切换交给渲染器处理 */
                    if (event.type == SDL_KEYDOWN) {
                        if (event.key.keysym.sym == SDLK_F11) {
                            renderer_handle_event(&event);
                            break;
                        }
                        if (event.key.keysym.sym == SDLK_ESCAPE) {
                            renderer_handle_event(&event);
                            break;
                        }
                    }
                    int action = (event.type == SDL_KEYDOWN) ? 0 : 1;
                    input_handler_process_key_event(
                        event.key.keysym.sym,
                        event.key.keysym.scancode,
                        action, event.key.keysym.mod);
                    break;
                }
            }
        }

        /* 在主线程中应用待处理的视频配置（GL 上下文安全） */
        if (g_pendingVideoConfig) {
            renderer_update_video_size(g_pendingWidth, g_pendingHeight);
            g_pendingVideoConfig = false;
        }

        /* 在主线程中渲染 RGBA 帧（GL 上下文安全） */
        if (g_rawRgbaMode) {
            pthread_mutex_lock(&g_rgbaMutex);
            if (g_rgbaFrameReady && g_rgbaBuffer) {
                renderer_render_rgba_frame(g_rgbaBuffer, g_rgbaWidth, g_rgbaHeight);
                g_rgbaFrameReady = false;
            }
            pthread_mutex_unlock(&g_rgbaMutex);
        }

        int winW, winH;
        renderer_get_window_size(&winW, &winH);
        input_handler_update_window_size(winW, winH);

        RenderStats stats;
        renderer_get_stats(&stats);
        if (stats.fps > 0) {
            char title[128];
            snprintf(title, sizeof(title), "OHOS Scrcpy - %u fps", stats.fps);
            SDL_SetWindowTitle(renderer_get_window(), title);
        }

        usleep(1000);  /* 降低 CPU 占用 */
    }

    g_running = false;
    pthread_join(hbThread, NULL);

cleanup:
    LOG_TAG_I(MAIN_TAG, "Cleaning up...");

    free(g_rgbaBuffer);
    g_rgbaBuffer = NULL;

    hdc_manager_stop_server(serial);
    hdc_manager_remove_forward(serial, g_appConfig.videoPort);
    hdc_manager_remove_forward(serial, g_appConfig.controlPort);

    input_handler_destroy();
    renderer_destroy();
    video_decoder_destroy();
    tcp_client_destroy();
    hdc_manager_destroy();

    LOG_TAG_I(MAIN_TAG, "Exited");
    return 0;
}
