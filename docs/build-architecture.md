# 构建精简与依赖分工

统一入口：`python tools/build.py`（macOS/Linux 可用 `python3`）。默认构建可运行的 Web 服务，`--test` 增加开发测试。依赖仍从 `dependencies.json` 的固定来源获取并校验，CMake 配置期不联网。

## 哪些依赖可以去掉

| 依赖 | 实际职责 | 本轮处理 |
|---|---|---|
| Mira | Web 服务端 HTTP/1.1、TCP、协程事件循环 | 保留；TLS、WebSocket、HTTP/2/3 关闭 |
| curl | 书源 HTTP/HTTPS、跳转、代理、压缩响应、超时 | 保留；仅编译 HTTP/HTTPS，去掉命令行工具和文档 |
| OpenSSL | curl 的 TLS 实现 | Windows 默认移除，用系统 Schannel；macOS/Linux 保留 |
| zlib | gzip/deflate 响应解压 | 保留，Mira 不替代内容解压 |
| Gumbo | 容错 HTML5 解析、CSS/XPath 选择器底层 | 保留，与网络库职责无关 |
| QuickJS | 书源 JavaScript 规则 | 保留核心引擎，去掉未使用的 POSIX `os/std` 辅助模块 |
| SQLite、sqlite_modern_cpp | 本地书架、阅读进度、书源存储 | 保留数据库与轻量头文件封装 |
| nlohmann/json | 规则和接口 JSON 编解码 | 保留，头文件库 |
| doctest | 单元测试 | 仅 tests profile 需要，runtime profile 跳过 |
| Aria | 响应式状态、ViewModel 和共享运行时 | 保留，关闭其独立测试/示例/HTTP adapter |

## Mira 能否替代 curl

当前锁定的 Mira 1.0.0 已能完成 HTTP/HTTPS 传输，但它的公开契约把重定向、内容解压和代理策略留给上层。其 TLS 模块仍依赖 OpenSSL；改用 Mira HTTPS 并不会顺带去掉 OpenSSL 和 zlib。AriaRead 目前通过请求头传递 Cookie，并未启用 curl cookie jar，因此 Cookie 不是保留 curl 的理由。整体替换仍需补齐并回归跳转语义、代理、字符集转换、响应限额和平台证书信任。

代码依据：Mira `modules/http/include/mira/http/client.hpp`、`serializer.hpp`、`modules/tls/include/mira/tls/context.hpp`；AriaRead `src/infra/http_client_curl.cpp`、`include/ariaread/http_client.h`。本轮把 Web 远程书源导入收敛到同一 `HttpClient`，Web 层不再直接链接或调用 curl。后续如果迁移，只需替换这一个传输实现。

## 平台和工具链

- Windows 默认 MSVC x64，自动查找已安装 Visual Studio 的开发环境。无需同时安装 MSYS2；显式 `--toolchain mingw` 使用已安装的 UCRT64/MinGW。两者的 CMake 输出和安装前缀隔离，源码下载共享。
- Windows 默认 curl Schannel，使用系统证书存储和撤销检查，不构建 OpenSSL，也无需其 Perl/nmake/NASM 前置工具。协议能力随操作系统：Windows 10 可用 TLS 1.2，Windows 11/Server 2022 起支持 TLS 1.3。`--tls-backend openssl` 是显式兼容选项。
- macOS 保留 OpenSSL TLS，但启用 Apple SecTrust 验证系统钥匙串，避免写死 Homebrew CA 路径。当前 curl 已移除 Secure Transport，不能通过这个旧后端直接删除 OpenSSL。
- Linux 保留 OpenSSL 和系统 CA 证书；CI 使用固定的 `gcc:16.2.0-trixie` 环境，运行完整 C++、Web 和 HTTP/TLS 回归。首次构建仍需 Perl/make，后续复用组件缓存。
- 默认 Release。Windows 依赖 SDK 仅提供 Release /MD；CMake 在配置时限制配置，避免混用 /MDd。应用清理不删除依赖缓存。

2026-10-02 的稳定编译器基线为 GCC 16.2.0、LLVM Clang 23.1.2、Xcode 27.0 / AppleClang 21.0、Visual Studio 2026 / MSVC 14.51.36247（CMake 识别为 19.51.36247）。统一 C++23，不再保留旧编译器兼容任务。版本要求集中在 `cmake/CompilerRequirements.cmake`；后续升级应同时更新入口的 MSVC 稳定系列选择与 CI 环境。

上游依据：[curl CMake 构建选项](https://github.com/curl/curl/blob/curl-8_22_0/docs/INSTALL-CMAKE.md)、[系统证书信任](https://curl.se/docs/sslcerts.html)、[Secure Transport 移除记录](https://curl.se/ch/8.15.0.html)。

## 增量缓存

缓存按组件记录固定源码、配方、相关补丁、编译环境、依赖闭包和安装文件哈希。更新 json 不再导致 OpenSSL 重编；更新 zlib 会使依赖它的 curl 失效。编译器/ABI 环境改变会使全部二进制失效。

安装仍是事务：先准备精确源码，再保留旧前缀，从已验证组件恢复未变内容，只重建受影响部分。失败恢复旧前缀；本地修改、缺失文件、来源哈希错误均不能冒充命中。旧收据缺少组件归属信息，需要首次重建一次，不能盲信迁移。

GitHub Actions 缓存源码归档、Git checkout 和安装前缀；键包含实际编译器/SDK 环境，恢复后由构建器再次校验。不会把可达数 GB 的 `runs/` 编译中间目录加入缓存。跨机器缓存只在路径、平台、编译器与 ABI 校验均匹配时复用，不宣称是任意平台通用 SDK。

`--offline` 仅使用本地已有的精确锁定源码；缓存缺失时明确失败。`--profile runtime` 与 `--profile tests` 是底层依赖构建选项，应用入口根据 `--test` 自动选择。升级、回退及显式版本选择继续使用[依赖指南](dependencies.md)。
