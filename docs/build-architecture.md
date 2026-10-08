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

`--offline` 使用已有源码工作区或精确来源缓存，缺失时明确失败。`deps --profile runtime` 与 `deps --profile tests` 控制依赖集合，应用入口根据 `--test` 自动选择。升级、回退及显式版本选择继续使用[依赖指南](dependencies.md)。

## 缓存目录与恢复边界

源码平铺在仓库根目录 `deps/<name>/`。版本选择属于 `dependencies.json` 的元数据，不属于目录名。Git 来源允许用户自行 `pull`、切分支和编辑；归档来源同样可以编辑。普通构建采纳实际源码，不自动 reset，也不会因为 HEAD 与初次锁不同就拒绝构建。当前 HEAD 和内容指纹进入安装记录；源码变化使本组件及受影响消费者失效。锁选择变更后，未修改过的目录自动置换为新版本（旧目录备份进 `deps/.ariaread-sources/backups/`），有本地修改的目录保留并警告"新选择未应用"；显式更新（`--version` / `deps-update`）遇到本地修改则明确失败，不静默跳过。

`deps/.ariaread-sources/` 保存工作区管理信息；`build/deps/cache/` 保存经 SHA256 验证的归档，`build/deps/runs/` 保存隔离的编译快照。补丁仅修改快照。安装前缀为 `build/deps/prefix/`，Windows 为 `build/deps/windows-msvc/prefix/` 或 `windows-mingw/prefix/`。迁移旧 Git/Aria 缓存时保留原目录，避免破坏尚在使用旧路径的构建。

当前复用的是此前成功安装、且文件校验通过的组件。普通失败或可捕获的中断会恢复旧前缀；本次尚未提交的编译工作不会断点续建。强制终止、断电可能留下 `prefix.install-lock` 和未完成安装，需要确认没有仍在写入的进程，并核对备份和安装收据后恢复，不能仅按锁文件年龄或 PID 删除。

后续恢复设计应同时引入进程退出自动释放的操作系统锁和持久事务记录。缓存清理应先提供预览，保护活跃事务、最近可回滚备份、失败诊断和离线重建所需来源；当前不会自动清除 `runs/`、`prefix-backup-*` 或 `prefix-failed-*`。以上是待实现能力，并非现有断点恢复或垃圾回收保证。

macOS CI 使用提供正式 Xcode 27.0 的 [`xcode-27` 镜像](https://github.com/actions/runner-images/blob/main/images/macos/xcode-27-arm64-Readme.md)，并显式选择 `27.0`。镜像目前处于 public preview，编译器仍使用正式版；`macos-26` 镜像中的 Xcode 26.x 不满足本项目基线。

## 供其他项目参考的构建约定

唯一用户入口是 `python tools/build.py`，不要求用户在多个 shell 脚本之间选择。省略命令等价于 `build`，已有 `--test`、`--clean` 等选项继续有效。通用流水线来自 `requirements-build.txt` 固定的 AriaDeps 包，负责锁解析、源码工作区、组件缓存、事务安装与校验；`tools/_recipes/` 保存 Read 专用配方和补丁，`tools/_build/` 保留构建入口回归。构建入口显式将当前 Python 路径传给 CMake，保证配置期校验使用已安装同一包的解释器。

| 命令 | 职责 |
|---|---|
| `build`（默认） | 准备依赖、配置、编译、整理运行目录；`--test` 追加完整验收 |
| `deps` | 只准备依赖工作区和安装前缀 |
| `deps-check` | 只读验证源码与已安装组件是否匹配 |
| `deps-update` | 原子更新版本记录，后续构建应用选择 |
| `test-tools` | 离线运行构建工具、来源、迁移和缓存回归 |
| `cache-key` | 输出 CI 缓存标识，不能替代恢复后的内容校验 |

CMake 在配置期调用同一入口的 `deps-check`（带编译器桥接参数）完成只读校验，不访问网络，也不自建另一套版本选择逻辑。CI 调用与用户相同的入口，显式选择工具链，并通过 `--require-web-tests` 阻止静默漏测。

目录各自只有一个职责：`deps/` 是用户可编辑源码，`build/` 是机器生成内容，运行目录 `bin/` 是分发单位。`--source-dir`、`--deps-prefix`、`--build-dir` 分别覆盖这三类开发路径。应用清理不能删除源码、下载缓存或依赖前缀；跨工具链不能复用同一 CMake cache 或二进制前缀。

输出协议为 UTF-8：Python 父子进程和重定向日志统一编码；Windows 命令期间切换控制台编码，失败也恢复原设置；MSVC 诊断使用英文以避免本地代码页歧义。doctest 的测试名发现和 suite 标签发现都显式 `ENCODING UTF-8`，并以真实 CMake 模块、中文输出和生成的 CTest 用例验证，不能只检查终端外观。

推广前必须在 Read 的 Windows MSVC、Windows MinGW、Linux GCC、macOS Xcode 四个实际环境完成构建和测试。源码编辑导致失效、干净迁移、离线复用、失败回滚和中文失败诊断均属于构建接口的验收内容；在 Read 未通过前，不批量复制到其他仓库。
