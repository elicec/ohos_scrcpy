# ohos-scrcpy 操作说明

## 1. 快速开始

### 1.1 环境要求

- Windows 10/11 PC
- 已安装 DevEco Studio 及 OpenHarmony SDK
- 已开启 hdc（HarmonyOS Device Connector）
- 设备通过 USB 连接电脑并开启调试模式

### 1.2 启动客户端

进入 `dist` 目录，双击或命令行运行：

```powershell
ohos_scrcpy_client.exe
```

客户端会自动完成：

1. 通过 hdc 建立端口转发（视频 9901 / 控制 9902）
2. 推送服务端 `ohos_scrcpy_server` 到设备 `/data/local/tmp/`
3. 启动服务端并连接
4. 弹出窗口显示设备屏幕

## 2. 鼠标操作

| 操作 | 说明 |
|------|------|
| 左键单击 | 模拟单指点击 |
| 左键拖动 | 模拟单指滑动 |
| 滚轮 | 模拟上下滑动滚动 |

> 窗口会按设备屏幕宽高比保持比例渲染，黑边区域不会触发点击。

## 3. 键盘快捷键

### 3.1 系统快捷键

| 快捷键 | 功能 |
|--------|------|
| `Alt + B` | 返回键（Back） |
| `Alt + H` | 主页键（Home） |
| `Alt + P` | 电源键（Power） |
| `Alt + ↑` | 音量加（Volume Up） |
| `Alt + ↓` | 音量减（Volume Down） |

### 3.2 普通按键

以下按键会直接透传到设备（当前已映射）：

- 字母：`A-Z`
- 数字：`0-9`
- 方向键：`↑ ↓ ← →`
- 常用键：`Enter`、`Esc`、`Backspace`、`Tab`、`Space`
- 修饰键：`Shift`、`Ctrl`、`Alt`（左右键均支持）
- 功能键：`PageUp`、`PageDown`、`Home`、`End`、`Insert`、`Delete`

> 如需支持更多按键，可在 `client/src/input_handler.c` 的 `sdl_keycode_to_linux_keycode()` 中补充映射。

## 4. 调试

### 4.1 查看服务端日志

```bash
hdc shell tail -f /data/local/tmp/ohos_scrcpy_server.log
```

### 4.2 查看输入事件

使用项目自带的 `getevent` 工具：

```bash
hdc file send tools/getevent/build_arm64/getevent /data/local/tmp/getevent
hdc shell chmod +x /data/local/tmp/getevent
hdc shell /data/local/tmp/getevent -l
hdc shell /data/local/tmp/getevent /dev/input/eventX
```

### 4.3 常见问题

| 现象 | 处理 |
|------|------|
| 窗口黑屏 | 检查服务端日志，确认编码器是否初始化成功 |
| 无法控制设备 | 确认服务端已启动，且控制通道 9902 已转发 |
| 坐标偏差 | 确认使用的是 `dist` 目录下的最新服务端二进制 |
| 按键无响应 | 确认 `ohos-scrcpy keyboard` 虚拟输入设备已创建 |

## 5. 重新编译

```powershell
.\build.ps1
```

编译完成后，客户端和服务端二进制会自动输出到 `dist` 目录。
