# AriaRead macOS 接入

目前没有独立的原生应用壳，可运行的应用入口是 Web 服务。准备 **Xcode 27.0** 或对应 Command Line Tools（**Apple Clang 21+**）、Git、Python 3.10+ 和 CMake 3.21+，从仓库根目录运行：

```bash
python3 tools/build.py
```

默认只构建运行 Web 服务需要的依赖与目标，测试框架通过 `--test` 按需启用。macOS 的 HTTPS 使用 libcurl + OpenSSL；首次构建仍需获取并编译锁定的第三方依赖，后续构建会验证并复用缓存。应用保持使用 `build`，依赖安装目录保持使用 `build/deps/prefix`。

入口会先用临时 CMake 工程检查编译器，复用根工程的版本要求与所选编译参数；检查失败时不会获取或构建依赖。

```bash
# 构建全部测试目标并执行 CTest
python3 tools/build.py --test

# 同时要求 Web 测试依赖（包括 Node.js）齐全
python3 tools/build.py --test --require-web-tests

# 已具备所需源码、归档或安装缓存时离线构建
python3 tools/build.py --offline

# 清理并重建应用；依赖安装、下载和源码缓存保留
python3 tools/build.py --clean

# 仅更新已有运行目录的资源与许可证
python3 tools/build.py --skip-cmake
```

可用 `--jobs` 控制并行度、`--build-dir` 和 `--deps-prefix` 覆盖路径。若通过 `CC/CXX` 选用其他桌面编译器，最低要求为 **LLVM Clang 23.1.2** 或 **GCC 16.2**。切换编译器或 CMake 生成器时使用新的构建目录；脚本会拒绝混用已有 CMake 缓存。同一编译器的参数也会同步给依赖构建：未设置 `CC/CXX` 时恢复缓存参数，显式设置裸编译器路径时清除旧参数。

默认运行 `build/bin/ariaread_web_server`；多配置生成器的具体路径以构建输出为准。分发时保留整个运行目录，包括动态库、`web/` 和 `licenses/`。完整选项见 `python3 tools/build.py --help` 与[构建架构说明](../../docs/build-architecture.md)。

## 原生界面接入

- [C API](../../include/ariaread/bridge.h)：面向 Swift、Objective-C、C 和其他 FFI 调用方，返回字符串须用 `ariaread_free_string` 释放。
- [C++ 包装](../../include/ariaread/wrapper.hpp)：类型为 `ariaread::EngineWrapper`。
- [ViewModel](../../src/viewmodels/include/ariaread/vm/)：可供界面层复用的书架、阅读、搜索与书源状态。

这些接口由当前工程构建；本目录不提供已打包的原生应用或 SDK。

## macOS 验证与分发范围

本轮 GCC 16.2 在 macOS 上独立全量通过 176/176 测试，但 `otool` 显示产物仍链接 `/opt/homebrew/opt/gcc/lib/gcc/current/libstdc++.6.dylib`。这组 GNU 构建用于源码与编译器回归；目标系统需要提供兼容的编译器运行库。

可搬移的 macOS 分发使用默认 Apple Clang 构建。本轮已把该运行目录迁移到临时位置，HTTP 与 Web 验证通过，动态库引用中没有开发机路径。
