# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

OHOS Scrcpy is a USB screen mirroring tool for OpenHarmony devices, inspired by Android's scrcpy. It captures the device screen, encodes it (H.264 or raw RGBA fallback), and streams it to a PC client for display and input control.

## Architecture

Client/Server model with two independent TCP channels:

- **Video channel (port 9901)**: Server → Client. Sends H.264 frames (SPS/PPS, IDR, P-frame) or raw RGBA frames in fallback mode.
- **Control channel (port 9902)**: Bidirectional. Client sends touch/key/scroll/system-key events; server sends handshake responses and heartbeats.

Communication goes over USB via `hdc fport` port forwarding. The protocol is defined in `common/protocol.h` (shared between client and server) — all structs are `__attribute__((packed))`.

**Client (PC)**: C/C++ with SDL2 + OpenGL 3.3 Core + FFmpeg. Decodes H.264 (hardware-accelerated when available) and renders via YUV→RGB shaders. Falls back to direct RGBA texture rendering when the device can't encode.

**Server (OpenHarmony device)**: C with OpenHarmony NDK APIs. Uses `AVScreenCapture` for screen capture, `OH_VideoEncoder` for H.264 encoding, and dynamically loads `libohinput.so` for input injection.

### Key module layout

- `common/` — shared protocol definitions and logging macros
- `client/src/` — PC client: main loop (`main.c`), HDC device management, TCP networking, FFmpeg decoding, OpenGL rendering, SDL input handling
- `server/src/` — device server: main loop (`main.c`), screen capture, video encoding, input injection, TCP server
- `server/stubs/` — stub headers for compiling server code without the full OHOS SDK (structure validation only)

## Build Commands

### Full build (Windows, PowerShell)

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

This builds both client (MinGW x64) and server (OHOS arm64 cross-compile), collects DLL dependencies, and outputs everything to `dist/`.

### Client only (cross-platform)

```bash
cd client
mkdir -p build && cd build
cmake .. -G "MinGW Makefiles"    # Windows with MinGW
cmake --build . -j
```

Dependencies: SDL2, FFmpeg (libavcodec, libavutil, libswscale), GLEW, pthreads. Found via `pkg-config`.

### Server only (OHOS arm64 cross-compile)

```bash
cd server
bash build_arm64.sh
```

Requires OpenHarmony NDK (DevEco Studio SDK) with clang for `aarch64-linux-ohos` target. Links against `libnative_avscreen_capture`, `libnative_media_venc`, `libnative_media_codecbase`, `libnative_media_core`, `libnative_buffer`.

The server also has a stub CMake build (`server/CMakeLists.txt`) for structure validation without the OHOS SDK — it only compiles `main.c` and `tcp_server.c`.

### Quick start (Linux/macOS)

```bash
bash scripts/quick_start.sh --scale 0.5 --bitrate 4000000 --fps 60
```

### Run on Windows

```
dist\ohos_scrcpy_client.exe [--scale 0.5] [--bitrate 4000000] [--fps 60] [--hdc path/to/hdc]
```

Or use `scripts/start.bat`.

## Key Design Decisions

- **Dual-channel separation**: Video and control on separate TCP connections to avoid head-of-line blocking.
- **RGBA fallback mode**: When the device's H.264 encoder is unavailable, the server sends raw RGBA frames directly. The client auto-detects this via `MSG_VIDEO_RAW_RGBA` messages and switches its rendering pipeline.
- **Dynamic loading for input injection**: `input_injector.c` loads `libohinput.so` at runtime via `dlopen` to avoid compile-time dependency on the input API.
- **Cross-platform HDC command execution**: `hdc_manager.c` uses `CreateProcessA` on Windows and `popen()` on Unix.
- **Video decoder fallback chain**: Client tries platform-specific HW decoders first (VAAPI/VDPAU on Linux, DXVA2/D3D11VA on Windows, VideoToolbox on macOS), then falls back to FFmpeg software decode.

## Languages and Standards

- C11 for all C code, C++17 for `video_decoder.cpp` (the only C++ file — uses FFmpeg C API)
- PowerShell for the main Windows build script, Bash for Linux/macOS scripts and server cross-compilation
