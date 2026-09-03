# OpenRead

📖 跨平台阅读引擎，专注于开源中文书源生态。

基于 [Aria](https://github.com/dqsjqian/Aria)（C++20 响应式 MVVM 框架）构建，兼容主流书源格式。

## 界面截图

同一份 C++ 核心同时驱动两种 Web 形态：

| 形态 | 截图 | 说明 |
|---|---|---|
| REST + SSE 薄客户端 | ![OpenRead-Web](docs/marketing/images/OpenRead-Web.png) | 浏览器直连 C++ HTTP 壳，Property 变化经 SSE 推送 |
| SSR 服务端渲染 | ![OpenRead-SSR](docs/marketing/images/OpenRead-SSR.png) | 页面由 C++ 侧渲染，前端零 JS 依赖也能跑 |

## 特性

- **C++20 响应式 MVVM**：基于 Aria 框架，支持协程异步、响应式状态与 ViewModel
- **跨平台引擎**：CSS3/XPath 选择器、JS 运行时桥接、Gumbo HTML5 解析
- **自包含第三方依赖**：nlohmann/json、OpenSSL、libcurl 均从源码编译，零系统依赖
- **扩展 ViewModel 层**：SearchViewModel、BookshelfViewModel、ReaderViewModel、SourceViewModel
- **Engine Adapters**：engine_reader_adapter、engine_source_adapter，将 engine.h 的私有依赖隔离在 .cpp 内
- **C++ Web Server**：Aria HttpAdapter + ViewModel，39 条 REST 路由，SSE 推送，零 Python 依赖

## 构建

```bash
mkdir -p build && cd build
cmake .. -DOPENREAD_BUILD_TESTS=ON
cmake --build . --target openread-tests
ctest --output-on-failure
```

## 项目结构

```
OpenRead/
├── CMakeLists.txt          # C++20，全平台统一编译选项
├── include/                # 公共头文件
│   └── openread/
│       └── version.h.in    # 版本号模板
├── src/                    # 核心引擎源码
│   ├── engine/             # 引擎核心
│   ├── selector/           # CSS/XPath 选择器
│   ├── rule/               # 规则解析器
│   ├── infra/              # 基础设施（HTTP/JS/DB）
│   ├── viewmodels/         # ViewModel 层
│   └── apps/web_server/    # Web Server（Aria HttpAdapter + REST API）
├── third_party/
│   ├── aria/               # Aria C++20 协程 MVVM 框架（git submodule）
│   ├── curl/               # libcurl 从源码编译
│   ├── nlohmann_json/      # nlohmann/json
│   ├── openssl/             # OpenSSL
│   ├── quickjs/             # QuickJS
│   └── sqlite3_src/        # SQLite3 amalgamation
├── bindings/web/openread/web/  # 前端静态资源
└── tests/                  # 单元测试
```

## 测试

109 个测试全部通过。

## 开源准备

- [x] 源码全部从源码编译（无预编译二进制）
- [x] `.gitmodules` 声明所有第三方依赖
- [x] MIT LICENSE
- [x] README.md

## 许可证

MIT License — 详见 [LICENSE](LICENSE)。
