# OHOS Scrcpy 服务端 - 编译脚本
# 在 OpenHarmony 全量源码环境中编译服务端

#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OH_ROOT="${1:-$OHOS_ROOT}"

if [ -z "$OH_ROOT" ]; then
    echo "Usage: $0 <OpenHarmony source root>"
    echo "  or set OHOS_ROOT environment variable"
    exit 1
fi

echo "=== OHOS Scrcpy Server Build ==="
echo "OpenHarmony root: $OH_ROOT"

# 创建目标目录
SERVER_DIR="$OH_ROOT/foundation/multimedia/player_framework/OHScrcpy_Server"
mkdir -p "$SERVER_DIR"

# 复制源文件
echo "Copying source files..."
cp "$SCRIPT_DIR/src/main.c" "$SERVER_DIR/"
cp "$SCRIPT_DIR/src/screen_capture.c" "$SERVER_DIR/"
cp "$SCRIPT_DIR/src/video_encoder.c" "$SERVER_DIR/"
cp "$SCRIPT_DIR/src/input_injector.c" "$SERVER_DIR/"
cp "$SCRIPT_DIR/src/tcp_server.c" "$SERVER_DIR/"
cp "$SCRIPT_DIR/include/"*.h "$SERVER_DIR/"
cp "$SCRIPT_DIR/../common/"*.h "$SERVER_DIR/"
cp "$SCRIPT_DIR/BUILD.gn" "$SERVER_DIR/"

# 应用补丁(将编译目标添加到 bundle.json)
echo "Patching bundle.json..."
cd "$OH_ROOT/foundation/multimedia/player_framework/"
if ! grep -q "OHScrcpy_Server" bundle.json; then
    # 备份原文件
    cp bundle.json bundle.json.bak
    # 在 base_group 中添加编译目标
    sed -i 's|"//foundation/multimedia/player_framework/frameworks/native/player:player_packages"|"//foundation/multimedia/player_framework/frameworks/native/player:player_packages",\n      "//foundation/multimedia/player_framework/OHScrcpy_Server:ohos_scrcpy_server"|' bundle.json
    echo "bundle.json patched"
else
    echo "bundle.json already patched"
fi

# 编译
echo "Building..."
cd "$OH_ROOT"
PRODUCT_NAME="${2:-rk3568}"
./build.sh --product-name "$PRODUCT_NAME" --build-target ohos_scrcpy_server

# 复制产物
OUTPUT_DIR="$SCRIPT_DIR/bin"
mkdir -p "$OUTPUT_DIR"
if [ -f "$OH_ROOT/out/$PRODUCT_NAME/multimedia/player_framework/ohos_scrcpy_server" ]; then
    cp "$OH_ROOT/out/$PRODUCT_NAME/multimedia/player_framework/ohos_scrcpy_server" "$OUTPUT_DIR/"
    echo "Build successful! Output: $OUTPUT_DIR/ohos_scrcpy_server"
else
    echo "Build may have failed. Check build output."
    exit 1
fi
