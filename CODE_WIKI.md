# OHOS Scrcpy - Code Wiki

> OpenHarmony USB 屏幕镜像工具 - 完整代码文档

## 目录

- [1. 项目概述](#1-项目概述)
- [2. 项目整体架构](#2-项目整体架构)
- [3. 目录结构](#3-目录结构)
- [4. 通信协议规范](#4-通信协议规范)
- [5. 客户端模块详解](#5-客户端模块详解)
- [6. 服务端模块详解](#6-服务端模块详解)
- [7. 公共模块](#7-公共模块)
- [8. 依赖关系](#8-依赖关系)
- [9. 构建与运行](#9-构建与运行)
- [10. 关键流程时序](#10-关键流程时序)
- [11. 设计要点与扩展](#11-设计要点与扩展)

---

## 1. 项目概述

### 1.1 项目简介

**OHOS Scrcpy** 是一个面向 OpenHarmony 设备的 USB 屏幕镜像工具，灵感来自 Android 生态的 `scrcpy`。它允许用户在 PC 上实时查看并控制 OpenHarmony 设备的屏幕，支持鼠标/键盘输入回传到设备。

### 1.2 核心特性

| 特性 | 说明 |
|------|------|
| 实时屏幕镜像 | 通过 USB 投屏 OpenHarmony 设备画面到 PC |
| 双通道通信 | 视频通道 + 控制通道分离，降低延迟 |
| H.264 硬件编解码 | 设备端硬件编码，PC 端硬件/软件解码 |
| RGBA 降级模式 | 当编码器不可用时，直接传输原始 RGBA 帧 |
| 输入回传 | 鼠标、键盘、滚轮、系统快捷键注入设备 |
| 跨平台客户端 | 支持 Windows / Linux / macOS |
| OpenGL 渲染 | SDL2 + OpenGL 3.3 Core，YUV→RGB 着色器转换 |
| 低延迟优化 | TCP_NODELAY、禁用 VSync、低延迟编码 tune |

### 1.3 工作原理

```
┌──────────┐   USB + hdc fport   ┌──────────────────────────────┐
│   PC     │ ◄──────────────────► │     OpenHarmony Device       │
│ (Client) │                      │       (Server)               │
│          │  Video Ch. (9901)    │                              │
│  SDL +   │ ◄─────────────────── │  AVScreenCapture → Encoder   │
│  OpenGL  │                      │                              │
│  FFmpeg  │  Control Ch. (9902)  │                              │
│          │ ───────────────────► │  InputInjector (OH_Input)    │
└──────────┘                      └──────────────────────────────┘
```

PC 端通过 `hdc` 工具与设备建立 USB 通信，使用 `hdc fport` 进行端口转发，将设备上的两个 TCP 端口（视频/控制）映射到本地。客户端连接本地端口即可与设备端服务通信。

---

## 2. 项目整体架构

### 2.1 分层架构

项目采用经典的 **客户端 / 服务端（C/S）** 分层架构：

```
┌─────────────────────────────────────────────────────────────────┐
│                        客户端 (PC 端)                            │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  应用层 (main.c) - 协调各模块、主循环、事件分发            │   │
│  ├──────────────────────────────────────────────────────────┤   │
│  │  设备管理 (hdc_manager)  │  网络通信 (tcp_client)         │   │
│  │  视频解码 (video_decoder)│  渲染显示 (renderer)           │   │
│  │  输入处理 (input_handler)                                 │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
                              │
                              │ TCP (经 hdc fport 转发)
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                      服务端 (OpenHarmony 设备)                   │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  应用层 (main.c) - 启动流程、回调编排                      │   │
│  ├──────────────────────────────────────────────────────────┤   │
│  │  屏幕捕获 (screen_capture) │ 视频编码 (video_encoder)     │   │
│  │  输入注入 (input_injector) │ 网络服务 (tcp_server)        │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 双通道设计

服务端与客户端之间使用 **两个独立的 TCP 通道**：

| 通道 | 默认端口 | 方向 | 用途 |
|------|---------|------|------|
| 视频通道 | 9901 | 服务端 → 客户端 | 传输视频帧（H.264 / RGBA） |
| 控制通道 | 9902 | 双向 | 客户端 → 服务端的输入事件；服务端 → 客户端的握手响应、心跳 |

控制通道同时承载握手协议，连接建立后用于传输输入事件和心跳。

---

## 3. 目录结构

```
ohos-scrcpy/
├── common/                          # 公共模块（客户端/服务端共享）
│   ├── log.h                        # 日志工具（宏定义）
│   └── protocol.h                   # 通信协议定义（消息类型、结构体）
│
├── client/                          # PC 端客户端
│   ├── CMakeLists.txt               # CMake 构建配置
│   ├── include/                     # 头文件
│   │   ├── hdc_manager.h            # HDC 设备管理接口
│   │   ├── input_handler.h          # 输入事件处理接口
│   │   ├── renderer.h               # SDL/OpenGL 渲染接口
│   │   ├── tcp_client.h             # TCP 客户端接口
│   │   └── video_decoder.h          # FFmpeg 视频解码接口
│   └── src/                         # 实现文件
│       ├── main.c                   # 客户端主入口
│       ├── hdc_manager.c            # hdc 命令封装
│       ├── tcp_client.c             # TCP 连接管理
│       ├── renderer.c               # OpenGL 渲染实现
│       ├── input_handler.c          # 输入事件转换
│       └── video_decoder.cpp        # FFmpeg H.264 解码（C++）
│
├── server/                          # OpenHarmony 设备端服务
│   ├── BUILD.gn                     # OpenHarmony GN 构建配置
│   ├── CMakeLists.txt               # 桩编译 CMake 配置
│   ├── build_arm64.sh               # aarch64 交叉编译脚本
│   ├── build_server.sh              # OpenHarmony 源码树编译脚本
│   ├── ohos_scrcpy_server.cfg       # 服务端权限配置（init 服务）
│   ├── include/                     # 头文件
│   │   ├── input_injector.h         # 输入注入接口
│   │   ├── screen_capture.h         # 屏幕捕获接口
│   │   ├── tcp_server.h             # TCP 服务端接口
│   │   └── video_encoder.h          # 视频编码接口
│   ├── src/                         # 实现文件
│   │   ├── main.c                   # 服务端主入口
│   │   ├── screen_capture.c         # AVScreenCapture 实现
│   │   ├── video_encoder.c          # OH_VideoEncoder 实现
│   │   ├── input_injector.c         # OH_Input 动态加载实现
│   │   └── tcp_server.c             # TCP 监听实现
│   └── stubs/                       # 桩头文件（用于无 OHOS SDK 时的结构验证编译）
│       ├── input_injector.h
│       ├── screen_capture.h
│       └── video_encoder.h
│
└── scripts/                         # 辅助脚本
    ├── build_client.sh              # 客户端构建脚本
    ├── quick_start.sh               # 一键启动脚本（Linux/macOS）
    └── start.bat                    # Windows 启动脚本
```

---

## 4. 通信协议规范

协议定义在 [common/protocol.h](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/common/protocol.h) 中，客户端与服务端共享。

### 4.1 协议常量

| 常量 | 值 | 说明 |
|------|----|----|
| `PROTOCOL_VERSION` | 1 | 协议版本号 |
| `DEFAULT_SERVER_PORT` | 9900 | 默认服务端口（未使用） |
| `DEFAULT_VIDEO_PORT` | 9901 | 视频通道端口 |
| `DEFAULT_CONTROL_PORT` | 9902 | 控制通道端口 |
| `MAX_FRAME_SIZE` | 4 MB | 单帧最大尺寸 |
| `HANDSHAKE_MAGIC` | 0x4F534352 ("OSCR") | 握手魔数 |

### 4.2 视频通道消息

视频通道消息由 `VideoFrameHeader` 头部 + 数据负载组成：

```c
typedef struct __attribute__((packed)) {
    uint8_t  type;           /* VideoMessageType */
    uint32_t length;         /* 数据长度(不含头部) */
    uint64_t timestamp;      /* 时间戳(微秒) */
} VideoFrameHeader;
```

消息类型（`VideoMessageType`）：

| 类型 | 值 | 说明 |
|------|----|----|
| `MSG_VIDEO_SPS_PPS` | 0x01 | SPS/PPS 参数集（H.264 解码必需） |
| `MSG_VIDEO_IDR_FRAME` | 0x02 | IDR 关键帧 |
| `MSG_VIDEO_P_FRAME` | 0x03 | P 帧 |
| `MSG_VIDEO_CONFIG` | 0x04 | 视频配置信息（宽高、帧率、码率） |
| `MSG_VIDEO_RAW_RGBA` | 0x05 | 原始 RGBA 帧（降级模式） |

`MSG_VIDEO_RAW_RGBA` 的负载以 `RawFrameHeader`（width, height）开头，后跟 `width * height * 4` 字节的 RGBA 像素数据。

### 4.3 控制通道消息

控制通道消息由 `ControlHeader` 头部 + 数据负载组成：

```c
typedef struct __attribute__((packed)) {
    uint8_t  type;           /* ControlMessageType */
    uint8_t  reserved;       /* 保留对齐 */
    uint16_t length;         /* 数据长度(不含头部) */
} ControlHeader;
```

消息类型（`ControlMessageType`）：

| 类型 | 值 | 负载 | 说明 |
|------|----|------|------|
| `MSG_CTRL_TOUCH` | 0x10 | `TouchEvent` | 触摸事件 |
| `MSG_CTRL_KEY` | 0x20 | `KeyEvent` | 按键事件 |
| `MSG_CTRL_SCROLL` | 0x30 | `ScrollEvent` | 滚动事件 |
| `MSG_CTRL_BACK` | 0x40 | 无 | 返回键 |
| `MSG_CTRL_HOME` | 0x41 | 无 | 主页键 |
| `MSG_CTRL_POWER` | 0x42 | 无 | 电源键 |
| `MSG_CTRL_VOLUME_UP` | 0x43 | 无 | 音量+ |
| `MSG_CTRL_VOLUME_DOWN` | 0x44 | 无 | 音量- |
| `MSG_CTRL_SET_SCREEN_SIZE` | 0x50 | - | 设置屏幕尺寸 |
| `MSG_CTRL_HEARTBEAT` | 0xFF | 无 | 心跳（服务端原样回复） |

### 4.4 事件结构体

```c
/* 触摸事件 */
typedef struct __attribute__((packed)) {
    uint8_t  action;         /* TouchAction: DOWN=0, UP=1, MOVE=2 */
    uint8_t  pointerId;      /* 触摸点 ID */
    uint16_t reserved;
    uint32_t x;              /* X 坐标(设备像素) */
    uint32_t y;              /* Y 坐标(设备像素) */
    uint64_t timestamp;      /* 时间戳(微秒) */
} TouchEvent;

/* 按键事件 */
typedef struct __attribute__((packed)) {
    uint8_t  action;         /* KeyAction: DOWN=0, UP=1 */
    uint8_t  reserved;
    uint16_t keycode;        /* 键码 */
} KeyEvent;

/* 滚动事件 */
typedef struct __attribute__((packed)) {
    uint32_t x;
    uint32_t y;
    int32_t  dx;             /* 水平滚动量 */
    int32_t  dy;             /* 垂直滚动量 */
} ScrollEvent;
```

### 4.5 握手协议

握手在控制通道上完成，客户端连接后首先发送 `HandshakeRequest`，服务端回复 `HandshakeResponse`：

```c
typedef struct __attribute__((packed)) {
    uint32_t magic;          /* HANDSHAKE_MAGIC */
    uint32_t version;        /* 协议版本 */
    uint16_t videoPort;      /* 期望的视频端口 */
    uint16_t controlPort;    /* 期望的控制端口 */
} HandshakeRequest;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    int32_t  result;         /* 0=成功, 负数=错误码 */
    uint16_t videoPort;      /* 实际视频端口 */
    uint16_t controlPort;    /* 实际控制端口 */
    uint16_t screenWidth;    /* 设备屏幕宽度 */
    uint16_t screenHeight;   /* 设备屏幕高度 */
} HandshakeResponse;
```

握手结果码：`HANDSHAKE_OK=0`、`HANDSHAKE_VERSION_MISMATCH=-1`、`HANDSHAKE_PORT_IN_USE=-2`、`HANDSHAKE_CAPTURE_FAILED=-3`、`HANDSHAKE_ENCODER_FAILED=-4`。

---

## 5. 客户端模块详解

客户端运行在 PC 上，使用 C/C++ 实现，跨平台支持 Windows / Linux / macOS。

### 5.1 主入口 `main.c`

**文件**: [client/src/main.c](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/client/src/main.c)

#### 职责
- 解析命令行参数
- 协调各子模块的初始化与销毁
- 主循环：SDL 事件分发、RGBA 帧渲染、FPS 统计
- 信号处理（SIGINT/SIGTERM 优雅退出）

#### 关键函数

| 函数 | 说明 |
|------|------|
| `parse_args()` | 解析 `-s`、`--scale`、`--bitrate`、`--fps`、`--fullscreen`、`--always-on-top`、`--borderless`、`--video-port`、`--control-port`、`--hdc` 等参数 |
| `on_video_frame()` | 视频帧回调。根据 `VideoMessageType` 分发：SPS/PPS → 解码器配置；IDR/P → 解码；CONFIG → 更新渲染器/输入器尺寸；RAW_RGBA → 拷贝到缓冲区由主线程渲染 |
| `on_decoded_frame()` | 解码帧回调，将 YUV420P 数据送入渲染器 |
| `on_control_message()` | 控制消息回调（当前为空实现） |
| `heartbeat_thread()` | 心跳线程，每 3 秒发送一次心跳 |
| `signal_handler()` | 信号处理，设置 `g_running = false` |
| `main()` | 主流程：HDC 初始化 → 查找设备 → 端口转发 → 启动服务端 → TCP 连接 → 握手 → 创建解码器/渲染器/输入处理器 → 启动接收线程 → 主循环 |

#### 关键全局状态

```c
static AppConfig g_appConfig;          /* 应用配置 */
static volatile bool g_running;        /* 运行标志 */
static bool g_rawRgbaMode;             /* 是否处于 RGBA 降级模式 */
static uint8_t *g_rgbaBuffer;          /* RGBA 帧缓冲区 */
static pthread_mutex_t g_rgbaMutex;    /* RGBA 缓冲区互斥锁 */
```

#### 主流程

1. 初始化 HDC 管理器
2. 查找已连接设备（自动选择或匹配 `-s` 指定序列号）
3. 设置端口转发（视频端口 + 控制端口）
4. 推送并启动设备端服务
5. 创建 TCP 客户端，重试连接（最多 10 次，每次间隔 500ms）
6. 执行握手，获取设备屏幕尺寸
7. 创建视频解码器（失败不退出，可能使用 RGBA 模式）
8. 创建 SDL/OpenGL 渲染器
9. 创建输入处理器
10. 启动视频/控制接收线程和心跳线程
11. 主循环：处理 SDL 事件 → 渲染 RGBA 帧（如适用）→ 更新窗口标题 FPS

### 5.2 HDC 管理模块 `hdc_manager`

**文件**: [client/include/hdc_manager.h](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/client/include/hdc_manager.h), [client/src/hdc_manager.c](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/client/src/hdc_manager.c)

#### 职责
封装 `hdc` 命令行工具，管理设备连接、文件推送、服务端部署、端口转发。

#### 关键函数

| 函数 | 说明 |
|------|------|
| `hdc_manager_init()` | 初始化，验证 hdc 可用性 |
| `hdc_manager_get_devices()` | 执行 `hdc list targets` 获取设备列表 |
| `hdc_manager_is_device_connected()` | 检查指定序列号设备是否连接 |
| `hdc_manager_push_file()` | 执行 `hdc file send` 推送文件 |
| `hdc_manager_shell()` | 执行 `hdc shell` 命令 |
| `hdc_manager_start_server()` | 推送服务端二进制到 `/data/local/tmp/ohos_scrcpy_server`，赋可执行权限，`nohup` 后台启动 |
| `hdc_manager_stop_server()` | 执行 `pkill -f ohos_scrcpy_server` 停止服务 |
| `hdc_manager_forward_port()` | 执行 `hdc fport tcp:LOCAL tcp:REMOTE` 设置端口转发（失败时回退 `fport add`） |
| `hdc_manager_remove_forward()` | 执行 `hdc fport rm tcp:PORT` 移除转发 |

#### 跨平台实现

`execute_hdc_command()` 内部针对 Windows 和 Unix 不同实现：
- **Windows**: 使用 `CreateProcessA` + `CreatePipe` 捕获输出
- **Unix**: 使用 `popen()`

服务端二进制路径通过 `GetModuleFileNameA` (Windows) 或 `readlink("/proc/self/exe")` (Linux) 自动定位。

### 5.3 TCP 客户端模块 `tcp_client`

**文件**: [client/include/tcp_client.h](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/client/include/tcp_client.h), [client/src/tcp_client.c](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/client/src/tcp_client.c)

#### 职责
管理与设备端的两个 TCP 连接（视频/控制），提供帧收发和事件发送接口。

#### 客户端状态机

```
CLIENT_STATE_DISCONNECTED → CLIENT_STATE_CONNECTING → CLIENT_STATE_CONNECTED
                                                       ↓
                                                CLIENT_STATE_ERROR
```

#### 关键函数

| 函数 | 说明 |
|------|------|
| `tcp_client_create()` | 创建客户端上下文，Windows 下初始化 Winsock |
| `tcp_client_connect()` | 连接视频端口和控制端口，启用 `TCP_NODELAY` |
| `tcp_client_handshake()` | 发送 `HandshakeRequest`，接收 `HandshakeResponse` 并验证 |
| `tcp_client_start_video_receiver()` | 启动视频接收线程 |
| `tcp_client_start_control_receiver()` | 启动控制接收线程 |
| `tcp_client_send_touch()` | 发送触摸事件（ControlHeader + TouchEvent） |
| `tcp_client_send_key()` | 发送按键事件 |
| `tcp_client_send_scroll()` | 发送滚动事件 |
| `tcp_client_send_system_key()` | 发送系统按键（BACK/HOME/POWER 等，仅 ControlHeader） |
| `tcp_client_send_heartbeat()` | 发送心跳 |
| `tcp_client_disconnect()` | 关闭 socket，join 接收线程 |

#### 接收缓冲区

视频接收缓冲区大小为 `RECV_BUF_SIZE = 4MB`，足以容纳一帧原始 RGBA 数据（如 1080×1920×4 ≈ 8MB，实际经过 scale 缩小后通常 < 4MB）。

### 5.4 视频解码模块 `video_decoder`

**文件**: [client/include/video_decoder.h](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/client/include/video_decoder.h), [client/src/video_decoder.cpp](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/client/src/video_decoder.cpp)

> 唯一的 C++ 文件，因为 FFmpeg API 是 C++ 友好的。

#### 职责
使用 FFmpeg 解码 H.264 视频流，优先使用硬件加速解码器。

#### 解码器选择策略

`find_best_decoder()` 按以下顺序尝试：

1. **Linux**: `h264_vaapi` (VA-API) → `h264_vdpau` (VDPAU)
2. **Windows**: `h264_dxva2` (DXVA2) → `h264_d3d11va` (D3D11VA)
3. **macOS**: `h264_videotoolbox` (VideoToolbox)
4. **回退**: 软件解码 `avcodec_find_decoder(AV_CODEC_ID_H264)`

#### 关键函数

| 函数 | 说明 |
|------|------|
| `video_decoder_create()` | 查找解码器，分配 `AVCodecContext`、`AVFrame`、`AVPacket`，配置低延迟参数 |
| `video_decoder_set_sps_pps()` | 接收 SPS/PPS，构建 Annex-B 格式 extradata（`00 00 00 01` + SPS + `00 00 00 01` + PPS），打开解码器 |
| `video_decoder_decode()` | 发送数据包到解码器，循环接收解码后的帧；硬件解码时通过 `av_hwframe_transfer_data` 转到系统内存 |
| `video_decoder_flush()` | 刷新解码器缓冲区 |
| `video_decoder_destroy()` | 释放所有 FFmpeg 资源 |

#### 低延迟配置

```cpp
codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
av_opt_set(priv_data, "tune", "zerolatency", 0);
av_opt_set(priv_data, "threads", "4", 0);
```

### 5.5 渲染模块 `renderer`

**文件**: [client/include/renderer.h](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/client/include/renderer.h), [client/src/renderer.c](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/client/src/renderer.c)

#### 职责
使用 SDL2 创建窗口，使用 OpenGL 3.3 Core 渲染 YUV420P 或 RGBA 帧。

#### 着色器

**YUV→RGB 转换着色器**（用于 H.264 解码后的 YUV420P 帧）：

```glsl
/* 片段着色器 */
float y = texture(texY, TexCoord).r;
float u = texture(texU, TexCoord).r - 0.5;
float v = texture(texV, TexCoord).r - 0.5;
float r = y + 1.402 * v;
float g = y - 0.344136 * u - 0.714136 * v;
float b = y + 1.772 * u;
FragColor = vec4(r, g, b, 1.0);
```

**RGBA 着色器**（用于降级模式的原始 RGBA 帧）：直接采样纹理。

#### 关键函数

| 函数 | 说明 |
|------|------|
| `renderer_create()` | 初始化 SDL，创建窗口和 OpenGL 上下文，编译着色器，创建 VAO/VBO 和纹理 |
| `renderer_update_video_size()` | 更新视频尺寸，根据宽高比调整窗口大小 |
| `renderer_render_frame()` | 渲染 YUV420P 帧（3 个纹理） |
| `renderer_render_rgba_frame()` | 渲染 RGBA 帧（1 个纹理），首次调用时切换到 RGBA 模式 |
| `renderer_handle_event()` | 处理 SDL 事件（窗口大小、F11 全屏、ESC 退出全屏） |
| `renderer_toggle_fullscreen()` | 切换全屏/窗口模式 |
| `renderer_get_stats()` | 获取 FPS、帧数、丢帧、延迟统计 |
| `renderer_destroy()` | 释放 OpenGL 资源，销毁窗口，关闭 SDL |

#### 宽高比保持

`update_viewport()` 根据窗口和视频的宽高比，计算居中渲染区域，使用 `glViewport` 设置视口，保持画面不变形。

#### VSync

`SDL_GL_SetSwapInterval(0)` 禁用 VSync，以降低延迟（由主循环的 `usleep(1000)` 控制刷新率）。

### 5.6 输入处理模块 `input_handler`

**文件**: [client/include/input_handler.h](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/client/include/input_handler.h), [client/src/input_handler.c](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/client/src/input_handler.c)

#### 职责
捕获 SDL 鼠标/键盘事件，转换为协议消息发送到设备。

#### 坐标转换

`window_to_screen_coord()` 将窗口坐标转换为设备屏幕坐标：
1. 计算窗口中保持宽高比的渲染区域（与渲染器的 `update_viewport` 逻辑一致）
2. 计算窗口坐标在渲染区域内的相对位置 `[0, 1]`
3. 映射到设备屏幕像素坐标

#### 快捷键映射

| 快捷键 | 动作 |
|--------|------|
| Alt+B | 返回键 |
| Alt+H | 主页键 |
| Alt+P | 电源键 |
| Alt+↑ | 音量+ |
| Alt+↓ | 音量- |
| F11 | 切换全屏（由渲染器处理） |
| ESC | 退出全屏（由渲染器处理） |

#### 关键函数

| 函数 | 说明 |
|------|------|
| `input_handler_create()` | 初始化，配置屏幕尺寸和捕获开关 |
| `input_handler_update_screen_size()` | 更新设备屏幕尺寸（旋转时） |
| `input_handler_update_window_size()` | 更新窗口尺寸 |
| `input_handler_process_mouse_event()` | 处理鼠标事件：滚轮 → `ScrollEvent`；按下/抬起/移动 → `TouchEvent` |
| `input_handler_process_key_event()` | 处理键盘事件：先检查快捷键，否则发送 `KeyEvent` |
| `input_handler_get_shortcut()` | 判断是否为系统快捷键，返回对应 `ControlMessageType` |

---

## 6. 服务端模块详解

服务端运行在 OpenHarmony 设备上，使用 OpenHarmony NDK API 实现。

### 6.1 主入口 `main.c`

**文件**: [server/src/main.c](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/server/src/main.c)

#### 职责
- 解析命令行参数（`getopt_long`）
- 协调各子模块
- 注册回调链：屏幕帧 → 编码器 → TCP 发送；TCP 接收 → 输入注入

#### 启动流程

1. 初始化输入注入模块（动态加载 `libohinput.so`）
2. 创建屏幕捕获器（获取屏幕尺寸，计算缩放后尺寸）
3. 创建 TCP 服务端，设置控制消息回调
4. **启动 TCP 服务端（阻塞等待客户端连接）**
5. 客户端连接后，创建视频编码器（失败则降级到 RGBA 模式）
6. 设置编码器回调（`on_sps_pps`、`on_encoded_frame`）
7. 启动编码器
8. 发送视频配置信息（`VideoConfig`）
9. 启动屏幕捕获（`on_screen_frame` 回调）
10. 主循环：每 100ms 检查运行状态

#### 回调链

```
屏幕帧 (on_screen_frame)
    ├── 编码器可用 → video_encoder_encode_frame()
    │                    ↓ on_encoded_frame
    │                    ↓ tcp_server_send_video_frame(IDR/P_FRAME)
    │
    └── 编码器不可用 → 构造 RawFrameHeader + RGBA 数据
                         ↓ tcp_server_send_video_frame(RAW_RGBA)

控制消息 (on_control_message)
    ├── MSG_CTRL_TOUCH  → input_injector_inject_touch()
    ├── MSG_CTRL_KEY    → input_injector_inject_key()
    ├── MSG_CTRL_SCROLL → input_injector_inject_scroll()
    ├── MSG_CTRL_BACK   → input_injector_inject_back_key()
    ├── MSG_CTRL_HOME   → input_injector_inject_home_key()
    ├── MSG_CTRL_POWER  → input_injector_inject_power_key()
    ├── MSG_CTRL_VOLUME_UP   → input_injector_inject_volume_up_key()
    └── MSG_CTRL_VOLUME_DOWN → input_injector_inject_volume_down_key()
```

### 6.2 屏幕捕获模块 `screen_capture`

**文件**: [server/include/screen_capture.h](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/server/include/screen_capture.h), [server/src/screen_capture.c](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/server/src/screen_capture.c)

#### 职责
使用 OpenHarmony `AVScreenCapture` C-API 捕获屏幕 RGBA 帧。

#### 关键函数

| 函数 | 说明 |
|------|------|
| `screen_capture_create()` | 获取屏幕尺寸，根据 `scale` 计算实际捕获尺寸（宽高对齐到偶数） |
| `screen_capture_get_display_size()` | 获取屏幕尺寸：优先 `libnative_display_manager.so`，回退 `/sys/class/display/...`，最后默认 1080×1920 |
| `screen_capture_start()` | 创建 `OH_AVScreenCapture`，配置 `OH_CAPTURE_HOME_SCREEN` + `OH_ORIGINAL_STREAM` + `OH_VIDEO_SOURCE_SURFACE_RGBA`，注册回调，启动捕获 |
| `screen_capture_stop()` | 停止捕获，释放资源 |
| `on_video_buffer_available()` | 视频帧回调：`AcquireVideoBuffer` → `OH_NativeBuffer_Map` 获取像素地址 → 回调上层 → `Unmap` + `ReleaseVideoBuffer` |

#### 配置要点

```c
captureConfig.captureMode = OH_CAPTURE_HOME_SCREEN;
captureConfig.dataType = OH_ORIGINAL_STREAM;
captureConfig.videoInfo.videoCapInfo.videoSource = OH_VIDEO_SOURCE_SURFACE_RGBA;
OH_AVScreenCapture_SetMicrophoneEnabled(capture, false);  /* 关闭麦克风 */
```

只配置 `micCapInfo`，不配置 `innerCapInfo`，避免 "set audioSampleRate is not support" 错误。

### 6.3 视频编码模块 `video_encoder`

**文件**: [server/include/video_encoder.h](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/server/include/video_encoder.h), [server/src/video_encoder.c](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/server/src/video_encoder.c)

#### 职责
使用 OpenHarmony `OH_VideoEncoder` 进行 H.264 硬件编码。

#### 编码器查找

`find_h264_encoder_name()` 按顺序：
1. `OH_AVCodec_GetCapability("video/avc", true)` 查询
2. `OH_AVCodec_GetCapabilityByCategory("video/avc", true, HARDWARE)` 查询硬件编码器
3. 回退到名称 `"video_encoder.avc"`

#### 异步回调

| 回调 | 说明 |
|------|------|
| `on_error()` | 错误处理 |
| `on_stream_changed()` | 流信息变化时获取 SPS/PPS，触发 `spsPpsCallback` |
| `on_need_input_data()` | 编码器请求输入数据，记录 `inputIndex` 和 `inputData` |
| `on_need_output_data()` | 编码器输出数据，构造 `EncodedFrame` 触发 `frameCallback`，释放输出 buffer |

#### 关键函数

| 函数 | 说明 |
|------|------|
| `video_encoder_create()` | 查找编码器名称，`CreateByName` 或 `CreateByMime`，应用默认配置（码率 4Mbps、60fps、GOP 60、High profile） |
| `video_encoder_start()` | 配置 `OH_AVFormat`（宽高、像素格式 RGBA、码率、帧率、GOP、profile、低延迟），设置异步回调，`Prepare` + `Start` |
| `video_encoder_encode_frame()` | 等待 `inputReady`，逐行拷贝帧数据到输入 buffer（处理 stride 差异），`PushInputData` |
| `video_encoder_request_key_frame()` | 通过 `OH_MD_KEY_REQUEST_I_FRAME` 请求关键帧 |
| `video_encoder_set_bitrate()` | 动态调整码率 |
| `video_encoder_stop()` / `video_encoder_destroy()` | 停止并释放资源 |

### 6.4 输入注入模块 `input_injector`

**文件**: [server/include/input_injector.h](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/server/include/input_injector.h), [server/src/input_injector.c](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/server/src/input_injector.c)

#### 职责
使用 OpenHarmony `OH_Input` API 注入触摸和按键事件。

#### 动态加载

通过 `dlopen("libohinput.so", RTLD_LAZY)` 动态加载输入 API，避免编译时硬依赖。加载的符号包括：

- `OH_Input_CreateKeyEvent` / `OH_Input_DestroyKeyEvent`
- `OH_Input_SetKeyEventAction` / `OH_Input_SetKeyEventKeyCode` / `OH_Input_SetKeyEventActionTime`
- `OH_Input_InjectKeyEvent`
- `OH_Input_CreateTouchEvent` / `OH_Input_DestroyTouchEvent`
- `OH_Input_SetTouchEventAction` / `OH_Input_SetTouchEventFingerId` / `OH_Input_SetTouchEventDisplayX` / `OH_Input_SetTouchEventDisplayY` / `OH_Input_SetTouchEventActionTime`
- `OH_Input_InjectTouchEvent`

#### 动作码映射

| 协议动作 | NDK 动作 |
|---------|---------|
| `PROTO_TOUCH_DOWN` (0) | `NDK_TOUCH_DOWN` (1) |
| `PROTO_TOUCH_UP` (1) | `NDK_TOUCH_UP` (3) |
| `PROTO_TOUCH_MOVE` (2) | `NDK_TOUCH_MOVE` (2) |
| `PROTO_KEY_DOWN` (0) | `NDK_KEY_DOWN` (1) |
| `PROTO_KEY_UP` (1) | `NDK_KEY_UP` (2) |

#### 滚动事件模拟

`input_injector_inject_scroll()` 通过模拟 3 个触摸事件实现滚动：
1. `TOUCH_DOWN` 在起始位置
2. `TOUCH_MOVE` 到结束位置（`startY - dy * 10`）
3. `TOUCH_UP` 在结束位置

#### 系统按键码

| 按键 | Keycode |
|------|---------|
| HOME | 1 |
| BACK | 2 |
| VOLUME_UP | 16 |
| VOLUME_DOWN | 17 |
| POWER | 18 |

### 6.5 TCP 服务端模块 `tcp_server`

**文件**: [server/include/tcp_server.h](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/server/include/tcp_server.h), [server/src/tcp_server.c](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/server/src/tcp_server.c)

#### 职责
监听视频和控制两个端口，等待客户端连接，处理握手，转发视频帧，接收控制消息。

#### 关键函数

| 函数 | 说明 |
|------|------|
| `tcp_server_create()` | 初始化上下文 |
| `tcp_server_start()` | 创建两个监听 socket，**阻塞等待**视频/控制通道连接，处理握手，启动控制接收线程 |
| `tcp_server_send_video_frame()` | 发送 `VideoFrameHeader` + 数据 |
| `tcp_server_send_video_config()` | 发送 `VideoConfig`（封装为 `MSG_VIDEO_CONFIG` 帧） |
| `tcp_server_send_sps_pps()` | 将 SPS/PPS 合并为一帧（4 字节大端长度前缀 + 数据），封装为 `MSG_VIDEO_SPS_PPS` 发送 |
| `tcp_server_set_control_callback()` | 设置控制消息回调 |
| `control_thread_func()` | 控制通道接收线程：读取 `ControlHeader` + 数据，心跳原样回复，其他消息触发回调 |

#### 服务端状态机

```
SERVER_STATE_IDLE → SERVER_STATE_LISTENING → SERVER_STATE_CONNECTED
                                                ↓
                                         SERVER_STATE_ERROR
```

---

## 7. 公共模块

### 7.1 日志 `log.h`

**文件**: [common/log.h](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/common/log.h)

提供统一的日志输出宏，输出到 `stderr`，格式：`[HH:MM:SS][LEVEL][TAG] message`。

| 宏 | 级别 |
|----|------|
| `LOGD(fmt, ...)` | DEBUG |
| `LOGI(fmt, ...)` | INFO |
| `LOGW(fmt, ...)` | WARN |
| `LOGE(fmt, ...)` | ERROR |
| `LOG_TAG_X(tag, fmt, ...)` | 带自定义 tag 的日志 |

默认 tag 为 `"OHScrcpy"`。

### 7.2 协议 `protocol.h`

**文件**: [common/protocol.h](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/common/protocol.h)

定义客户端与服务端共享的所有协议常量、枚举和结构体（详见 [第 4 节](#4-通信协议规范)）。所有结构体使用 `__attribute__((packed))` 保证二进制布局一致。

---

## 8. 依赖关系

### 8.1 客户端依赖

| 依赖 | 用途 | 必需 |
|------|------|------|
| **SDL2** | 窗口管理、事件循环、OpenGL 上下文 | 是 |
| **FFmpeg** (libavcodec, libavutil, libswscale) | H.264 视频解码 | 是 |
| **GLEW** | OpenGL 扩展加载 | 是 |
| **OpenGL 3.3 Core** | GPU 渲染 | 是 |
| **pthread** | 多线程 | 是 |
| **Winsock (ws2_32)** | Windows 网络栈 | Windows |
| **hdc** | 设备通信（运行时） | 是 |

### 8.2 服务端依赖

| 依赖 | 用途 | 必需 |
|------|------|------|
| **OH_AVScreenCapture** (`libnative_avscreen_capture.so`) | 屏幕捕获 | 是 |
| **OH_VideoEncoder** (`libnative_media_venc.so`) | H.264 编码 | 否（降级到 RGBA） |
| **OH_Input** (`libohinput.so`) | 输入注入 | 否（动态加载） |
| **OH_NativeDisplayManager** (`libnative_display_manager.so`) | 屏幕尺寸查询 | 否（回退到 sysfs） |
| **OH_NativeBuffer** | 缓冲区映射 | 是 |
| **pthread** | 多线程 | 是 |
| **dl** (dlopen/dlsym) | 动态库加载 | 是 |

### 8.3 模块间依赖图

```
客户端:
  main.c ─┬─ hdc_manager ──── hdc (外部命令)
          ├─ tcp_client ──── Winsock/POSIX socket
          ├─ video_decoder ─ FFmpeg
          ├─ renderer ────── SDL2 + OpenGL + GLEW
          └─ input_handler ─ tcp_client (发送事件)

服务端:
  main.c ─┬─ screen_capture ─ OH_AVScreenCapture + OH_NativeBuffer
          ├─ video_encoder ── OH_VideoEncoder
          ├─ input_injector ─ OH_Input (dlopen)
          └─ tcp_server ───── POSIX socket + pthread
```

---

## 9. 构建与运行

### 9.1 客户端构建

#### 依赖安装

**Windows (MSYS2/MinGW)**:
```bash
pacman -S mingw-w64-x86_64-SDL2 mingw-w64-x86_64-ffmpeg mingw-w64-x86_64-glew cmake
```

**Linux (Ubuntu/Debian)**:
```bash
sudo apt install libsdl2-dev libffmpeg-dev libglew-dev cmake pkg-config
```

**macOS (Homebrew)**:
```bash
brew install sdl2 ffmpeg glew cmake pkg-config
```

#### 构建命令

```bash
# 使用脚本
bash scripts/build_client.sh

# 或手动
cd client && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release -j$(nproc)
```

构建产物：`client/build/ohos_scrcpy` (Linux/macOS) 或 `client/build/Release/ohos_scrcpy.exe` (Windows)。

### 9.2 服务端构建

服务端有三种构建方式：

#### 方式 1: 交叉编译（推荐，使用 DevEco Studio NDK）

编辑 [server/build_arm64.sh](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/server/build_arm64.sh) 中的 `OHOS_NATIVE` 路径，指向你的 DevEco Studio OpenHarmony NDK：

```bash
OHOS_NATIVE="D:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native"
bash server/build_arm64.sh
```

产物：`server/build_arm64/ohos_scrcpy_server`

#### 方式 2: OpenHarmony 源码树编译

```bash
bash server/build_server.sh /path/to/openharmony [product_name]
# 默认 product: rk3568
```

此脚本会将源码复制到 OpenHarmony 源码树的 `foundation/multimedia/player_framework/OHScrcpy_Server/` 目录，patch `bundle.json`，然后调用 `./build.sh`。

#### 方式 3: 桩编译（结构验证）

在无 OpenHarmony SDK 的环境下，使用 [server/stubs/](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/server/stubs) 下的桩头文件验证代码结构：

```bash
cd server && mkdir build && cd build
cmake ..
make
```

注意：桩编译只编译 `main.c` 和 `tcp_server.c`，跳过依赖 OHOS SDK 的模块。

### 9.3 运行流程

#### 前置条件

1. OpenHarmony 设备已通过 USB 连接
2. 设备已开启开发者模式和 USB 调试
3. `hdc` 工具在 PATH 中（或通过 `--hdc` 指定路径）
4. 服务端二进制 `ohos_scrcpy_server` 与客户端可执行文件在同一目录（客户端会自动推送）

#### 一键启动

**Linux/macOS**:
```bash
bash scripts/quick_start.sh [options]
```

**Windows**:
```bat
scripts\start.bat [options]
```

#### 手动启动

```bash
./ohos_scrcpy [options]
```

#### 命令行参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `-s SERIAL` | 自动选择 | 设备序列号 |
| `--scale SCALE` | 0.5 | 屏幕缩放比例 |
| `--bitrate BPS` | 4000000 | 视频码率 |
| `--fps FPS` | 60 | 目标帧率 |
| `-f, --fullscreen` | false | 全屏启动 |
| `--always-on-top` | false | 窗口置顶 |
| `--borderless` | false | 无边框窗口 |
| `--video-port PORT` | 9901 | 视频端口 |
| `--control-port PORT` | 9902 | 控制端口 |
| `--hdc PATH` | hdc | hdc 可执行文件路径 |
| `-h, --help` | - | 显示帮助 |

#### 服务端权限配置

[server/ohos_scrcpy_server.cfg](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/server/ohos_scrcpy_server.cfg) 定义了服务端作为 init 服务的权限配置，可放置在设备的 `/system/etc/init/` 目录下：

```json
{
    "services" : [{
        "name" : "ohos_scrcpy_server",
        "path" : ["/data/local/tmp/ohos_scrcpy_server"],
        "uid" : "shell",
        "gid" : ["shell", "system", "input"],
        "secon" : "u:r:ohos_scrcpy_server:s0",
        "caps" : ["SYS_PTRACE"]
    }]
}
```

通常通过 `hdc shell` 直接运行即可，无需配置 init 服务。

---

## 10. 关键流程时序

### 10.1 启动时序

```
客户端                              设备端
  │                                    │
  │── hdc list targets ───────────────►│
  │◄── 设备列表 ────────────────────────│
  │                                    │
  │── hdc fport tcp:9901 tcp:9901 ────►│  端口转发
  │── hdc fport tcp:9902 tcp:9902 ────►│
  │                                    │
  │── hdc file send server ───────────►│  推送服务端
  │── hdc shell chmod +x ─────────────►│
  │── hdc shell nohup server & ────────►│  启动服务端
  │                                    │
  │                                    │── 创建 TCP 监听 (9901, 9902)
  │                                    │── 等待客户端连接...
  │                                    │
  │── connect 127.0.0.1:9901 ─────────►│  视频通道连接
  │── connect 127.0.0.1:9902 ─────────►│  控制通道连接
  │                                    │
  │── HandshakeRequest ───────────────►│
  │◄── HandshakeResponse (屏幕尺寸) ────│
  │                                    │
  │                                    │── 创建编码器
  │◄── MSG_VIDEO_CONFIG ───────────────│
  │◄── MSG_VIDEO_SPS_PPS ──────────────│
  │◄── MSG_VIDEO_IDR_FRAME ────────────│
  │◄── MSG_VIDEO_P_FRAME ──────────────│  持续视频流
  │   ...                              │
```

### 10.2 输入事件时序

```
用户操作                  客户端                           设备端
   │                        │                                │
   │── 鼠标点击 ───────────►│                                │
   │                        │── SDL_MOUSEBUTTONDOWN event    │
   │                        │── input_handler_process_mouse  │
   │                        │── 坐标转换 (窗口→屏幕)          │
   │                        │── tcp_client_send_touch        │
   │                        │── ControlHeader + TouchEvent ─►│
   │                        │                                │── control_thread_func
   │                        │                                │── on_control_message
   │                        │                                │── input_injector_inject_touch
   │                        │                                │── OH_Input_InjectTouchEvent
   │                        │                                │
   │                        │                                │── 设备响应触摸
```

### 10.3 心跳时序

```
客户端                              设备端
  │                                    │
  │── (每 3 秒) MSG_CTRL_HEARTBEAT ──►│
  │◄── MSG_CTRL_HEARTBEAT (回复) ──────│
```

### 10.4 降级模式时序

当编码器不可用时（创建或启动失败）：

```
设备端                               客户端
  │                                     │
  │── video_encoder_create 失败 ────────│
  │── g_encoderAvailable = false        │
  │                                     │
  │── on_screen_frame (RGBA 帧) ───────►│
  │   └── MSG_VIDEO_RAW_RGBA ──────────►│
  │                                     │── on_video_frame
  │                                     │── g_rawRgbaMode = true
  │                                     │── 拷贝到 g_rgbaBuffer
  │                                     │
  │                                     │── 主循环渲染
  │                                     │── renderer_render_rgba_frame
```

---

## 11. 设计要点与扩展

### 11.1 设计亮点

1. **双通道分离**：视频和控制独立通道，避免大块视频数据阻塞控制指令。
2. **降级机制**：编码器不可用时自动降级到 RGBA 直传，保证基本可用性。
3. **动态加载**：服务端 `libohinput.so` 通过 `dlopen` 加载，编译时无需硬依赖。
4. **跨平台**：客户端通过宏隔离平台差异（`_WIN32` vs Unix），Windows 使用 `CreateProcess`，Unix 使用 `popen`。
5. **低延迟优化**：`TCP_NODELAY`、禁用 VSync、FFmpeg `zerolatency` tune、编码器 `low_latency` 选项。
6. **宽高比保持**：渲染器和输入处理器独立计算视口，保证画面不变形且坐标映射正确。
7. **线程安全**：RGBA 帧通过互斥锁在视频接收线程和主渲染线程间同步，避免 GL 上下文跨线程问题。

### 11.2 线程模型

**客户端线程**:
- 主线程：SDL 事件循环 + RGBA 渲染
- 视频接收线程：接收视频帧，调用解码器或填充 RGBA 缓冲区
- 控制接收线程：接收控制消息（当前为空实现）
- 心跳线程：每 3 秒发送心跳

**服务端线程**:
- 主线程：启动流程 + 主循环（100ms 检查）
- 控制接收线程：接收控制消息，触发输入注入
- AVScreenCapture 回调线程：视频帧可用时回调

### 11.3 可扩展点

| 扩展方向 | 实现思路 |
|---------|---------|
| 音频传输 | 扩展协议增加音频消息类型，服务端启用 `micCapInfo`，客户端增加音频播放 |
| 录屏保存 | 客户端在 `on_video_frame` 中将帧写入文件（FFmpeg muxer） |
| 多设备支持 | 客户端支持同时连接多个设备，每个设备独立 TCP 连接和窗口 |
| 网络穿透 | 替换 `hdc fport` 为直接 TCP 连接，支持远程设备 |
| 视频旋转 | 服务端检测屏幕旋转，发送 `MSG_CTRL_SET_SCREEN_SIZE` 通知客户端 |
| 高效编码 | 引入 H.265/AV1 编码，降低带宽占用 |
| 截图功能 | 实现 `renderer_take_screenshot`（当前为 TODO） |

### 11.4 已知限制

1. `renderer_take_screenshot()` 未实现（TODO）。
2. 客户端 `on_control_message()` 为空实现，未处理服务端下发的消息。
3. RGBA 降级模式带宽占用高（1080p @ 60fps ≈ 475 MB/s），仅适合 USB 连接。
4. 服务端 `tcp_server.c` 使用 POSIX socket，未做 Windows 适配（服务端只运行在设备上）。
5. `MSG_CTRL_SET_SCREEN_SIZE` 消息类型已定义但未使用。

---

## 附录：关键文件索引

| 文件 | 行数 | 说明 |
|------|------|------|
| [common/protocol.h](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/common/protocol.h) | 157 | 通信协议定义 |
| [common/log.h](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/common/log.h) | 57 | 日志宏 |
| [client/src/main.c](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/client/src/main.c) | 487 | 客户端主入口 |
| [client/src/hdc_manager.c](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/client/src/hdc_manager.c) | 342 | HDC 命令封装 |
| [client/src/tcp_client.c](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/client/src/tcp_client.c) | 385 | TCP 客户端 |
| [client/src/renderer.c](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/client/src/renderer.c) | 486 | OpenGL 渲染 |
| [client/src/video_decoder.cpp](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/client/src/video_decoder.cpp) | 271 | FFmpeg 解码 |
| [client/src/input_handler.c](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/client/src/input_handler.c) | 190 | 输入处理 |
| [server/src/main.c](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/server/src/main.c) | 351 | 服务端主入口 |
| [server/src/screen_capture.c](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/server/src/screen_capture.c) | 270 | 屏幕捕获 |
| [server/src/video_encoder.c](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/server/src/video_encoder.c) | 364 | 视频编码 |
| [server/src/input_injector.c](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/server/src/input_injector.c) | 265 | 输入注入 |
| [server/src/tcp_server.c](file:///e/github/OpenHarmony_USB_Screen_Mirroring_Tool/ohos-scrcpy/server/src/tcp_server.c) | 360 | TCP 服务端 |

---

*文档生成时间: 2026-06-21*
*项目版本: 1.0.0*
