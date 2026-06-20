#!/bin/bash
# OHOS Scrcpy - 一键部署脚本
# 将服务端推送到设备并启动客户端

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# 默认参数
SERIAL=""
SCALE=0.5
BITRATE=4000000
FPS=60
HDC="hdc"

usage() {
    echo "Usage: $0 [options]"
    echo ""
    echo "Options:"
    echo "  -s SERIAL     Device serial number"
    echo "  --scale N     Screen scale (default: 0.5)"
    echo "  --bitrate N   Video bitrate in bps (default: 4000000)"
    echo "  --fps N       Target FPS (default: 60)"
    echo "  --hdc PATH    Path to hdc executable"
    echo "  -h            Show this help"
    exit 0
}

while [[ $# -gt 0 ]]; do
    case $1 in
        -s) SERIAL="$2"; shift 2 ;;
        --scale) SCALE="$2"; shift 2 ;;
        --bitrate) BITRATE="$2"; shift 2 ;;
        --fps) FPS="$2"; shift 2 ;;
        --hdc) HDC="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) shift ;;
    esac
done

echo "=== OHOS Scrcpy Quick Start ==="

# 检查设备
HDC_CMD="$HDC"
if [ -n "$SERIAL" ]; then
    HDC_CMD="$HDC -s $SERIAL"
fi

echo "Checking device connection..."
$HDC_CMD list targets

# 设置端口转发
echo "Setting up port forwarding..."
$HDC_CMD fport tcp:9901 tcp:9901 2>/dev/null || true
$HDC_CMD fport tcp:9902 tcp:9902 2>/dev/null || true

# 启动客户端
echo "Starting client..."
CLIENT_BIN="$PROJECT_DIR/client/build/ohos_scrcpy"
if [ ! -f "$CLIENT_BIN" ]; then
    CLIENT_BIN="$PROJECT_DIR/client/build/Release/ohos_scrcpy.exe"
fi

if [ -f "$CLIENT_BIN" ]; then
    "$CLIENT_BIN" --scale $SCALE --bitrate $BITRATE --fps $FPS --hdc "$HDC"
else
    echo "Client binary not found. Please build first."
    echo "Run: $PROJECT_DIR/scripts/build_client.sh"
    exit 1
fi
