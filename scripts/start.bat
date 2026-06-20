@echo off
REM OHOS Scrcpy - Windows 启动脚本
REM 自动检查环境并启动投屏

setlocal enabledelayedexpansion

set SCRIPT_DIR=%~dp0
set CLIENT_EXE=%SCRIPT_DIR%ohos_scrcpy.exe
set HDC_PATH=hdc

echo ========================================
echo   OHOS Scrcpy - OpenHarmony 投屏工具
echo ========================================
echo.

REM 检查客户端可执行文件
if not exist "%CLIENT_EXE%" (
    echo [错误] 找不到 ohos_scrcpy.exe
    echo 请确保 ohos_scrcpy.exe 在当前目录下
    pause
    exit /b 1
)

REM 检查 hdc
where hdc >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [警告] hdc 不在 PATH 中
    echo 请将 HarmonyOS SDK 的 hdc 工具路径添加到系统 PATH
    echo.
    echo 如果 hdc 在其他位置，请使用 --hdc 参数指定路径
    echo 示例: ohos_scrcpy.exe --hdc "C:\path\to\hdc.exe"
    echo.
)

REM 检查设备连接
hdc list targets >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [错误] 未检测到已连接的设备
    echo 请确保:
    echo   1. 设备已通过 USB 连接到电脑
    echo   2. 设备已开启开发者模式和 USB 调试
    echo   3. 已在设备上授权调试权限
    pause
    exit /b 1
)

REM 解析参数
set ARGS=
:parse_args
if "%~1"=="" goto run
set ARGS=%ARGS% %1
shift
goto parse_args

:run
echo [信息] 正在启动投屏...
echo.

REM 启动客户端
"%CLIENT_EXE%" %ARGS%

if %ERRORLEVEL% neq 0 (
    echo.
    echo [错误] 程序异常退出 (代码: %ERRORLEVEL%)
    pause
)
