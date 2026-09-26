# AriaRead

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
- **自包含第三方依赖**：Continuo/OpenSSL/libcurl 等由脚本按固定版本取源码编译，零系统依赖
- **扩展 ViewModel 层**：SearchViewModel、BookshelfViewModel、ReaderViewModel、SourceViewModel
- **Engine Adapters**：engine_reader_adapter、engine_source_adapter，将 engine.h 的私有依赖隔离在 .cpp 内
- **C++ Web Server**：Continuo（自研 C++23 协程网络库）+ Aria ViewModel，39 条 REST 路由，SSE 推送，零 Python 依赖

## 构建

```bash
python3 tools/ci/build_ariaread_deps.py     # 取固定版本依赖（只写 build/deps）
mkdir -p build && cd build
cmake .. -DARIAREAD_BUILD_TESTS=ON
cmake --build .
ctest --output-on-failure
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

Windows 使用 `scripts/build_web_release.ps1`，运行 `build/bin/ariaread_web_server.exe`。多配置生成器使用 `build/bin/Release/`（或所选配置），脚本可传 `--config Debug` / `-Config Debug`。`ARIAREAD_BUILD_DIR` 可指定其他构建目录。

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
│   └── apps/web_server/    # Web Server（Continuo HTTP/1.1 + REST API）
├── third_party/
│   └── aria/               # Aria C++23 协程 MVVM 框架（git submodule，四库源码联动）
├── tools/ci/
│   └── build_ariaread_deps.py  # 依赖的唯一来源：固定版本 + SHA256（见下）
├── bindings/web/ariaread/web/  # 前端静态资源
└── tests/                  # 单元测试
```

## 依赖：一条路，不由 CMake 联网

所有第三方库（Continuo / OpenSSL / libcurl / zlib / nlohmann_json / SQLite3 /
QuickJS / Gumbo / doctest / sqlite_modern_cpp）由显式脚本取：固定版本 + SHA256
校验 + 许可证留档，只写进仓库的 `build/deps/`。CMake 只做 `find_package`，
配置时不联网、没有 vendored 回退分支。

```bash
python3 tools/ci/build_ariaread_deps.py            # 首次构建（约 10 分钟，主要是 OpenSSL）
cmake -S . -B build -DARIAREAD_BUILD_TESTS=ON      # 自动探测 build/deps/prefix
cmake --build build
ctest --test-dir build --output-on-failure
```

自定义前缀用 `-DARIAREAD_DEPS_PREFIX=<prefix>`。脚本 `--offline` 只走缓存、
`--only a,b` 只构建指定依赖、Windows 走 MSVC + nmake 分支（由 CI 验证）。

## 测试

从仓库根目录配置、构建并运行测试：

```bash
cmake -S . -B build -DARIAREAD_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

CTest 会注册引擎和可用的 ViewModel 测试，并在找到 Node.js 18+、Python 3.8+ 与 Web Server 目标时注册相应的调试回归。缺少可选依赖时，CMake 会明确提示跳过；可用 `-DARIAREAD_BUILD_WEB_TESTS=OFF` 关闭 Web 回归。

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
- [x] 第三方依赖改为脚本固定版本 + SHA256（`.gitmodules` 只剩 Aria 兄弟项目）
- [x] MIT LICENSE
- [x] README.md（中文）+ README.en.md（英文）

## 许可证

MIT License — 详见 [LICENSE](LICENSE)。
