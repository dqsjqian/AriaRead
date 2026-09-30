# AriaRead

[依赖更新完整指南](docs/dependencies.md) — 版本固定、选择性更新、离线、回退与提交步骤。

当前版本 **0.2.2** · Aria **3.1.0**

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
- **自包含第三方依赖**：Mira/OpenSSL/libcurl 等由脚本按依赖文件版本取源码编译，零系统依赖
- **扩展 ViewModel 层**：SearchViewModel、BookshelfViewModel、ReaderViewModel、SourceViewModel
- **Engine Adapters**：engine_reader_adapter、engine_source_adapter，将 engine.h 的私有依赖隔离在 .cpp 内
- **C++ Web Server**：Mira（自研 C++23 协程网络库）+ Aria ViewModel，39 条 REST 路由，SSE 推送，零 Python 依赖

## 构建

需要 CMake 3.21+、支持 C++23 的编译器和 Python 3.10+。从仓库根目录执行：

```bash
python3 tools/ci/build_ariaread_deps.py     # 按依赖文件取依赖（首次发现最新稳定版，只写 build/deps）
python3 tools/ci/fetch_aria.py             # 取回并验证锁定的 Aria 提交
cmake -S . -B build -DARIAREAD_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

## 运行与分发

构建和分发统一使用 `build/bin/`，服务构建目标会自动同步前端资源：

```bash
cmake --build build --target ariaread_web_server
./build/bin/ariaread_web_server

# 原一键打包命令仍可用，产物也在 build/bin
bash scripts/build_web_release.sh
```

`build/bin/` 包含 `ariaread_web_server`、Aria 动态库和 `web/`，可整体复制到其他目录运行。默认读取程序旁的 `web/`；开发时可用 `--web-root bindings/web/ariaread/web` 显式指定源码资源。

Windows 使用 `scripts/build_web_release.ps1`，运行 `build/bin/ariaread_web_server.exe`；多配置生成器使用 `build/bin/Release/`。Windows 固定依赖前缀仅提供 Release 库，脚本只接受 `-Config Release`。`ARIAREAD_BUILD_DIR` 可指定其他构建目录。

Windows 非标准工具链位置可通过 `MSYS2_ROOT`、`ARIAREAD_VS_ROOT`、`ARIAREAD_WINDOWS_KITS_ROOT` 指定。`scripts/build_msvc.bat` 是专用 MSVC 入口，同样会校验 Aria 锁定提交。

项目不再使用 `release/` 目录；启动与分发均使用上述构建产物。

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
├── tools/ci/
│   ├── build_ariaread_deps.py  # 第三方依赖唯一来源：版本锁 + SHA256（见下）
│   └── fetch_aria.py           # Aria 依赖文件指定的完整提交取回脚本 -> build/deps/aria
├── bindings/web/ariaread/web/  # 前端静态资源
└── tests/                  # 单元测试
```

## 依赖：一条路，不由 CMake 联网

所有第三方库（Mira / OpenSSL / libcurl / zlib / nlohmann_json / SQLite3 /
QuickJS / Gumbo / doctest / sqlite_modern_cpp）统一保存在 `dependencies.json`：
来源字段与可选 `version` 表示要求，自动生成的 `resolved` 记录实际版本、不可变提交和归档 SHA256。首次解析选最新稳定版，
后续普通构建复用锁；显式版本优先。归档校验、编译产物和许可证只写进 `build/deps/`。CMake 只做 `find_package`，
配置时不联网、没有 vendored 回退分支。Aria（兄弟框架）同理：由
`tools/ci/fetch_aria.py` 以依赖文件指定的完整提交取到 `build/deps/aria`，无 submodule。
脚本每次检查实际 Git HEAD 和工作树，拒绝覆盖本地修改；更新成功后将旧目录保留为
`build/deps/aria-backup-*`。尚未发布的同一提交可用
`python3 tools/ci/fetch_aria.py --source /path/to/Aria`（或 `ARIA_SOURCE` 环境变量）取回。
本地联调也可在 CMake 配置时显式指定 `-DARIA_DIR=/path/to/Aria`。

```bash
python3 tools/ci/build_ariaread_deps.py            # 首次构建（约 10 分钟，主要是 OpenSSL）
python3 tools/ci/fetch_aria.py                     # 取回 Aria（依赖文件指定的完整提交，无 submodule）
cmake -S . -B build -DARIAREAD_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release      # 自动探测 build/deps/prefix
cmake --build build
ctest --test-dir build --output-on-failure
```

自定义安装位置用脚本 `--prefix <prefix>`，再传 `-DARIAREAD_DEPS_PREFIX=<prefix>` 给 CMake。

```bash
python3 tools/ci/build_ariaread_deps.py --update                 # 主动更新最新稳定版并重建
python3 tools/ci/build_ariaread_deps.py --version zlib=1.3.2      # 显式版本优先
python3 tools/ci/build_ariaread_deps.py --offline                # 精确锁定的离线缓存
python3 tools/ci/build_ariaread_deps.py --only zlib,json          # 部分依赖及所需前置依赖
```

安装缓存绑定依赖文件、构建配方/补丁、编译器、Release 配置和每个安装文件的内容。
版本或配置变化会保留 `prefix-backup-*` 并从干净前缀重建；失败时恢复旧前缀，
保留 `prefix-failed-*` 供排查。已安装文件或 Git 缓存有本地修改时拒绝覆盖。
`--offline` 不发现新版本；离线重建需要依赖文件中的归档/Git 提交已缓存。
部分安装不能用于完整项目配置，补齐后才通过 CMake 的离线校验。
QuickJS、Gumbo、sqlite_modern_cpp 的本地补丁仅应用到已审核版本；上游新版本需要先适配配方，
脚本会明确报错，避免旧补丁静默套到新源码。Windows 支持 MSVC/nmake 和 MinGW。

## 测试

从仓库根目录配置、构建并运行测试：

```bash
cmake -S . -B build -DARIAREAD_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

主工程 configure 必须显式 `-DCMAKE_BUILD_TYPE=Release`：deps（Mira/OpenSSL/libcurl）
按 Release /MD 构建，空 build type 会编出 /MDd 目标，链接 `ariaread_web_server` 时
LNK2038 运行库失配。

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
