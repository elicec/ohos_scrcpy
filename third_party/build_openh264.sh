#!/bin/bash
# Cross-compile OpenH264 static library for OHOS aarch64
set -e

OHOS_NATIVE="D:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native"
# mingw32-make 直接 CreateProcess 执行命令，路径含空格会截断，用无空格 junction
SYSROOT="C:/OHOS_SYSROOT"
TARGET="aarch64-linux-ohos"

export PATH="/c/msys64/usr/bin:/c/msys64/mingw64/bin:/c/OHOS_LLVM/bin:$PATH"

cd "$(dirname "$0")/openh264"

mingw32-make OS=linux ARCH=arm64 BUILDTYPE=Release USE_ASM=No AR=llvm-ar.exe \
  CC="clang --target=$TARGET --sysroot=$SYSROOT -D__MUSL__ -DOHOS_PLATFORM" \
  CXX="clang++ --target=$TARGET --sysroot=$SYSROOT -D__MUSL__ -DOHOS_PLATFORM" \
  libraries -j8

echo ""
echo "=== OpenH264 build complete ==="
ls -lh libopenh264.a
