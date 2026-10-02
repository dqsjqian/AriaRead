# 依赖更新使用指南

从 **AriaRead 仓库根目录**执行。需要 Python 3.10+、CMake 和 [README](../README.md) 所列工具链；macOS/Linux 可把 `python` 换成 `python3`。只用 Python 标准库，无需 pip。首次解析和主动更新需要联网，GitHub API 可使用已登录的 `gh`。

## 只维护一个依赖文件

`dependencies.json` 同时保存要求和已解析结果。每个依赖条目包含：

- 来源字段，如 `provider`、`repo`、`artifact`。
- 可选的顶层 `version`：你希望长期固定的版本。不填表示首次解析或主动更新时选最新稳定版。
- 脚本生成的 `resolved`：实际版本、完整提交、下载地址、SHA256 和请求校验值。不要手改这些结果或校验值。

来源不会在 `resolved` 中再复制一份。普通构建复用有效结果，不会每次追随上游；预发布和开发分支不属于“最新稳定版”。只有这一个依赖文件需要随版本选择提交 Git，下载和构建缓存不提交。

可用名称区分大小写：`Mira`、`aria`、`curl`、`doctest`、`gumbo`、`json`、`openssl`、`quickjs`、`sqlite3`、`sqlite_modern_cpp`、`zlib`。

## 常用命令

```bash
python tools/ci/update_dependencies.py --help

# 只更新 curl；其他依赖（包括 Mira）保持原选择
python tools/ci/update_dependencies.py --only curl

# 选择多个库，并为本次操作指定版本
python tools/ci/update_dependencies.py --only json --only openssl --version json=3.12.0 --version openssl=4.0.3

# 按文件中的要求主动更新全部依赖
python tools/ci/update_dependencies.py
```

`--only` 可重复；`--version` 对不同名称可重复。同用时，所有版本覆盖项都必须列在 `--only` 中。名称拼错、同名覆盖重复或选择范围不一致会报错。

要让两个库长期固定、另一个跟最新，在现有 `json` 条目中加 `"version": "3.12.0"`，在 `openssl` 条目中加 `"version": "4.0.3"`，保留来源字段；`curl` 条目不填顶层 `version`。然后执行：

```bash
python tools/ci/update_dependencies.py --only json --only openssl --only curl
```

这样固定要求持续生效，只有未固定的 curl 重新选择最新稳定版。删除某项顶层 `version` 后主动更新即可解除固定；不需要改 Python 脚本，也不需要删除 `resolved`。

优先级为本次 CLI 覆盖、文件中的显式 `version`、已有有效结果。CLI 不修改顶层长期要求：如果文件没有不同的显式版本，后续普通解析继续复用 CLI 选出的结果；如果文件明确要求另一版本，下一次不传覆盖参数的 builder 调用会恢复文件要求。

## 更新后构建和测试

更新器原子保存解析结果，不自动编译项目，也不保证新版本 API 兼容。更新成功后执行：

```bash
python tools/ci/fetch_aria.py
python tools/ci/build_ariaread_deps.py --jobs 3
cmake -S . -B build/flavors/dependency-check -DARIAREAD_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/flavors/dependency-check --config Release --parallel 3
ctest --test-dir build/flavors/dependency-check -C Release --output-on-failure --no-tests=error
```

构建和 CTest 使用相同配置；更换编译器或平台时换构建目录。CI 加上 `-DARIAREAD_REQUIRE_WEB_TESTS=ON`，缺少 Node 等测试依赖就明确失败；普通本机构建保留可选 Web 测试行为。

审查 `git diff -- dependencies.json`，构建和测试成功后提交该文件的变更。系统编译器、SDK 和 Qt 等平台工具仍需单独安装，更新器不会安装它们。

## 离线、局部构建和本地源码

```bash
# 只补缺失或不匹配的结果，不主动升级有效选择
python tools/ci/dependencies.py resolve --file dependencies.json

# 不联网；缺少匹配结果时失败
python tools/ci/dependencies.py resolve --file dependencies.json --offline

# 安装前缀相同则复用；需要重建时必须已有精确源码缓存
python tools/ci/build_ariaread_deps.py --offline

# 只构建指定第三方库及所需前置依赖
python tools/ci/build_ariaread_deps.py --only zlib,json
```

离线元数据解析成功不代表归档已下载。离线重建需要精确匹配的归档或 Git 提交缓存；`update --offline` 不能发现新版本。部分安装不能用于完整应用配置，须先补齐依赖。

Aria 获取器支持 `--file`、`--version`、`--update`、`--offline` 和 `--source`。`--version` 优先于 `ARIA_DEP_ARIA_VERSION`；`--source /path/to/Aria` 优先于 `ARIA_SOURCE`。本地 Git 源仍须包含选定的完整提交；获取器拒绝覆盖本地修改，成功替换时保留旧目录为 `build/deps/aria-backup-*`。直接源码联调也可用 CMake 的 `-DARIA_DIR=/path/to/Aria`。

## 安装前缀与实验文件

第三方库由 `build_ariaread_deps.py` 构建。用顶层 `version` 或 builder 的 `--version NAME=VERSION` 选择库版本。Aria 内部的 `ARIA_DEP_*` CMake 参数不负责选择 AriaRead 已安装的第三方库。

CMake 固定读取仓库根目录的 `dependencies.json`，只验证结果和前缀，不联网、不重新选择版本。因此本次合法 CLI 覆盖得到的结果能被 CMake 消费；下一次 builder 的版本选择仍遵守前述优先级。

前缀按组件绑定解析结果、构建配方/补丁、编译器、ABI 环境和安装文件内容。局部变化只重建该组件及受影响的静态消费者；编译器/ABI 变化使整个前缀失效。更新保留 `prefix-backup-*`；构建失败恢复旧前缀，并保留 `prefix-failed-*`。旧格式缓存迁移需首次重建一次。修改过的安装文件或 Git 缓存不会被覆盖。QuickJS、Gumbo、sqlite_modern_cpp 的补丁仅适用于已审核版本，新版本需要先适配配方。Windows 配方支持 x64、Release，以及 Visual Studio、Ninja、Ninja Multi-Config、NMake 的相应生成器参数。

更新器和底层解析器还支持 `--file PATH`、`--output PATH`、`--cache-dir PATH`。没有 `--output` 时原子更新输入文件；指定它时写入另一份完整文件，适合实验：

```bash
python tools/ci/dependencies.py resolve --file dependencies.json --output build/deps/experiment.json --only json --version json=3.12.0
python tools/ci/build_ariaread_deps.py --file build/deps/experiment.json --path build/deps-experiment --only json
```

实验文件不会自动改变应用 CMake 使用的根文件。用于正式构建前，应审查并把所需选择同步到根文件，再按根文件构建前缀。自定义安装位置用 builder 的 `--prefix <path>`，再传 `-DARIAREAD_DEPS_PREFIX=<path>` 给 CMake。

## 失败与回退

解析或下载校验失败时不提交半套新结果；先修正版本、网络或 API 限流问题。不要手改 SHA256 来接受不同字节。若新版本不兼容，保留当前修改后，从已验证的 Git 提交恢复 **`dependencies.json` 这一个文件**，再运行获取、构建和测试。恢复元数据不会自动恢复二进制；受保护的旧前缀和失败目录可用于排查。

## 统一入口与平台精简

推荐 `python tools/build.py`；它完成固定依赖获取、配置、编译和运行目录检查。`--test` 同时补测试依赖并执行 CTest。底层依赖脚本默认 `--profile tests`，仅运行时可用 `--profile runtime` 并给 CMake 传 `-DARIAREAD_BUILD_TESTS=OFF`。

Windows 默认 `--tls-backend auto` 选择 Schannel，OpenSSL 不在构建集合；显式 `--tls-backend openssl` 才构建它，CMake 同时传 `-DARIAREAD_TLS_BACKEND=openssl`。macOS/Linux 的 auto 仍为 OpenSSL，macOS 启用 Apple SecTrust。Windows MSVC/MinGW 不共用二进制前缀；下载缓存仍共享。详见[依赖精简与分工](build-architecture.md)。
