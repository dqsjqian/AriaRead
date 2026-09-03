#!/usr/bin/env pwsh
# ──────────────────────────────────────────────
# OpenRead Web 版 · Windows 一键构建 & 打包脚本
#
# 构建原生 C++ Web Server（零 Python 依赖，性能最优）
# 全流程：环境检测 → C++ 编译 → 验证 → 打包
#
# 用法：
#   .\scripts\build_web_release.ps1              # 增量构建
#   .\scripts\build_web_release.ps1 -Clean      # 全量 clean 构建
#   .\scripts\build_web_release.ps1 -SkipCMake  # 跳过 C++ 编译（仅重新打包）
#
# 产物：
#   release\web\openread.exe       — 原生 C++ Web Server 可执行文件
#   release\web\*.dll             — 运行时动态库
# ──────────────────────────────────────────────

param(
    [switch]$Clean,
    [switch]$SkipCMake
)

$ErrorActionPreference = "Stop"

# ── 自动检测并添加 MSYS2 MinGW 到 PATH ──
$MSYS2_CANDIDATES = @(
    "C:\msys64\mingw64\bin",
    "C:\msys2\mingw64\bin",
    "D:\msys64\mingw64\bin"
)
$MSYS2_FOUND = $false
foreach ($path in $MSYS2_CANDIDATES) {
    if (Test-Path $path -PathType Container) {
        if ($env:PATH -notlike "*$path*") {
            $env:PATH = "$path;$env:PATH"
            Write-Host "🛠️  已自动添加 MSYS2 MinGW 到 PATH: $path" -ForegroundColor DarkGray
        }
        $MSYS2_FOUND = $true
        break
    }
}

$SCRIPT_DIR = Split-Path -Parent $MyInvocation.MyCommand.Definition
$PROJECT_ROOT = Resolve-Path (Join-Path $SCRIPT_DIR "..")
$RELEASE_DIR = Join-Path $PROJECT_ROOT "release\web"
$BUILD_DIR = Join-Path $PROJECT_ROOT "build"
$OPENREAD_PKG = Join-Path $PROJECT_ROOT "bindings\web\openread"

# 总阶段数
$PhaseNum = 4

Write-Host ""
Write-Host "╔═══════════════════════════════════════════════╗"
Write-Host "║  📦 OpenRead Web 版 · 原生 C++ 构建 & 打包    ║"
Write-Host "╚═══════════════════════════════════════════════╝"
Write-Host ""

# ══════════════════════════════════════════════
# 阶段 1：环境检测
# ══════════════════════════════════════════════
Write-Host "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
Write-Host "🔍 阶段 1/$PhaseNum - 环境检测"
Write-Host "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

$HAS_ERROR = $false

# 检查 cmake
$CMAKE = Get-Command cmake -ErrorAction SilentlyContinue
if ($CMAKE) {
    $CMAKE_VER = (cmake --version | Select-Object -First 1) -replace "[^0-9.]"
    Write-Host "  ✅ cmake $CMAKE_VER"
} else {
    Write-Host "  ❌ cmake 未安装"
    $HAS_ERROR = $true
}

# 检查 C++ 编译器（MinGW）
$GCC = Get-Command gcc -ErrorAction SilentlyContinue
if ($GCC) {
    $GCC_VER = (gcc --version | Select-Object -First 1)
    Write-Host "  ✅ C++ 编译器: $GCC_VER"
} else {
    Write-Host "  ❌ gcc 未找到，请将 MSYS2 MinGW 的 bin 目录加入 PATH"
    $HAS_ERROR = $true
}

# 检查 web 静态资源
$WEB_DIR = Join-Path $OPENREAD_PKG "web"
if (Test-Path $WEB_DIR -PathType Container) {
    $WEB_FILES = (Get-ChildItem $WEB_DIR -Recurse -File).Count
    Write-Host "  ✅ Web 静态资源: $WEB_FILES 个文件"
} else {
    Write-Host "  ❌ Web 静态资源目录不存在: $WEB_DIR"
    $HAS_ERROR = $true
}

if ($HAS_ERROR) {
    Write-Host ""
    Write-Host "❌ 环境检测未通过，请先安装缺失的依赖"
    exit 1
}

Write-Host ""

# ══════════════════════════════════════════════
# 阶段 2：C++ 编译
# ══════════════════════════════════════════════
Write-Host "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
Write-Host "🔨 阶段 2/$PhaseNum - C++ 编译"
Write-Host "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if ($SkipCMake) {
    Write-Host "  ⏭️  跳过（-SkipCMake）"
} else {
    # 获取编译器路径（用于 CMake）
    $GCC_PATH = (Get-Command gcc).Source
    $GXX_PATH = (Get-Command g++).Source

    # --clean 时清理构建目录
    if ($Clean) {
        Write-Host "  🗑️  Clean 模式：清理构建目录..."
        if (Test-Path $BUILD_DIR) {
            Remove-Item -Recurse -Force $BUILD_DIR
        }
    }

    # 检查缓存是否有效：CMakeCache.txt 存在且 OpenSSL 头文件完整
    $NEED_RECONFIGURE = $false
    $CACHE_FILE = Join-Path $BUILD_DIR "CMakeCache.txt"
    if (Test-Path $CACHE_FILE -PathType Leaf) {
        # 从缓存中解析 OPENSSL_INCLUDE_DIR
        $opensslIncludeLine = Select-String -Path $CACHE_FILE -Pattern "^OPENSSL_INCLUDE_DIR:PATH=(.+)$"
        if ($opensslIncludeLine) {
            $opensslIncludeDir = $opensslIncludeLine.Matches.Groups[1].Value.Trim()
            $sslHeader = Join-Path $opensslIncludeDir "openssl\ssl.h"
            if (-not (Test-Path $sslHeader -PathType Leaf)) {
                Write-Host "  ⚠️  检测到 OpenSSL 头文件缺失，缓存无效"
                $NEED_RECONFIGURE = $true
                # 清理 OpenSSL 安装产物和 stamp，强制 ExternalProject 重新安装
                $opensslInstallDir = Join-Path $BUILD_DIR "_deps\openssl-install"
                $opensslStamp = Join-Path $BUILD_DIR "openssl_external-prefix\src\openssl_external-stamp\openssl_external-install"
                if (Test-Path $opensslInstallDir) {
                    Remove-Item -Recurse -Force $opensslInstallDir
                    Write-Host "  🗑️  已清理损坏的 OpenSSL 安装目录"
                }
                if (Test-Path $opensslStamp) {
                    Remove-Item -Force $opensslStamp
                    Write-Host "  🗑️  已清理 OpenSSL install stamp"
                }
            }
        } else {
            Write-Host "  ⚠️  无法从缓存中确认 OpenSSL 路径，将重新 configure"
            $NEED_RECONFIGURE = $true
        }
    } else {
        $NEED_RECONFIGURE = $true
    }

    if ($NEED_RECONFIGURE) {
        Write-Host "  📐 cmake configure..."
        $cmakeArgs = @(
            "-S", "$PROJECT_ROOT",
            "-B", "$BUILD_DIR",
            "-G", "MinGW Makefiles",
            "-DCMAKE_C_COMPILER=$GCC_PATH",
            "-DCMAKE_CXX_COMPILER=$GXX_PATH",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DOPENREAD_BUILD_TESTS=OFF",
            "-DOPENREAD_ENFORCE_SELF_CONTAINED=ON",
            "-DOPENREAD_USE_SYSTEM_CURL=OFF"
        )
        & cmake @cmakeArgs
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  ❌ cmake configure 失败"
            exit 1
        }
    } else {
        Write-Host "  📐 cmake configure（已有缓存，跳过）"
    }

    # cmake build（增量编译）
    $NPROC = (Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors
    if (-not $NPROC) { $NPROC = 4 }
    Write-Host "  🔧 cmake build openread_web_server（${NPROC} 线程，增量编译）..."
    $env:PATH = "C:\msys64\mingw64\bin;C:\msys64\usr\bin;$env:PATH"
    cmake --build "$BUILD_DIR" --target openread_web_server -j"$NPROC"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  ❌ cmake build 失败"
        exit 1
    }
    Write-Host "  ✅ C++ 编译完成"
}

Write-Host ""

# ══════════════════════════════════════════════
# 阶段 3：验证构建产物
# ══════════════════════════════════════════════
Write-Host "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
Write-Host "📋 阶段 3/$PhaseNum - 验证构建产物"
Write-Host "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# 验证 openread_web_server 可执行文件
$NATIVE_BIN = Join-Path $PROJECT_ROOT "build\bin\openread_web_server.exe"
if (-not (Test-Path $NATIVE_BIN -PathType Leaf)) {
    Write-Host "  ❌ 未找到 openread_web_server.exe: $NATIVE_BIN"
    Write-Host "     请检查 CMake 构建日志"
    exit 1
}
$NATIVE_SIZE = "{0:N2} MB" -f ((Get-Item $NATIVE_BIN).Length / 1MB)
Write-Host "  📦 可执行文件: openread_web_server.exe ($NATIVE_SIZE)"

# 验证关键方法符号
$REQUIRED_SYMBOLS = @("register_routes", "catalog_with_cache", "content_with_cache")
$MISSING_SYMBOLS = @()
$stringsOutput = & strings $NATIVE_BIN 2>$null
foreach ($sym in $REQUIRED_SYMBOLS) {
    if (-not ($stringsOutput | Select-String $sym)) {
        $MISSING_SYMBOLS += $sym
    }
}
if ($MISSING_SYMBOLS.Count -gt 0) {
    Write-Host "  ⚠️  可执行文件缺少关键方法: $($MISSING_SYMBOLS -join ', ')"
} else {
    Write-Host "  ✅ 关键方法验证通过"
}

# 检查动态库依赖
Write-Host "  🔍 检查动态库依赖..."
$deps = & objdump -x $NATIVE_BIN | Select-String "DLL Name"
Write-Host "     依赖 DLL 列表:"
$deps | ForEach-Object { Write-Host "       $($_ -replace '.*DLL Name: ', '')" }

Write-Host ""

# ══════════════════════════════════════════════
# 阶段 4：打包
# ══════════════════════════════════════════════
Write-Host "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
Write-Host "📦 阶段 4/$PhaseNum - 打包"
Write-Host "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

New-Item -ItemType Directory -Force -Path $RELEASE_DIR | Out-Null

# 复制主可执行文件
$destExe = Join-Path $RELEASE_DIR "openread.exe"
Copy-Item $NATIVE_BIN $destExe -Force
Write-Host "  📦 已复制 openread_web_server.exe → release\web\openread.exe"

# 复制 Aria DLL（如果有的话）
$buildBinDir = Join-Path $PROJECT_ROOT "build\bin"
$ariaDlls = Get-ChildItem $buildBinDir -Filter "libaria_*.dll" -ErrorAction SilentlyContinue
foreach ($dll in $ariaDlls) {
    Copy-Item $dll.FullName (Join-Path $RELEASE_DIR $dll.Name) -Force
    Write-Host "  📦 已复制 $($dll.Name)"
}

# 复制 MinGW 运行时 DLL
$mingwBin = Split-Path -Parent (Get-Command gcc).Source
$mingwDlls = @("libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll")
foreach ($dll in $mingwDlls) {
    $src = Join-Path $mingwBin $dll
    if (Test-Path $src -PathType Leaf) {
        Copy-Item $src (Join-Path $RELEASE_DIR $dll) -Force
        Write-Host "  📦 已复制 MinGW DLL: $dll"
    }
}

# 复制前端静态资源
$webSrc = Join-Path $OPENREAD_PKG "web"
if (Test-Path $webSrc -PathType Container) {
    $webDest = Join-Path $RELEASE_DIR "web"
    if (Test-Path $webDest) { Remove-Item -Recurse -Force $webDest }
    Copy-Item -Recurse $webSrc $webDest
    $webCount = (Get-ChildItem $webDest -Recurse -File).Count
    Write-Host "  📦 已复制前端静态资源: $webCount 个文件"
}

Write-Host "  ✅ 打包完成"

Write-Host ""

# ══════════════════════════════════════════════
# 验证产物
# ══════════════════════════════════════════════
Write-Host "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
Write-Host "✅ 验证产物"
Write-Host "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

$BINARY = Join-Path $RELEASE_DIR "openread.exe"
if (-not (Test-Path $BINARY -PathType Leaf)) {
    Write-Host "❌ 打包失败，未找到 openread.exe"
    exit 1
}

$SIZE = "{0:N2} MB" -f ((Get-Item $BINARY).Length / 1MB)
Write-Host "  📁 产物: release\web\openread.exe ($SIZE)"

# 验证二进制架构
Write-Host "  🔍 验证二进制架构..."
$ARCH_INFO = & objdump -f $BINARY | Select-String "file format|architecture"
$ARCH_INFO | ForEach-Object { Write-Host "     $_" }

# 验证最终二进制的动态库依赖
Write-Host "  🔍 验证最终二进制动态库依赖..."
$BINARY_DEPS = & objdump -x $BINARY | Select-String "DLL Name"
$BINARY_DEPS | ForEach-Object { Write-Host "     $($_ -replace '.*DLL Name: ', '')" }

Write-Host ""

# ══════════════════════════════════════════════
# 完成
# ══════════════════════════════════════════════
Write-Host "═══════════════════════════════════════════════"
Write-Host "✅ 全流程构建完成！"
Write-Host ""
Write-Host "📁 产物目录: release\web\（原生 C++ Web Server）"
Write-Host "   ├── openread.exe  ($SIZE) — 主程序（C++ 原生）"
Write-Host "   ├── libaria_*.dll        — Aria 动态库"
Write-Host "   ├── web\                 — 前端静态资源"
Write-Host "   └── *.dll                — MinGW 运行时"
Write-Host ""
Write-Host "🚀 本机使用:"
Write-Host "   .\release\web\openread.exe 9091     # 指定端口"
Write-Host "   .\release\web\openread.exe -p 9091  # 同上"
Write-Host "   .\release\web\openread.exe          # 默认 9091"
Write-Host ""
Write-Host "📋 分发给他人:"
Write-Host "   将 release\web\ 整个目录发送给对方"
