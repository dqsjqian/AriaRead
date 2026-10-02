#!/usr/bin/env pwsh
# Build and distribute one runtime directory: build/bin (or bin/<config>).
#
# This is the single Windows entry point. It replaces the former
# scripts\build_msvc.bat: MSVC is detected here, the environment is assembled
# without vcvarsall.bat (restricted environments may block reg.exe), and the
# Ninja generator is used instead of "Visual Studio 18 2026" because MSBuild
# throws MSB6001 when the host shell chain injects duplicate "Path"/"PATH"
# environment keys. A MinGW gcc toolchain is used when no MSVC compiler is
# found.
param(
    [switch]$Clean,
    [switch]$SkipCMake,
    [string]$Config = "Release"
)
$ErrorActionPreference = "Stop"
# The pinned dependency prefix (build\deps\prefix) ships Release-only static
# libs; any other build type compiles /MDd objects against /MD libraries and
# dies in LNK2038 _ITERATOR_DEBUG_LEVEL mismatches. Fail fast here instead.
if ($Config -ne "Release") {
    throw "Only -Config Release is supported: the pinned dependency prefix ships Release-only static libs."
}
if ($Clean -and $SkipCMake) { throw "-Clean cannot be combined with -SkipCMake" }
$PROJECT_ROOT = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$BUILD_DIR = if ($env:ARIAREAD_BUILD_DIR) { $env:ARIAREAD_BUILD_DIR } else { Join-Path $PROJECT_ROOT "build" }

# Preserve the existing MinGW workflow while also allowing an existing MSVC cache.
$mingwCandidates = @()
if ($env:MSYS2_ROOT) { $mingwCandidates += (Join-Path $env:MSYS2_ROOT "ucrt64\bin") }
$mingwCandidates += @("C:\msys64\ucrt64\bin", "C:\msys64\mingw64\bin", "C:\msys2\mingw64\bin")
# A self-contained MSYS2 can live on any fixed drive (for example
# D:\worksoft\msys64). Without one of these, PATH keeps no gcc and the pinned
# dependency build falls through to "Compiler is unavailable: gcc".
$mingwCandidates += @("D", "E", "F", "G") | ForEach-Object {
    @("$($_):\msys64\ucrt64\bin", "$($_):\msys2\ucrt64\bin", "$($_):\worksoft\msys64\ucrt64\bin")
}
foreach ($candidate in $mingwCandidates) {
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
    if ($env:ARIAREAD_VS_ROOT) { $vsRoots += $env:ARIAREAD_VS_ROOT }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $detected = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null | Select-Object -First 1
        if ($detected) { $vsRoots += $detected }
    }
    $vsRoots += @("C:\Program Files\Microsoft Visual Studio", "C:\Program Files (x86)\Microsoft Visual Studio")
    # A self-contained Visual Studio can live on any fixed drive (for example
    # D:\worksoft\VS2026). vswhere normally reports it, but it is skipped when
    # the environment blocks it, so keep a probe as a last resort.
    $vsRoots += @("D", "E", "F", "G") | ForEach-Object {
        @("$($_):\Microsoft Visual Studio", "$($_):\Program Files\Microsoft Visual Studio",
          "$($_):\worksoft\VS2026", "$($_):\Program Files\Microsoft Visual Studio\2022")
    }
    foreach ($vsRoot in $vsRoots) {
        if (-not (Test-Path $vsRoot)) { continue }
        $msvcDir = Get-ChildItem (Join-Path $vsRoot "VC\Tools\MSVC") -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name | Select-Object -Last 1
        $kitsRoot = if ($env:ARIAREAD_WINDOWS_KITS_ROOT) { $env:ARIAREAD_WINDOWS_KITS_ROOT }
                    elseif ($env:WindowsSdkDir) { $env:WindowsSdkDir }
                    else { Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10" }
        # A standalone SDK can sit on any fixed drive (for example
        # D:\Windows Kits\10), so probe the others when the Program Files
        # default is absent. The explicit override is still checked first.
        if (-not (Test-Path (Join-Path $kitsRoot "Include"))) {
            $kitsRoot = @("D", "E", "F", "G") |
                ForEach-Object { "$($_):\Windows Kits\10" } |
                Where-Object { Test-Path (Join-Path $_ "Include") } |
                Select-Object -First 1
            if (-not $kitsRoot) { continue }
        }
        $sdkDir = Get-ChildItem (Join-Path $kitsRoot "Include") -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^\d+\.' } | Sort-Object Name -Descending | Select-Object -First 1
        if (-not $msvcDir -or -not $sdkDir) { continue }
        $vctools = $msvcDir.FullName
        $sdkver = $sdkDir.Name
        $ninja = Join-Path $vsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
        # Common7\IDE and Common7\Tools carry the helper executables CMake and
        # Ninja invoke; the MSVC compiler and SDK directories come first so an
        # older toolchain elsewhere on PATH cannot win.
        $cmakeBin = (Get-Command cmake -ErrorAction SilentlyContinue).Source
        if (-not $cmakeBin) { $cmakeBin = "C:\Program Files\CMake\bin\cmake.exe" }
        $env:PATH = "$vctools\bin\Hostx64\x64;$kitsRoot\bin\$sdkver\x64;$ninja;$(Split-Path $vsRoot -Parent)\Common7\IDE;$vsRoot\Common7\IDE;$vsRoot\Common7\Tools;$(Split-Path $cmakeBin -Parent);$env:PATH"
        $env:INCLUDE = "$vctools\include;$kitsRoot\Include\$sdkver\ucrt;$kitsRoot\Include\$sdkver\um;$kitsRoot\Include\$sdkver\shared;$kitsRoot\Include\$sdkver\winrt;$kitsRoot\Include\$sdkver\cppwinrt"
        $env:LIB = "$vctools\lib\x64;$kitsRoot\Lib\$sdkver\ucrt\x64;$kitsRoot\Lib\$sdkver\um\x64"
        $env:WindowsSdkDir = "$kitsRoot\"
        $env:WindowsSDKVersion = "$sdkver\"
        $env:VCToolsInstallDir = "$vctools\"
        break
    }
}

if (-not $SkipCMake) {
    # Verify the pin on every build; ARIA_SOURCE may select a local Git source.
    & python (Join-Path $PROJECT_ROOT "tools\ci\fetch_aria.py")
    if ($LASTEXITCODE -ne 0) { throw "Aria dependency verification failed" }
    # Verify the lock, compiler, patches and installed contents on every build.
    & python (Join-Path $PROJECT_ROOT "tools\ci\build_ariaread_deps.py")
    if ($LASTEXITCODE -ne 0) { throw "Dependency prefix build failed" }
    # Always (re)configure: the pinned deps prefix ships Release-only static
    # libs, so a stale cache in any other build type links /MDd objects
    # against /MD libraries and dies in LNK2038 mismatches. Re-running cmake
    # overrides the cached build type in place and rebuilds only what the
    # change touches.
    $cl = Get-Command cl -ErrorAction SilentlyContinue
    if ($cl) {
        & cmake -S $PROJECT_ROOT -B $BUILD_DIR -G Ninja `
            -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl "-DCMAKE_BUILD_TYPE=$Config"
    } else {
        $gcc = (Get-Command gcc -ErrorAction Stop).Source
        $gxx = (Get-Command g++ -ErrorAction Stop).Source
        & cmake -S $PROJECT_ROOT -B $BUILD_DIR -G "MinGW Makefiles" `
            "-DCMAKE_C_COMPILER=$gcc" "-DCMAKE_CXX_COMPILER=$gxx" "-DCMAKE_BUILD_TYPE=$Config"
    }
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
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
        "-DARIAREAD_RUNTIME_CONFIG=$BUILD_DIR/AriaReadRuntimePaths.cmake" `
        -P (Join-Path $PROJECT_ROOT "cmake/SyncWebRuntime.cmake")
    if ($LASTEXITCODE -ne 0) { throw "Runtime asset sync failed" }
}
& $binary --help | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Runtime executable check failed" }
Write-Host "Ready: $RUNTIME_DIR"
Write-Host "Run: & '$binary'"
Write-Host "Distribute this directory with its libraries, web/ assets and licenses/. Review the runtime redistribution terms in THIRD_PARTY_NOTICES.md."
