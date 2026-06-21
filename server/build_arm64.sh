#!/bin/bash
set -e

OHOS_NATIVE="D:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native"
CLANG="$OHOS_NATIVE/llvm/bin/clang.exe"
SYSROOT="$OHOS_NATIVE/sysroot"
TARGET="aarch64-linux-ohos"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SERVER_DIR="$SCRIPT_DIR"
BUILD_DIR="$SERVER_DIR/build_arm64"
mkdir -p "$BUILD_DIR"

COMMON_FLAGS="--target=$TARGET --sysroot=\"$SYSROOT\" -D__MUSL__ -DOHOS_PLATFORM"

INCLUDES="-I\"$SERVER_DIR/include\" \
  -I\"$SERVER_DIR/../common\" \
  -I\"$SYSROOT/usr/include\""

SRC_FILES="
  $SERVER_DIR/src/main.c
  $SERVER_DIR/src/screen_capture.c
  $SERVER_DIR/src/video_encoder.c
  $SERVER_DIR/src/input_injector.c
  $SERVER_DIR/src/tcp_server.c
"

LIBS="-lnative_avscreen_capture \
  -lnative_media_venc \
  -lnative_media_codecbase \
  -lnative_media_core \
  -lnative_buffer \
  -lpthread"

LIB_DIRS="-L\"$SYSROOT/usr/lib/aarch64-linux-ohos\""

echo "=== Cross-compiling ohos_scrcpy_server for aarch64 ==="
echo "Compiler: $CLANG"
echo "Target: $TARGET"
echo ""

eval "\"$CLANG\"" $COMMON_FLAGS $INCLUDES \
  -O2 -Wall -Wno-unused-parameter -Wno-unused-function \
  -o "$BUILD_DIR/ohos_scrcpy_server" \
  $SRC_FILES \
  $LIB_DIRS $LIBS

echo ""
echo "=== Build complete! ==="
file "$BUILD_DIR/ohos_scrcpy_server" 2>/dev/null || ls -lh "$BUILD_DIR/ohos_scrcpy_server"
