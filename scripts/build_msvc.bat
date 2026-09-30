@echo off
rem Build AriaRead web server with the local MSVC toolchain (self-contained).
rem Avoid vcvarsall.bat for restricted environments that block reg.exe.
rem Ninja generator is used instead of "Visual Studio 18 2026": MSBuild throws
rem MSB6001 (duplicate "Path"/"PATH" env keys injected by the host shell chain).
rem Environment is assembled manually below (equivalent to vcvarsall x64).
rem Toolchain locations are probed dynamically (vswhere first); every hardcoded
rem path is only a fallback candidate, never a requirement.
rem Usage: build_msvc.bat [configure|build|clean]
setlocal enabledelayedexpansion

rem Repo root = the directory that contains this scripts\ folder.
set "PROJECT_ROOT=%~dp0.."
set "BUILD_DIR=%PROJECT_ROOT%\build"
set "STEP=%1"
if "%STEP%"=="" set STEP=all

if "%STEP%"=="clean" (
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
    echo [clean] removed %BUILD_DIR%
    exit /b 0
)

rem ---- locate toolchain ----
set "VSDIR="
if defined ARIAREAD_VS_ROOT if exist "%ARIAREAD_VS_ROOT%\VC\Tools\MSVC" set "VSDIR=%ARIAREAD_VS_ROOT%"
if not defined VSDIR if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
    for /f "usebackq delims=" %%P in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do if not defined VSDIR set "VSDIR=%%P"
)
if not defined VSDIR if exist "C:\Program Files\Microsoft Visual Studio\VC\Tools\MSVC" set "VSDIR=C:\Program Files\Microsoft Visual Studio"
if not defined VSDIR if exist "C:\Program Files (x86)\Microsoft Visual Studio\VC\Tools\MSVC" set "VSDIR=C:\Program Files (x86)\Microsoft Visual Studio"
if "%VSDIR%"=="" (echo [error] Visual Studio installation not found; set ARIAREAD_VS_ROOT & exit /b 1)

set "KITSDIR=%ARIAREAD_WINDOWS_KITS_ROOT%"
if not defined KITSDIR set "KITSDIR=%WindowsSdkDir%"
if not defined KITSDIR set "KITSDIR=%ProgramFiles(x86)%\Windows Kits\10"
if not exist "%KITSDIR%" (echo [error] Windows Kits directory not found & exit /b 1)

rem Highest SDK version directory under Include\ (dir /b sorts by name).
set "SDKVER="
for /f "delims=" %%V in ('dir /b /ad "%KITSDIR%\Include" 2^>nul ^| findstr /r "^[0-9][0-9]*\."') do set "SDKVER=%%V"
if "%SDKVER%"=="" (echo [error] no Windows SDK found under %KITSDIR%\Include & exit /b 1)

set "VCTOOLS="
for /d %%D in ("%VSDIR%\VC\Tools\MSVC\*") do set "VCTOOLS=%%D"
if "%VCTOOLS%"=="" (echo [error] MSVC tools dir not found & exit /b 1)
for /f "delims=" %%V in ("%VCTOOLS%") do set "VCTOOLVER=%%~nxV"
set "NINJA=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
rem OpenSSL's Configure step needs perl. cmake/BuildOpenSSL.cmake auto-detects
rem MSYS2 perl; on a plain MSVC toolchain find_program(perl) needs one on PATH.
rem Only a native Windows perl works (cygwin/perl5 msys perl is rejected by
rem Configure). Set ARIAREAD_PERL_DIR to override; otherwise probe the usual
rem locations, native MSYS2 ucrt64 first.
set "PERLDIR="
if defined ARIAREAD_PERL_DIR if exist "%ARIAREAD_PERL_DIR%\perl.exe" set "PERLDIR=%ARIAREAD_PERL_DIR%"
for %%P in (
  "%MSYS2_ROOT%\ucrt64\bin"
  "C:\msys64\ucrt64\bin"
  "C:\Strawberry\perl\bin"
  "%ProgramFiles%\Strawberry\perl\bin"
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

cd /d "%PROJECT_ROOT%" || exit /b 1

where cl >nul 2>&1 || (echo [error] cl.exe not on PATH & exit /b 1)
where ninja >nul 2>&1 || (echo [error] ninja.exe not on PATH & exit /b 1)
echo [env] MSVC %VCTOOLVER% / SDK %SDKVER% / Ninja + cl

rem Validate the pinned Aria checkout on every build; ARIA_SOURCE may select a local source.
where python >nul 2>&1 || (echo [error] python not on PATH & exit /b 1)
python tools\ci\fetch_aria.py || exit /b 1

rem Verify the lock, compiler, patches and installed contents on every build.
python tools\ci\build_ariaread_deps.py || exit /b 1

rem Always (re)configure: the pinned deps prefix ships Release-only static
rem libs, so the cached build type must be Release. A stale cache (Debug, or
rem an aborted configure) used to compile /MDd objects against /MD libraries
rem and died in LNK2038 _ITERATOR_DEBUG_LEVEL mismatches. Re-running cmake
rem overrides the cached type in place and rebuilds only what the change
rem touches.
echo [configure] Ninja + cl (Release) ...
rem CRT stays the repo default (dynamic /MD): the shared aria DLLs and the
rem exe must share one CRT heap; static /MT re-triggers the known
rem cross-module heap corruption documented in .github/workflows/ci.yml.
cmake -S . -B build -G Ninja ^
    -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ^
    -DCMAKE_BUILD_TYPE=Release || exit /b 1
if "%STEP%"=="configure" exit /b 0

echo [build] ariaread_web_server Release ...
cmake --build build --target ariaread_web_server --parallel %NUMBER_OF_PROCESSORS% || exit /b 1

set RUNTIME=%BUILD_DIR%\bin
if not exist "%RUNTIME%\ariaread_web_server.exe" (echo [error] binary missing & exit /b 1)
echo [run] %RUNTIME%\ariaread_web_server.exe --help
"%RUNTIME%\ariaread_web_server.exe" --help || exit /b 1
echo [ok] Ready: %RUNTIME%
endlocal
