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
foreach ($candidate in @("C:\msys64\mingw64\bin", "C:\msys2\mingw64\bin", "D:\msys64\mingw64\bin")) {
    if (Test-Path $candidate -PathType Container) {
        $env:PATH = "$candidate;$env:PATH"
        break
    }
}
if (-not $SkipCMake) {
    if (-not (Test-Path (Join-Path $BUILD_DIR "CMakeCache.txt"))) {
        $gcc = (Get-Command gcc -ErrorAction Stop).Source
        $gxx = (Get-Command g++ -ErrorAction Stop).Source
        & cmake -S $PROJECT_ROOT -B $BUILD_DIR -G "MinGW Makefiles" `
            "-DCMAKE_C_COMPILER=$gcc" "-DCMAKE_CXX_COMPILER=$gxx" "-DCMAKE_BUILD_TYPE=$Config" `
            -DARIAREAD_ENFORCE_SELF_CONTAINED=ON -DARIAREAD_USE_SYSTEM_CURL=OFF
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
