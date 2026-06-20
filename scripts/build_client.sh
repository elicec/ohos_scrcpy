#!/bin/bash
# OHOS Scrcpy - 客户端构建脚本 (Windows 交叉编译或本地编译)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CLIENT_DIR="$PROJECT_DIR/client"
BUILD_DIR="$CLIENT_DIR/build"

echo "=== OHOS Scrcpy Client Build ==="

# 检查依赖
command -v cmake >/dev/null 2>&1 || { echo "Error: cmake not found"; exit 1; }

# 创建构建目录
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# 配置
echo "Configuring..."
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DSDL2_DIR="${SDL2_DIR:-$CONDA_PREFIX/lib/cmake/SDL2}" \
    -DFFMPEG_DIR="${FFMPEG_DIR:-/usr/local}" \
    ${CMAKE_EXTRA_ARGS}

# 编译
echo "Building..."
cmake --build . --config Release -j$(nproc 2>/dev/null || echo 4)

echo ""
echo "Build complete!"
echo "Output: $BUILD_DIR/ohos_scrcpy.exe (or $BUILD_DIR/Release/ohos_scrcpy.exe)"
