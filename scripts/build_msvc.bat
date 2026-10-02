@echo off
rem Compatibility entry point: build_msvc.bat [configure^|build^|clean]
rem clean removes application objects via CMake; downloads/dependencies are retained.
setlocal
set "STEP=%~1"
set "BUILD_FLAGS="
if "%STEP%"=="" goto run
if /i "%STEP%"=="all" goto run
if /i "%STEP%"=="build" goto run
if /i "%STEP%"=="configure" set "BUILD_FLAGS=--configure-only"
if /i "%STEP%"=="configure" goto run
if /i "%STEP%"=="clean" set "BUILD_FLAGS=--clean-only"
if /i "%STEP%"=="clean" goto run
 echo [error] Usage: build_msvc.bat [configure^|build^|clean]
exit /b 1
:run
where py >nul 2>&1
if errorlevel 1 goto python
py -3 "%~dp0..\tools\build.py" --toolchain msvc %BUILD_FLAGS%
exit /b %ERRORLEVEL%
:python
python "%~dp0..\tools\build.py" --toolchain msvc %BUILD_FLAGS%
exit /b %ERRORLEVEL%
