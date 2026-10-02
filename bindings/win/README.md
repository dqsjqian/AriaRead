# AriaRead Windows 接入

目前没有独立的原生应用壳，可运行的应用入口是 Web 服务。从仓库根目录使用统一入口：

```powershell
python tools/build.py
```

默认使用 **MSVC x64 + Ninja + Schannel**。准备 Git、Python 3.10+，以及 Visual Studio 2026 / Build Tools **18.10.3 稳定版**的“使用 C++ 的桌面开发”、Windows SDK 和 C++ CMake 工具；MSVC 要求 **19.51.36247+**（14.51 工具集）。也可单独安装 CMake 3.21+ 和 Ninja。普通 PowerShell 或命令提示符可直接执行，脚本会查找 VS 并配置 x64 编译环境，默认流程不需要安装 MSYS2、OpenSSL 或 Perl。

工具集优先遵循 VS 的 `Microsoft.VCToolsVersion.default.txt`；文件不存在时只在稳定版 14.51 系列中选择。并排安装的 14.52 预览工具集不会自动启用。若已有 Developer Prompt 选择了预览版，请使用稳定版 x64 提示符或普通终端；默认版本文件无效时请修复 VS 的 C++ 工作负载。

入口会先用临时 CMake 工程检查编译器，复用根工程的版本要求与所选编译参数；检查失败时不会获取或构建依赖。

Schannel 是 Windows 自带的 TLS 实现，由 libcurl 用来处理 HTTPS 和系统证书信任；它不会替代 HTTP、HTML 解析或 JavaScript 等其他能力。需要 OpenSSL 时可显式传入 `--tls-backend openssl`，此时源码构建 OpenSSL 还需要原生 Windows Perl（例如 Strawberry Perl）。

```powershell
# 构建全部测试目标并执行 CTest；要求 Web 测试依赖（包括 Node.js）齐全
python tools/build.py --test --require-web-tests

# 使用已安装的 Visual Studio 生成器（名称以 cmake --help 为准）
python tools/build.py --generator "Visual Studio 18 2026"

# MSYS2 UCRT64 是显式备选，须准备 GCC/G++ 16.2+、CMake 和 Ninja
python tools/build.py --toolchain mingw

# 只清理应用构建产物，保留依赖安装、下载和源码缓存
python tools/build.py --clean-only
```

默认 MSVC 构建目录为 `build/windows-msvc-release`，依赖安装在 `build/deps/windows-msvc/prefix`；MinGW 对应 `build/windows-mingw-release` 和 `build/deps/windows-mingw/prefix`。两种工具链不能共用同一个 CMake 构建目录或依赖安装目录。`--build-dir`、`--deps-prefix` 可分别覆盖路径；非标准安装可使用 `ARIAREAD_VS_ROOT`、`ARIAREAD_WINDOWS_KITS_ROOT` 或 `MSYS2_ROOT`。

Windows 当前仅支持 `--config Release`，保证应用与依赖使用一致的 CRT。旧入口 `scripts/build_msvc.bat` 和 `scripts/build_web_release.ps1` 均委托给同一 Python 入口。构建完成后按输出路径运行 `ariaread_web_server.exe`；分发时保留整个运行目录，包括 DLL、`web/` 和 `licenses/`。完整选项见 `python tools/build.py --help` 与[构建架构说明](../../docs/build-architecture.md)。

## 原生界面接入

- [C API](../../include/ariaread/bridge.h)：面向 Swift、Objective-C、C 和其他 FFI 调用方，返回字符串须用 `ariaread_free_string` 释放。
- [C++ 包装](../../include/ariaread/wrapper.hpp)：类型为 `ariaread::EngineWrapper`。
- [ViewModel](../../src/viewmodels/include/ariaread/vm/)：可供界面层复用的书架、阅读、搜索与书源状态。

这些接口由当前工程构建；本目录不提供已打包的原生应用或 SDK。
