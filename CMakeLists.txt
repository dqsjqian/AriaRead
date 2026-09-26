cmake_minimum_required(VERSION 3.16)
project(AriaRead VERSION 0.1.0 LANGUAGES CXX C)

# ──────────────────────────────────────────────
# C++20 标准，全平台统一
# 升级自 C++17：为接入 Aria（C++20 协程 MVVM 框架）做准备。
# 兼容性：源码无 C++20 移除的废弃 API（result_of/is_pod/u8""/random_shuffle 等），
# 无隐式捕获 this 的 [=] lambda；第三方库各自锁定自身标准，不受此设置影响。
# ──────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)  # 用 std=c++23 不用 gnu++23

# ──────────────────────────────────────────────
# 编译选项
# ──────────────────────────────────────────────
if(MSVC)
    add_compile_options(/W4 /utf-8)
    # 动态 CRT：Aria 以共享库（aria_*.dll）接入，/MT 会让 exe 与 DLL 各持
    # 一份 CRT 堆，跨模块分配/释放（析构、引用计数释放）触发堆损坏
    # （0xc0000374，测试后退出即崩）。/MD 让全部模块共享 UCRT 堆。
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
else()
    add_compile_options(-Wall -Wextra -Wpedantic -fPIC -Wno-unused-parameter)
    # macOS/iOS 隐藏符号（减小体积）
    set(CMAKE_C_VISIBILITY_PRESET hidden)
    set(CMAKE_CXX_VISIBILITY_PRESET hidden)
    set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
endif()

# ──────────────────────────────────────────────
# 构建选项
# ──────────────────────────────────────────────
option(ARIAREAD_BUILD_SHARED    "Build shared library"              OFF)
option(ARIAREAD_BUILD_TESTS     "Build unit tests"                  ON)
# ARIAREAD_BUILD_PYTHON removed — C++ Web Server (Continuo) fully replaces Python/Flask
# ARIAREAD_ENFORCE_SELF_CONTAINED / ARIAREAD_USE_SYSTEM_CURL removed — 依赖现在只有
# 一个来源（tools/ci/build_ariaread_deps.py 的固定版本前缀），不再有"系统库 vs
# vendored"的开关。

# ──────────────────────────────────────────────
# 输出目录统一
# ──────────────────────────────────────────────
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ──────────────────────────────────────────────
# 版本号头文件（从 CMake 版本号自动生成，全项目唯一版本源）
# ──────────────────────────────────────────────
configure_file(
    "${CMAKE_SOURCE_DIR}/include/ariaread/version.h.in"
    "${CMAKE_BINARY_DIR}/generated/ariaread/version.h"
    @ONLY
)

# ──────────────────────────────────────────────
# 第三方依赖：唯一来源是 tools/ci/build_ariaread_deps.py 构建的前缀
#
# 与 Continuo 一致的依赖哲学：
#   * 依赖由显式脚本取（固定版本 + SHA256 + 许可证留档），只写仓库 build/；
#   * CMake 侧只 find_package，配置时不联网、不 FetchContent、不 add_subdirectory
#     第三方源码；
#   * 没有前缀就直接失败并提示先跑脚本——不再维护 vendored third_party 回退。
#
# Aria（兄弟项目，四库源码联动）同样去 submodule 化：由 tools/ci/fetch_aria.py
# 以固定 commit SHA 取到 build/deps/aria，CMake 只 add_subdirectory 已取回的
# 源码树，配置期同样不联网。
# ──────────────────────────────────────────────
set(ARIAREAD_DEPS_PREFIX "" CACHE PATH
    "tools/ci/build_ariaread_deps.py 的安装前缀；留空则自动探测 <repo>/build/deps/prefix")
if(NOT ARIAREAD_DEPS_PREFIX
        AND EXISTS "${CMAKE_SOURCE_DIR}/build/deps/prefix/share/ariaread-deps/manifest.json")
    set(ARIAREAD_DEPS_PREFIX "${CMAKE_SOURCE_DIR}/build/deps/prefix")
endif()
if(NOT ARIAREAD_DEPS_PREFIX)
    message(FATAL_ERROR
        "未找到依赖前缀。请先执行：python3 tools/ci/build_ariaread_deps.py\n"
        "再用 -DARIAREAD_DEPS_PREFIX=<prefix> 配置（留空会自动探测 build/deps/prefix）。")
endif()
if(NOT EXISTS "${ARIAREAD_DEPS_PREFIX}/share/ariaread-deps/manifest.json")
    message(FATAL_ERROR
        "ARIAREAD_DEPS_PREFIX 缺少 manifest.json，不像依赖前缀：${ARIAREAD_DEPS_PREFIX}")
endif()
list(APPEND CMAKE_PREFIX_PATH "${ARIAREAD_DEPS_PREFIX}")
message(STATUS "Using pinned dependency prefix: ${ARIAREAD_DEPS_PREFIX}")

# 前缀里同时有静态库和动态库时优先静态：产物是"拷走就能跑"的目录，
# 不希望运行时再去 @rpath 找 dylib（zlib 的 CMake 两种都编）。
if(UNIX)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a" ${CMAKE_FIND_LIBRARY_SUFFIXES})
endif()

# Continuo（C++23 协程网络库）：Web Server 的 HTTP/1.1 引擎，取代 cpp-httplib
find_package(continuo 0.1 REQUIRED)
# nlohmann/json（header-only JSON 库）
find_package(nlohmann_json 3.12 REQUIRED)
# zlib：libcurl 的 gzip 解压后端，缺了书源会 CURLE_BAD_CONTENT_ENCODING
find_package(ZLIB REQUIRED)
# libcurl（HTTP 客户端，静态链接，OpenSSL 后端由 curl 自己的 config 带入）
find_package(CURL REQUIRED)
set(CURL_FOUND TRUE)
# Gumbo（HTML5 解析）、QuickJS（书源 JS 引擎）、SQLite3（amalgamation）
find_package(Gumbo REQUIRED)
find_package(QuickJS REQUIRED)
find_package(SQLite3Amalgamation REQUIRED)
# sqlite_modern_cpp（header-only C++ 封装，带 AriaRead 的 C++20 兼容补丁）
find_package(sqlite_modern_cpp REQUIRED)

# 统一的目标名，供 src/、tests/ 使用（前缀模式与后续可能的其它来源只改这里）
set(ARIAREAD_GUMBO_TARGET gumbo::gumbo)
set(ARIAREAD_QUICKJS_TARGET quickjs::quickjs)
set(ARIAREAD_SQLITE_TARGET sqlite3)
set(ARIAREAD_SQLITE_MODERN_CPP_INCLUDE_DIR "${ARIAREAD_DEPS_PREFIX}/include")


# ──────────────────────────────────────────────
# Aria（C++23 响应式 MVVM 框架，固定 commit SHA 源码接入）
# 用于在 C++ 层承载响应式状态/协程异步/ViewModel，供各端 UI 复用同一业务逻辑。
# 由 tools/ci/fetch_aria.py 取到 build/deps/aria（无 git submodule、无 vendored
# 源码），配置前先跑一次脚本。
# ──────────────────────────────────────────────
set(ARIA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/build/deps/aria CACHE PATH
    "Pinned Aria source tree, created by tools/ci/fetch_aria.py")
if(NOT EXISTS ${ARIA_DIR}/CMakeLists.txt)
    message(FATAL_ERROR
        "Aria not found at ${ARIA_DIR}.\n"
        "Run once before configuring:\n"
        "  python tools/ci/fetch_aria.py\n"
        "(no git submodules -- Aria is fetched at a pinned commit SHA)")
endif()
message(STATUS "Using pinned Aria at ${ARIA_DIR}")
# 关闭 Aria 自带的测试/示例/基准，避免：
#   1. doctest target 与 AriaRead 测试重名冲突
#   2. 编译大量无关的 examples/benchmark 拖慢构建
set(ARIA_BUILD_TESTS     OFF CACHE BOOL "" FORCE)
set(ARIA_BUILD_EXAMPLES  OFF CACHE BOOL "" FORCE)
set(ARIA_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
# runtime/binding 编为共享库（符合 Aria 单例 ABI 设计）；
# AriaRead 静态库照常链接其导出符号。
set(ARIA_BUILD_SHARED    ON  CACHE BOOL "" FORCE)
# 不再启用 Aria 的 HTTP adapter：Web Server 的 HTTP 层已换成 Continuo，
# Aria 在这里只提供响应式/协程/ViewModel（core + async + binding）。
set(ARIA_BUILD_HTTP        OFF CACHE BOOL "" FORCE)
set(ARIA_HTTP_ENABLE_TLS   OFF CACHE BOOL "" FORCE)
add_subdirectory(${ARIA_DIR} aria)

# ──────────────────────────────────────────────
# 核心库
# ──────────────────────────────────────────────
add_subdirectory(src)

# ──────────────────────────────────────────────
# ViewModel 层（基于 Aria，可选——仅当 Aria 已取回时构建）
# 先于 tests：tests/ 中的 ViewModel 测试需要 ariaread_vm target。
# ──────────────────────────────────────────────
if(TARGET aria::binding)
    add_subdirectory(src/viewmodels)
endif()

# ──────────────────────────────────────────────
# 单元测试（引擎 + ViewModel，统一在此引入 doctest）
# ──────────────────────────────────────────────
if(ARIAREAD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ──────────────────────────────────────────────
# 安装配置（供 iOS/Android/Qt 集成）
# ──────────────────────────────────────────────
include(GNUInstallDirs)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(TARGETS ariaread
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# ──────────────────────────────────────────────
# Web 前端静态资源（由 C++ Web Server ariaread_web_server 提供服务）
# ──────────────────────────────────────────────
