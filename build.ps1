# ============================================================================
#   ohos-scrcpy - One-click build script (PowerShell)
#   Usage: powershell -ExecutionPolicy Bypass -File .\build.ps1
# ============================================================================

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
Write-Host ""
Write-Host "[INFO] ========================================"
Write-Host "[INFO]   ohos-scrcpy build"
Write-Host "[INFO]   Project root: $ProjectRoot"
Write-Host "[INFO] ========================================"
Write-Host ""

# ============================================================================
# 1. Detect build tools
# ============================================================================
Write-Host "[INFO] Step 1: Detect build tools..."

$candidateDirs = @(
    "C:\msys64\mingw64\bin",
    "C:\msys64\usr\bin",
    "D:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\native\llvm\bin",
    "C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\native\llvm\bin",
    "D:\software\ohos-sdk\native\llvm\bin",
    "C:\OHOS_SDK\native\llvm\bin"
)

# Also detect OHOS clang directly from known location
$ohosClangCandidates = @(
    "D:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\native\llvm\bin\clang.exe",
    "C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\native\llvm\bin\clang.exe",
    "D:\software\ohos-sdk\native\llvm\bin\clang.exe"
)

foreach ($dir in $candidateDirs) {
    if (Test-Path $dir) {
        $env:PATH = "$dir;" + $env:PATH
    }
}

function Get-CommandPath($name) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($dir in $candidateDirs) {
        $full = Join-Path $dir "$name.exe"
        if (Test-Path $full) { return $full }
        $full2 = Join-Path $dir $name
        if (Test-Path $full2) { return $full2 }
    }
    return $null
}

$mingwMake = Get-CommandPath "mingw32-make"
if (-not $mingwMake) {
    Write-Host "[ERROR] mingw32-make not found. Install MSYS2"
    exit 1
}
Write-Host "[INFO]   [OK] mingw32-make: $mingwMake"

$cmakePath = Get-CommandPath "cmake"
if (-not $cmakePath) {
    Write-Host "[ERROR] cmake not found"
    exit 1
}
Write-Host "[INFO]   [OK] cmake: $cmakePath"

$ohosClang = Get-CommandPath "aarch64-linux-ohos-clang"
# Fallback: check for clang.exe with OHOS sysroot
if (-not $ohosClang) {
    foreach ($c in $ohosClangCandidates) {
        if (Test-Path $c) {
            $ohosClang = $c
            break
        }
    }
}
$serverAvailable = [bool]$ohosClang
if ($serverAvailable) {
    Write-Host "[INFO]   [OK] OHOS clang: $ohosClang"
} else {
    Write-Host "[WARN] OHOS clang not found (skipping server build)"
    Write-Host "[WARN] Expected location: D:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\native\llvm\bin\clang.exe"
}

$bashExe = "C:\msys64\usr\bin\bash.exe"
if (-not (Test-Path $bashExe)) {
    $bashExe = $null
} else {
    Write-Host "[INFO]   [OK] MSYS2 bash: $bashExe"
}
Write-Host ""

# ============================================================================
# 2. Prepare output dir (preserve existing DLLs)
# ============================================================================
$DistDir = Join-Path $ProjectRoot "dist"
if (-not (Test-Path $DistDir)) {
    New-Item -ItemType Directory -Path $DistDir -Force | Out-Null
}

# ============================================================================
# 3. Build client (Windows x64)
# ============================================================================
Write-Host "[INFO] ========================================"
Write-Host "[INFO] Step 2: Build client (Windows x64)"
Write-Host "[INFO] ========================================"

$ClientDir = Join-Path $ProjectRoot "client"
$ClientBuildDir = Join-Path $ClientDir "build"

Push-Location $ClientDir

try {
    if (Test-Path $ClientBuildDir) {
        Write-Host "[INFO] Cleaning old build dir..."
        Remove-Item -Recurse -Force $ClientBuildDir
    }

    Write-Host "[INFO] Running: cmake configure (MinGW Makefiles)..."
    & $cmakePath -S . -B build -G "MinGW Makefiles" -DCMAKE_MAKE_PROGRAM="$mingwMake"
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

    Write-Host "[INFO] Running: cmake build..."
    & $cmakePath --build build -- -j
    if ($LASTEXITCODE -ne 0) { throw "Client build failed" }

    $clientExe = Join-Path $ClientBuildDir "ohos_scrcpy.exe"
    if (Test-Path $clientExe) {
        Copy-Item $clientExe (Join-Path $DistDir "ohos_scrcpy_client.exe") -Force
        Write-Host "[INFO] Client copied to: $DistDir\ohos_scrcpy_client.exe"
    }

    # Collect DLL dependencies recursively
    Write-Host "[INFO] Collecting DLL dependencies..."
    $msysBin = "C:\msys64\mingw64\bin"
    $systemDlls = "^(KERNEL32|USER32|GDI32|SHELL32|ADVAPI32|WS2_32|msvcrt|msvcp|CRYPT32|OLE32|OLEAUT32|SHLWAPI|SETUPAPI|VERSION|IMM32|WINMM|OPENGL32|d3d|dxgi|ntdll|dbghelp|bcrypt|ucrtbase|api-ms|secur32|cfgmgr32|SHCORE|USERENV|POWRPROF|PROPSYS|WTSAPI32|combase|SspiCli|dpapi|profapi|cryptbase|kernelbase|msvcp_win|win32u|bcryptPrimitives)"

    $queue = [System.Collections.Queue]::new()
    $visited = @{}

    # Start from the exe
    $queue.Enqueue((Join-Path $DistDir "ohos_scrcpy_client.exe"))

    while ($queue.Count -gt 0) {
        $binary = $queue.Dequeue()
        if ($visited.ContainsKey($binary)) { continue }
        $visited[$binary] = $true

        $deps = & "C:\msys64\mingw64\bin\objdump.exe" -p $binary 2>$null |
            Select-String "DLL Name" |
            ForEach-Object { ($_ -replace ".*DLL Name: ","").Trim() }

        foreach ($dep in $deps) {
            if ($dep -match $systemDlls) { continue }
            $destPath = Join-Path $DistDir $dep
            if (Test-Path $destPath) { continue }
            $srcPath = Join-Path $msysBin $dep
            if (Test-Path $srcPath) {
                Copy-Item $srcPath $destPath -Force
                Write-Host "[INFO]   + $dep"
                $queue.Enqueue($destPath)
            }
        }
    }
    Write-Host "[INFO] DLL dependencies collected"
} catch {
    Write-Host "[ERROR] $_"
    Pop-Location
    exit 1
}

Pop-Location
Write-Host "[INFO] Client build success!"
Write-Host ""

# ============================================================================
# 4. Build server (OHOS arm64)
# ============================================================================
if ($serverAvailable -and $bashExe) {
    Write-Host "[INFO] ========================================"
    Write-Host "[INFO] Step 3: Build server (OHOS arm64)"
    Write-Host "[INFO] ========================================"

    $ServerBuildDir = Join-Path $ProjectRoot "server\build_arm64"
    if (Test-Path $ServerBuildDir) {
        Remove-Item -Recurse -Force $ServerBuildDir
    }

    # Windows path -> MSYS2 path
    $msysPath = $ProjectRoot -replace '\\', '/'
    $drive = $msysPath.Substring(0,1).ToLower()
    $msysPath = '/' + $drive + $msysPath.Substring(2)

    Write-Host "[INFO] MSYS path: $msysPath/server"
    Write-Host "[INFO] Running: bash build_arm64.sh ..."

    $bashCommand = 'cd "' + $msysPath + '/server" && bash build_arm64.sh'

    # Use cmd /c to invoke bash to avoid PowerShell stdout pipe issues
    $tempOut = Join-Path $env:TEMP "ohos_build_$$.txt"
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $bashExe
    $psi.Arguments = "-lc `"$bashCommand`""
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    $outputContent = $proc.StandardOutput.ReadToEnd()
    $errorContent = $proc.StandardError.ReadToEnd()
    $proc.WaitForExit()
    $exitCode = $proc.ExitCode

    if ($outputContent) { Write-Host $outputContent }
    if ($errorContent) { Write-Host $errorContent }

    if ($exitCode -ne 0) {
        Write-Host "[ERROR] Server build failed (exit=$exitCode)"
        exit 1
    }

    $serverBin = Join-Path $ServerBuildDir "ohos_scrcpy_server"
    if (-not (Test-Path $serverBin)) {
        Write-Host "[ERROR] Server binary not found: $serverBin"
        exit 1
    }

    Copy-Item $serverBin (Join-Path $DistDir "ohos_scrcpy_server") -Force
    Write-Host "[INFO] Server copied to: $DistDir\ohos_scrcpy_server"
    Write-Host "[INFO] Server build success!"
} else {
    Write-Host "[INFO] Skipping server build"
}

# ============================================================================
# Done
# ============================================================================
Write-Host ""
Write-Host "[INFO] ========================================"
Write-Host "[INFO]   All builds completed!"
Write-Host "[INFO]   Output dir: $DistDir"
if (Test-Path (Join-Path $DistDir "ohos_scrcpy_client.exe")) {
    Write-Host "[INFO]     - ohos_scrcpy_client.exe"
}
if (Test-Path (Join-Path $DistDir "ohos_scrcpy_server")) {
    Write-Host "[INFO]     - ohos_scrcpy_server"
}
Write-Host "[INFO] ========================================"
Write-Host ""
