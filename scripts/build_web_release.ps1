#!/usr/bin/env pwsh
# Build and distribute one runtime directory: build/bin (or bin/<config>).
param(
    [switch]$Clean,
    [switch]$SkipCMake,
    [string]$Config = "Release"
)
$ErrorActionPreference = "Stop"
if ($Clean -and $SkipCMake) { throw "-Clean cannot be combined with -SkipCMake" }
$PROJECT_ROOT = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$BUILD_DIR = if ($env:ARIAREAD_BUILD_DIR) { $env:ARIAREAD_BUILD_DIR } else { Join-Path $PROJECT_ROOT "build" }

# Preserve the existing MinGW workflow while also allowing an existing MSVC cache.
foreach ($candidate in @("C:\DevTools\msys64\ucrt64\bin", "C:\DevTools\msys64\mingw64\bin", "C:\msys64\mingw64\bin", "C:\msys2\mingw64\bin", "D:\msys64\mingw64\bin")) {
    if (Test-Path $candidate -PathType Container) {
        $env:PATH = "$candidate;$env:PATH"
        break
    }
}

# Toolchain auto-detect: MSVC first (self-contained env assembly -- no
# vcvarsall.bat and no reg.exe, which restricted environments may block), then
# fall back to MinGW gcc. Both feed Ninja single-config builds.
$clOnPath = (Get-Command cl -ErrorAction SilentlyContinue) -ne $null
if (-not $clOnPath) {
    # vswhere first: it finds the VS installation root wherever it is
    # installed; the hardcoded paths below are only fallback candidates.
    $vsRoots = @()
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $detected = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null | Select-Object -First 1
        if ($detected) { $vsRoots += $detected }
    }
    $vsRoots += @("C:\DevTools\VS2026", "D:\VS2026", "C:\Program Files\Microsoft Visual Studio", "C:\Program Files (x86)\Microsoft Visual Studio")
    foreach ($vsRoot in $vsRoots) {
        if (-not (Test-Path $vsRoot)) { continue }
        $msvcDir = Get-ChildItem (Join-Path $vsRoot "VC\Tools\MSVC") -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name | Select-Object -Last 1
        $kitsRoot = "D:\Windows Kits\10"
        if (-not (Test-Path $kitsRoot)) { $kitsRoot = "C:\Program Files (x86)\Windows Kits\10" }
        $sdkDir = Get-ChildItem (Join-Path $kitsRoot "Include") -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^\d+\.' } | Sort-Object Name -Descending | Select-Object -First 1
        if (-not $msvcDir -or -not $sdkDir) { continue }
        $vctools = $msvcDir.FullName
        $sdkver = $sdkDir.Name
        $ninja = Join-Path $vsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
        $env:PATH = "$vctools\bin\Hostx64\x64;$kitsRoot\bin\$sdkver\x64;$ninja;C:\Program Files\CMake\bin;$env:PATH"
        $env:INCLUDE = "$vctools\include;$kitsRoot\Include\$sdkver\ucrt;$kitsRoot\Include\$sdkver\um;$kitsRoot\Include\$sdkver\shared;$kitsRoot\Include\$sdkver\winrt;$kitsRoot\Include\$sdkver\cppwinrt"
        $env:LIB = "$vctools\lib\x64;$kitsRoot\Lib\$sdkver\ucrt\x64;$kitsRoot\Lib\$sdkver\um\x64"
        $env:WindowsSdkDir = "$kitsRoot\"
        $env:WindowsSDKVersion = "$sdkver\"
        $env:VCToolsInstallDir = "$vctools\"
        break
    }
}

if (-not $SkipCMake) {
    # Pinned dependency prefix: built once by tools\ci\build_ariaread_deps.py.
    # Configure itself never touches the network.
    $depsManifest = Join-Path $PROJECT_ROOT "build\deps\prefix\share\ariaread-deps\manifest.json"
    if (-not (Test-Path $depsManifest)) {
        Write-Host "[deps] pinned dependency prefix missing - building it (first run only)..."
        & python (Join-Path $PROJECT_ROOT "tools\ci\build_ariaread_deps.py")
        if ($LASTEXITCODE -ne 0) { throw "Dependency prefix build failed" }
    }
    if (-not (Test-Path (Join-Path $BUILD_DIR "CMakeCache.txt"))) {
        $cl = Get-Command cl -ErrorAction SilentlyContinue
        if ($cl) {
            & cmake -S $PROJECT_ROOT -B $BUILD_DIR -G Ninja `
                -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl "-DCMAKE_BUILD_TYPE=$Config" `
                -DARIAREAD_ENFORCE_SELF_CONTAINED=ON -DARIAREAD_USE_SYSTEM_CURL=OFF
        } else {
            $gcc = (Get-Command gcc -ErrorAction Stop).Source
            $gxx = (Get-Command g++ -ErrorAction Stop).Source
            & cmake -S $PROJECT_ROOT -B $BUILD_DIR -G "MinGW Makefiles" `
                "-DCMAKE_C_COMPILER=$gcc" "-DCMAKE_CXX_COMPILER=$gxx" "-DCMAKE_BUILD_TYPE=$Config" `
                -DARIAREAD_ENFORCE_SELF_CONTAINED=ON -DARIAREAD_USE_SYSTEM_CURL=OFF
        }
        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
    }
    $buildArgs = @("--build", $BUILD_DIR, "--config", $Config, "--target", "ariaread_web_server")
    if ($Clean) { $buildArgs += "--clean-first" }
    $jobs = if ($env:ARIAREAD_BUILD_JOBS) { $env:ARIAREAD_BUILD_JOBS } else { [Environment]::ProcessorCount }
    & cmake @buildArgs --parallel $jobs
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed" }
}
$RUNTIME_DIR = Join-Path $BUILD_DIR "bin"
if (Test-Path (Join-Path $RUNTIME_DIR "$Config/ariaread_web_server.exe")) {
    $RUNTIME_DIR = Join-Path $RUNTIME_DIR $Config
}
$binary = Join-Path $RUNTIME_DIR "ariaread_web_server.exe"
if (-not (Test-Path $binary -PathType Leaf)) { throw "Server executable is missing: $binary" }
if ($SkipCMake) {
    # Refresh the same CMake-managed runtime, including optional MinGW DLLs.
    $cache = Get-Content (Join-Path $BUILD_DIR "CMakeCache.txt")
    $compilerLine = $cache | Select-String '^CMAKE_CXX_COMPILER:FILEPATH=(.*)$' | Select-Object -First 1
    $compiler = if ($compilerLine) { $compilerLine.Matches[0].Groups[1].Value } else { "" }
    $mingw = $compiler -match '(g\+\+|clang\+\+)\.exe$' -and $compiler -match '(mingw|msys)'
    & cmake "-DARIAREAD_SOURCE_DIR=$PROJECT_ROOT" "-DARIAREAD_OUTPUT_DIR=$RUNTIME_DIR" `
        "-DARIAREAD_MINGW=$mingw" "-DARIAREAD_CXX_COMPILER=$compiler" `
        -P (Join-Path $PROJECT_ROOT "cmake/SyncWebRuntime.cmake")
    if ($LASTEXITCODE -ne 0) { throw "Runtime asset sync failed" }
}
& $binary --help | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Runtime executable check failed" }
Write-Host "Ready: $RUNTIME_DIR"
Write-Host "Run: & '$binary'"
Write-Host "Distribute this directory with its libraries and web/ assets."
