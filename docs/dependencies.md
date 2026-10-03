# 依赖更新使用指南

从 **AriaRead 仓库根目录**执行。需要 Python 3.10+、CMake 和 [README](../README.md) 所列工具链；macOS/Linux 可把 `python` 换成 `python3`。只用 Python 标准库，无需 pip。首次解析和主动更新需要联网，GitHub API 可使用已登录的 `gh`。

所有命令走统一入口 `python tools/build.py`。内部实现（`tools/_build/deps_build.py`、`deps_sources.py`）是纯库模块，没有单独可执行的依赖脚本。

## 只维护一个依赖文件

`dependencies.json` 同时保存要求和已解析结果，**包括 aria 在内的全部依赖一视同仁**。每个依赖条目包含：

- 来源字段，如 `provider`、`repo`、`artifact`。
- 可选的顶层 `version`：你希望长期固定的版本。不填表示首次解析或主动更新时选最新稳定版。
- 工具生成的 `resolved`：实际版本、完整提交、下载地址、SHA256 和请求校验值。不要手改这些结果或校验值。

来源不会在 `resolved` 中再复制一份。普通构建复用有效结果，不会每次追随上游；预发布和开发分支不属于“最新稳定版”。只有这一个依赖文件需要随版本选择提交 Git，下载和构建缓存不提交。

可用名称区分大小写：`Mira`、`aria`、`curl`、`doctest`、`gumbo`、`json`、`openssl`、`quickjs`、`sqlite3`、`sqlite_modern_cpp`、`zlib`。

## 源码工作区

每个依赖的源码平铺在 `deps/<名>/`（`deps/aria`、`deps/curl`……），没有版本号目录名，也没有 per 库脚本。`dependencies.json` 只决定一个**缺失**目录的初始内容；之后目录的实际内容就是构建输入——你可以 `git pull`、切分支、直接编辑，改动会在下次构建时使该组件及受影响消费者重编。

锁选择变更后：未修改过的目录自动置换为新版本（旧目录备份到 `deps/.ariaread-sources/backups/`）；有本地修改的目录保留，构建时警告“新选择未应用”。显式更新（`deps-update` 或 `--version`）遇到本地修改会明确失败，不会静默跳过。

## 常用命令

```bash
python tools/build.py deps-update --help

# 只更新 curl；其他依赖保持原选择
python tools/build.py deps-update --only curl

# 选择多个库，并为本次操作指定版本
python tools/build.py deps-update --only json --only openssl --version json=3.12.0 --version openssl=4.0.3

# 按文件中的要求主动更新全部依赖
python tools/build.py deps-update
```

`--only` 可重复；`--version` 对不同名称可重复。同用时，所有版本覆盖项都必须列在 `--only` 中。名称拼错、同名覆盖重复或选择范围不一致会报错。

要让两个库长期固定、另一个跟最新，在现有 `json` 条目中加 `"version": "3.12.0"`，在 `openssl` 条目中加 `"version": "4.0.3"`，保留来源字段；`curl` 条目不填顶层 `version`。然后执行：

```bash
python tools/build.py deps-update --only json --only openssl --only curl
```

这样固定要求持续生效，只有未固定的 curl 重新选择最新稳定版。删除某项顶层 `version` 后主动更新即可解除固定；不需要改任何脚本，也不需要删除 `resolved`。

优先级为本次 CLI 覆盖、文件中的显式 `version`、已有有效结果。CLI 不修改顶层长期要求：如果文件没有不同的显式版本，后续普通解析继续复用 CLI 选出的结果；如果文件明确要求另一版本，下一次不传覆盖参数的构建会恢复文件要求。

## 更新后构建和测试

更新器原子保存解析结果，不自动编译项目，也不保证新版本 API 兼容。更新成功后执行：

```bash
python tools/build.py --test
```

这一条命令完成依赖工作区准备、安装前缀、配置、编译和 CTest。CI 加 `--require-web-tests`，缺少 Node 等测试依赖就明确失败；普通本机构建保留可选 Web 测试行为。

审查 `git diff -- dependencies.json`，构建和测试成功后提交该文件的变更。系统编译器、SDK 和 Qt 等平台工具仍需单独安装，更新器不会安装它们。

## 离线、局部构建和只读校验

```bash
# 一键离线构建：使用已有源码工作区或精确来源缓存，缺失时明确失败
python tools/build.py --offline

# 只构建指定第三方库及所需前置依赖
python tools/build.py deps --only zlib,json

# 只读验证源码与已安装组件是否匹配（CMake 配置期也调用它）
python tools/build.py deps-check
```

离线元数据解析成功不代表归档已下载。离线重建需要精确匹配的归档或已存在的源码工作区；`deps-update --offline` 不能发现新版本。部分安装不能用于完整应用配置，须先补齐依赖。

## 安装前缀与实验文件

第三方库（含 aria）由统一入口构建。用顶层 `version` 或 `--version NAME=VERSION` 选择库版本。

CMake 通过入口的 `deps-check` 验证结果和前缀，不联网、不重新选择版本。因此本次合法 CLI 覆盖得到的结果能被 CMake 消费；下一次构建的版本选择仍遵守前述优先级。

前缀按组件绑定解析结果、构建配方/补丁、编译器、ABI 环境和安装文件内容。局部变化只重建该组件及受影响的静态消费者；编译器/ABI 变化使整个前缀失效。更新保留 `prefix-backup-*`；构建失败恢复旧前缀，并保留 `prefix-failed-*`。旧格式缓存迁移需首次重建一次。修改过的安装文件不会被覆盖。QuickJS、Gumbo、sqlite_modern_cpp、doctest 的补丁仅适用于已审核版本，新版本需要先适配配方。Windows 配方支持 x64、Release，以及 Visual Studio、Ninja、Ninja Multi-Config、NMake 的相应生成器参数。

实验用 `--file` 指向另一份完整依赖文件：

```bash
python tools/build.py deps --file build/deps/experiment.json --only json
```

实验文件不会自动改变应用 CMake 使用的根文件。用于正式构建前，应审查并把所需选择同步到根文件，再按根文件构建前缀。自定义安装位置用 `--deps-prefix <path>`，再传 `-DARIAREAD_DEPS_PREFIX=<path>` 给 CMake。

## 失败与回退

解析或下载校验失败时不提交半套新结果；先修正版本、网络或 API 限流问题。不要手改 SHA256 来接受不同字节。若新版本不兼容，保留当前修改后，从已验证的 Git 提交恢复 **`dependencies.json` 这一个文件**，再运行 `deps` 和 `--test`。恢复元数据不会自动恢复二进制；受保护的旧前缀和失败目录可用于排查。

## 统一入口与平台精简

推荐 `python tools/build.py`；它完成固定依赖获取、配置、编译和运行目录检查。`--test` 同时补测试依赖并执行 CTest。依赖集合由 `--profile` 控制（`--test` 自动选 `tests`，否则 `runtime`），CMake 相应传 `-DARIAREAD_BUILD_TESTS`。

Windows 默认 `--tls-backend auto` 选择 Schannel，OpenSSL 不在构建集合；显式 `--tls-backend openssl` 才构建它，CMake 同时传 `-DARIAREAD_TLS_BACKEND=openssl`。macOS/Linux 的 auto 仍为 OpenSSL，macOS 启用 Apple SecTrust。Windows MSVC/MinGW 不共用二进制前缀；下载缓存仍共享。详见[依赖精简与分工](build-architecture.md)。
