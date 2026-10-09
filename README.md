# AriaRead

[依赖更新完整指南](docs/dependencies.md) — 版本固定、选择性更新、离线、回退与提交步骤。

当前版本 **0.3.1** · Aria **3.1.1**

📖 跨平台阅读引擎，专注于开源中文书源生态。

基于 [Aria](https://github.com/dqsjqian/Aria)（C++23 响应式 MVVM 框架）构建，兼容主流书源格式。

[English](README.en.md) | [简体中文](README.md)

## 界面截图

同一份 C++ 核心同时驱动两种 Web 形态：

| 形态 | 截图 | 说明 |
|---|---|---|
| REST + SSE 薄客户端 | ![AriaRead-Web](docs/marketing/images/AriaRead-Web.png) | 浏览器直连 C++ HTTP 壳，Property 变化经 SSE 推送 |
| SSR 服务端渲染 | ![AriaRead-SSR](docs/marketing/images/AriaRead-SSR.png) | 页面由 C++ 侧渲染，前端零 JS 依赖也能跑 |

## 特性

- **C++23 响应式 MVVM**：基于 Aria 框架，支持协程异步、响应式状态与 ViewModel
- **跨平台引擎**：CSS3/XPath 选择器、JS 运行时桥接、Gumbo HTML5 解析
- **按平台精简依赖**：锁定源码与校验缓存；Windows 使用系统 Schannel，不构建 OpenSSL；macOS 使用系统钥匙串验证证书
- **扩展 ViewModel 层**：SearchViewModel、BookshelfViewModel、ReaderViewModel、SourceViewModel
- **Engine Adapters**：engine_reader_adapter、engine_source_adapter，将 engine.h 的私有依赖隔离在 .cpp 内
- **C++ Web Server**：Mira（自研 C++23 协程网络库）+ Aria ViewModel，REST 路由，SSE 推送，零 Python 依赖

## 构建

当前稳定工具链基线（2026-10-02）：GCC **16.2.0**、LLVM Clang **23.1.2**、Xcode **27.0** / AppleClang **21.0**、Visual Studio 2026 stable / MSVC Build Tools **14.51.36247**。统一 C++23；配置时拒绝旧编译器，CI 不保留旧版兼容任务。

需要 CMake 3.21+、支持 C++23 的编译器、Git 和 Python 3.10+。先用构建所用的 Python 安装 `requirements-build.txt` 中固定的 AriaDeps；建议在虚拟环境中安装。统一入口负责取依赖、配置、并行编译和打包：

```bash
# macOS / Linux
python3 -m pip install -r requirements-build.txt
python3 scripts/build.py

# Windows：普通 PowerShell 即可，自动查找 Visual Studio C++ x64 工具链
python -m pip install -r requirements-build.txt
python scripts/build.py

# Windows：明确选择已经安装的 MSYS2 UCRT64 / MinGW 工具链
python scripts/build.py --toolchain mingw

# 开发验收：补齐测试依赖，构建并运行全部测试
python3 scripts/build.py --test
```

Windows 默认只需 Visual Studio C++ 工具链、Windows SDK、CMake、Git、Python；**不要求安装 MSYS2、Perl 或 OpenSSL**。两套编译器是可选方案，无须同时安装。MSVC/MinGW 的构建目录与依赖前缀分开，下载缓存共享。默认 TLS 后端是 Schannel；需对比旧后端时传 `--tls-backend openssl`，届时才需要 Perl 和相应 make 工具。

macOS 使用 Xcode Command Line Tools；OpenSSL 构建仍需系统 Perl/make。curl 通过 Apple SecTrust 使用系统钥匙串，不依赖 Homebrew 的 CA 文件。Linux 还需要 Perl/make 和系统 CA 证书。运行 Web 回归需要 Node.js 18+。

默认只构建运行时，跳过 doctest；`--test` 补齐测试。普通重跑复用已验证依赖，`--offline` 使用已有源码和下载缓存，`--jobs N` 限制并行数。`--build-dir` / `ARIAREAD_BUILD_DIR` 自定义构建目录，`--deps-prefix` / `ARIAREAD_DEPS_PREFIX` 自定义依赖前缀。

## 运行与分发

| 入口 | 默认运行目录 |
|---|---|
| macOS / Linux | `build/bin/` |
| Windows MSVC | `build/windows-msvc-release/bin/`（多配置生成器为 `bin/Release/`） |
| Windows MinGW | `build/windows-mingw-release/bin/` |

完成后入口打印实际可执行文件路径。运行目录包含服务程序、Aria 动态库、`web/` 和 `licenses/`，可以整体复制；开发时可用 `--web-root bindings/web/ariaread/web` 指定源码资源。

`scripts/build.py` 是全平台唯一构建入口。Windows 只支持 Release，提前拒绝不匹配的 Debug CRT；空 CMake build type 自动选择 Release。`--clean` 仅清理当前应用构建，不删除下载缓存或依赖前缀。

## 项目结构

```
AriaRead/
├── CMakeLists.txt          # C++23，全平台统一编译选项
├── include/                # 公共头文件
│   └── ariaread/
│       └── version.h.in    # 版本号模板
├── src/                    # 核心引擎源码
│   ├── engine/             # 引擎核心
│   ├── selector/           # CSS/XPath 选择器
│   ├── rule/               # 规则解析器
│   ├── infra/              # 基础设施（HTTP/JS/DB）
│   ├── viewmodels/         # ViewModel 层
│   └── apps/web_server/    # Web Server（Mira HTTP/1.1 + REST API）
├── scripts/build.py          # 跨平台统一构建入口
├── tools/_build/           # 构建入口回归
├── scripts/_recipes/         # 项目依赖配方与补丁
├── requirements-build.txt  # 固定的 AriaDeps 构建依赖
├── deps/                   # 可编辑源码：aria/、Mira/、curl/、zlib/……
├── build/                  # 应用产物、依赖安装前缀、归档和编译缓存
├── bindings/web/ariaread/web/  # 前端静态资源
└── tests/                  # 单元测试
```

## 依赖：平铺源码，统一入口

`dependencies.json` 保存初次获取的来源、版本和 SHA256；源码平铺在 `deps/<name>/`，目录名不含版本或提交号。已有源码允许自己 `git pull`、切换分支或编辑，普通构建使用实际内容；记录实际 Git HEAD 和内容指纹，变更会使该组件及其消费者重编。不会把手动更新的源码伪报为锁定版本。

```bash
python scripts/build.py deps                              # 只准备运行时依赖
python scripts/build.py deps --profile tests              # 补齐测试依赖
python scripts/build.py deps-check --profile tests        # 只读检查源码与安装结果
python scripts/build.py deps-update --only curl           # 更新版本记录，不立即替换源码
python scripts/build.py deps-update --only zlib --version zlib=1.3.2
python scripts/build.py --offline --test                   # 从已有来源离线构建并验收
python scripts/build.py test-tools                        # 构建工具自身的离线回归
```

依赖更新后运行普通构建。没有手动改动的旧源码可备份后替换；有本地改动则保留并明确报错，待用户处理。第三方补丁在编译快照上应用，不改 `deps/` 中的源码。不是每个上游目录都是 Git 仓库：归档发布的库可直接编辑，要换版本用 `deps-update`。

`--source-dir`、`--deps-prefix`、`--build-dir` 分别选择源码、安装库和应用产物目录；默认 `deps/` 不提交 Git。MSVC 与 MinGW 共用源码，二进制分开。CMake 配置期只验证现有安装及实际源码，不下载依赖。aria 与其他依赖完全一致：从 `dependencies.json` 取到 `deps/aria`，与其他库一起装进前缀，CMake 侧 `find_package(aria)`；上游保留其共享库（单例 ABI）形态，其余依赖均为静态库。

缓存按实际源码、配方、补丁、编译器/ABI 和安装文件哈希校验。成功安装的未变组件可以复用；失败回滚旧前缀。用户修改源码和用户修改已安装的二进制是两回事，后者仍会触发保护。中断重跑复用下载，不保证未提交编译工作的断点续建。

详见[依赖指南](docs/dependencies.md)和[构建与目录设计约定](docs/build-architecture.md)。所有平台、CI 和维护流程使用 `scripts/build.py`，`tools/_build/` 是内部实现，不是第二套用户入口。

## 测试

从仓库根目录配置、构建并运行测试：

```bash
python scripts/build.py --test --require-web-tests
```

默认使用 Release；Windows 配置阶段会拒绝不受支持的配置，避免链接时才出现 CRT 失配。推荐 `python3 scripts/build.py --test`，同时管理测试依赖和 CMake 选项。

CTest 会注册引擎、可用的 ViewModel 测试和本地依赖获取安全回归，并在找到 Node.js 18+、Python 3.10+ 与 Web Server 目标时注册相应的调试回归。缺少可选依赖时，CMake 会明确提示跳过；可用 `-DARIAREAD_BUILD_WEB_TESTS=OFF` 关闭 Web 回归。

```bash
# 仅运行调试回归
ctest --test-dir build -R ariaread-debug --output-on-failure

# 也可独立运行；无需安装 npm 或 pip 包
node tests/test_debug_ui.cjs
python3 tests/test_debug_http.py --server build/bin/ariaread_web_server
```

HTTP 测试只使用本地模拟书源和内存数据库，覆盖控制台输入校验与运行限制、SSE 成功及失败阶段、响应和 gzip 解压大小限制、端口占用保护。服务路径也可通过 `ARIAREAD_WEB_SERVER` 环境变量指定；使用其他构建目录或多配置生成器时，将路径改为对应的可执行文件。

## 开源准备

- [x] 源码全部从源码编译（无预编译二进制）
- [x] 无 git submodule；第三方依赖和 Aria 均由脚本锁定并获取
- [x] MIT LICENSE
- [x] README.md（中文）+ README.en.md（英文）

## 许可证

MIT License — 详见 [LICENSE](LICENSE)。

自有代码采用 MIT；第三方组件保留各自协议。分发说明见 [第三方许可声明](THIRD_PARTY_NOTICES.md)。
