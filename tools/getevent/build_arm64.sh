#!/bin/bash
set -e

# 交叉编译 getevent 到 OpenHarmony aarch64
# 依赖 DevEco Studio 自带的 LLVM 工具链

OHOS_NATIVE="D:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native"
CLANG="$OHOS_NATIVE/llvm/bin/clang.exe"
SYSROOT="$OHOS_NATIVE/sysroot"
TARGET="aarch64-linux-ohos"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build_arm64"
mkdir -p "$BUILD_DIR"

COMMON_FLAGS="--target=$TARGET --sysroot=\"$SYSROOT\" -D__MUSL__ -DOHOS_PLATFORM"
INCLUDES="-I\"$SYSROOT/usr/include\""

echo "=== Cross-compiling getevent for aarch64 ==="
echo "Compiler: $CLANG"
echo "Target: $TARGET"
echo ""

eval "\"$CLANG\"" $COMMON_FLAGS $INCLUDES \
  -O2 -Wall -o "$BUILD_DIR/getevent" \
  "$SCRIPT_DIR/getevent.c"

echo ""
echo "=== Build complete! ==="
file "$BUILD_DIR/getevent" 2>/dev/null || ls -lh "$BUILD_DIR/getevent"
echo ""
echo "Push to device and run:"
echo "  hdc file send $BUILD_DIR/getevent /data/local/tmp/getevent"
echo "  hdc shell chmod +x /data/local/tmp/getevent"
echo "  hdc shell /data/local/tmp/getevent"
