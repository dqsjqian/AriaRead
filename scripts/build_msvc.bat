@echo off
rem Build AriaRead web server with VS2026 MSVC toolchain (self-contained).
rem vcvarsall.bat is NOT used: it calls reg.exe which is blocked by sandbox.
rem Ninja generator is used instead of "Visual Studio 18 2026": MSBuild throws
rem MSB6001 (duplicate "Path"/"PATH" env keys injected by the host shell chain).
rem Environment is assembled manually below (equivalent to vcvarsall x64).
rem Usage: build_msvc.bat [configure|build|clean]
setlocal enabledelayedexpansion

rem ---- locate toolchain ----
set "VSDIR=D:\VS2026\IDE"
set "KITSDIR=D:\Windows Kits\10"
set SDKVER=10.0.26100.0
set VCTOOLS=
for /d %%D in ("%VSDIR%\VC\Tools\MSVC\*") do set "VCTOOLS=%%D"
if "%VCTOOLS%"=="" (echo [error] MSVC tools dir not found & exit /b 1)
for /f "delims=" %%V in ("%VCTOOLS%") do set "VCTOOLVER=%%~nxV"
set "NINJA=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
rem OpenSSL's Configure step needs perl. cmake/BuildOpenSSL.cmake auto-detects
rem MSYS2 perl; on a plain MSVC toolchain find_program(perl) needs one on PATH.
rem Set ARIAREAD_PERL_DIR to override; otherwise probe the usual locations.
set "PERLDIR="
if defined ARIAREAD_PERL_DIR if exist "%ARIAREAD_PERL_DIR%\perl.exe" set "PERLDIR=%ARIAREAD_PERL_DIR%"
for %%P in (
  "C:\Strawberry\perl\bin"
  "%ProgramFiles%\Strawberry\perl\bin"
  "%ProgramFiles%\Git\usr\bin"
  "%ProgramFiles(x86)%\Git\usr\bin"
) do if not defined PERLDIR if exist "%%~P\perl.exe" set "PERLDIR=%%~P"
if defined PERLDIR (echo [info] perl found at %PERLDIR%) else (echo [warn] perl not found; OpenSSL Configure may fail)

rem ---- assemble MSVC x64 environment ----
set "PATH=%VCTOOLS%\bin\Hostx64\x64;%KITSDIR%\bin\%SDKVER%\x64;%NINJA%;%VSDIR%\Common7\IDE;%VSDIR%\Common7\Tools;C:\Program Files\CMake\bin;C:\Program Files\Git\usr\bin;%PATH%"
if defined PERLDIR set "PATH=%PERLDIR%;%PATH%"
set "INCLUDE=%VCTOOLS%\include;%KITSDIR%\Include\%SDKVER%\ucrt;%KITSDIR%\Include\%SDKVER%\um;%KITSDIR%\Include\%SDKVER%\shared;%KITSDIR%\Include\%SDKVER%\winrt;%KITSDIR%\Include\%SDKVER%\cppwinrt"
set "LIB=%VCTOOLS%\lib\x64;%KITSDIR%\Lib\%SDKVER%\ucrt\x64;%KITSDIR%\Lib\%SDKVER%\um\x64"
set "WindowsSdkDir=%KITSDIR%\"
set "WindowsSDKVersion=%SDKVER%\"
set "VCToolsInstallDir=%VCTOOLS%\"

set PROJECT_ROOT=D:\Learning\AriaRead
set BUILD_DIR=%PROJECT_ROOT%\build
set STEP=%1
if "%STEP%"=="" set STEP=all

cd /d "%PROJECT_ROOT%" || exit /b 1

where cl >nul 2>&1 || (echo [error] cl.exe not on PATH & exit /b 1)
where ninja >nul 2>&1 || (echo [error] ninja.exe not on PATH & exit /b 1)
echo [env] MSVC %VCTOOLVER% / SDK %SDKVER% / Ninja + cl 19.51

rem Pinned dependency prefix: built once by tools\ci\build_ariaread_deps.py
rem (downloads + SHA256-verified builds; ~40 min on first run because of
rem OpenSSL). Configure itself never touches the network.
if not exist "%PROJECT_ROOT%\build\deps\prefix\share\ariaread-deps\manifest.json" (
    echo [deps] pinned dependency prefix missing - building it now ^(first run only^) ...
    where python >nul 2>&1 || (echo [error] python not on PATH; run: python tools\ci\build_ariaread_deps.py & exit /b 1)
    python tools\ci\build_ariaread_deps.py || exit /b 1
)

if "%STEP%"=="clean" (
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
    echo [clean] removed %BUILD_DIR%
    exit /b 0
)

if not exist "%BUILD_DIR%\build.ninja" (
    echo [configure] Ninja + cl ...
    cmake -S . -B build -G Ninja ^
        -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ^
        -DCMAKE_BUILD_TYPE=Release ^
        -DCMAKE_MSVC_RUNTIME_LIBRARY="MultiThreaded" ^
        -DARIAREAD_ENFORCE_SELF_CONTAINED=ON ^
        -DARIAREAD_USE_SYSTEM_CURL=OFF || exit /b 1
)
if "%STEP%"=="configure" exit /b 0

echo [build] ariaread_web_server Release ...
cmake --build build --target ariaread_web_server --parallel %NUMBER_OF_PROCESSORS% || exit /b 1

set RUNTIME=%BUILD_DIR%\bin
if not exist "%RUNTIME%\ariaread_web_server.exe" (echo [error] binary missing & exit /b 1)
echo [run] %RUNTIME%\ariaread_web_server.exe --help
"%RUNTIME%\ariaread_web_server.exe" --help || exit /b 1
echo [ok] Ready: %RUNTIME%
endlocal
