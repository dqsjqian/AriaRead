cmake_minimum_required(VERSION 3.16)
project(OpenRead VERSION 0.1.0 LANGUAGES CXX C)

# ──────────────────────────────────────────────
# C++20 标准，全平台统一
# 升级自 C++17：为接入 Aria（C++20 协程 MVVM 框架）做准备。
# 兼容性：源码无 C++20 移除的废弃 API（result_of/is_pod/u8""/random_shuffle 等），
# 无隐式捕获 this 的 [=] lambda；第三方库各自锁定自身标准，不受此设置影响。
# ──────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)  # 用 std=c++20 不用 gnu++20

# ──────────────────────────────────────────────
# 编译选项
# ──────────────────────────────────────────────
if(MSVC)
    add_compile_options(/W4 /utf-8)
    # 静态链接运行时（避免 DLL 依赖）
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
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
option(OPENREAD_BUILD_SHARED    "Build shared library"              OFF)
option(OPENREAD_BUILD_TESTS     "Build unit tests"                  ON)
# OPENREAD_BUILD_PYTHON removed — C++ Web Server (Aria HttpAdapter) fully replaces Python/Flask
option(OPENREAD_ENFORCE_SELF_CONTAINED "Enforce self-contained third-party dependencies" ON)
option(OPENREAD_USE_SYSTEM_CURL "Use system libcurl instead of bundled third_party/curl" OFF)

if(OPENREAD_ENFORCE_SELF_CONTAINED AND OPENREAD_USE_SYSTEM_CURL)
    message(FATAL_ERROR "OPENREAD_ENFORCE_SELF_CONTAINED=ON forbids OPENREAD_USE_SYSTEM_CURL=ON")
endif()

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
    "${CMAKE_SOURCE_DIR}/include/openread/version.h.in"
    "${CMAKE_BINARY_DIR}/generated/openread/version.h"
    @ONLY
)

# ──────────────────────────────────────────────
# 第三方依赖（FetchContent 自动下载）
# ──────────────────────────────────────────────
include(FetchContent)

# nlohmann/json（header-only JSON 库）
# 优先使用本地 third_party/nlohmann_json，避免构建时依赖网络。
set(OPENREAD_THIRD_PARTY_JSON_DIR "${CMAKE_SOURCE_DIR}/third_party/nlohmann_json" CACHE PATH "Bundled nlohmann/json source directory")
if(EXISTS "${OPENREAD_THIRD_PARTY_JSON_DIR}/CMakeLists.txt")
    message(STATUS "Using local bundled nlohmann/json: ${OPENREAD_THIRD_PARTY_JSON_DIR}")
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    set(JSON_Install OFF CACHE BOOL "" FORCE)
    add_subdirectory(${OPENREAD_THIRD_PARTY_JSON_DIR} ${CMAKE_BINARY_DIR}/_deps/json-build EXCLUDE_FROM_ALL)
else()
    message(STATUS "Bundled nlohmann/json not found, fallback to FetchContent clone")
    FetchContent_Declare(
        json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
        GIT_SHALLOW    TRUE
        UPDATE_DISCONNECTED TRUE
    )
    FetchContent_MakeAvailable(json)
endif()

# ──────────────────────────────────────────────
# OpenSSL（从 third_party/openssl 源码编译，全平台统一 TLS 后端）
# ──────────────────────────────────────────────
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/openssl/Configure")
    message(STATUS "Building OpenSSL from source (third_party/openssl)")
    include(${CMAKE_SOURCE_DIR}/cmake/BuildOpenSSL.cmake)
else()
    message(FATAL_ERROR "OpenSSL source not found at third_party/openssl. "
        "Please run: git clone --depth 1 --branch openssl-3.3.1 https://github.com/openssl/openssl.git third_party/openssl")
endif()

# libcurl（HTTP 客户端）
# 默认使用仓库内/自动拉取的 bundled curl，避免依赖开发机上的系统包管理器动态库。
set(OPENREAD_THIRD_PARTY_CURL_DIR "${CMAKE_SOURCE_DIR}/third_party/curl" CACHE PATH "Bundled curl source directory")
if(OPENREAD_USE_SYSTEM_CURL)
    message(STATUS "Using system libcurl (OPENREAD_USE_SYSTEM_CURL=ON)")
    find_package(CURL REQUIRED)
else()
    message(STATUS "Using bundled libcurl from third_party/curl")

    # 关闭不需要的目标与协议，减少构建体积与外部依赖。
    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
    set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
    set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)   # 不需要 SCP/SFTP，关闭 LibSSH2
    set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)    # 不需要 LDAP 协议
    set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)   # 不需要 LDAPS 协议
    set(CURL_CA_PATH "none" CACHE STRING "" FORCE)    # 不使用 CA path 目录
    set(CURL_CA_BUNDLE "none" CACHE STRING "" FORCE)  # 不使用 CA bundle 文件（运行时通过代码设置）

    # 统一构建为静态库，减少运行时动态库冲突风险。
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

    # 全平台统一使用 OpenSSL 作为 TLS 后端（替代之前 macOS 的 SecureTransport）
    set(CURL_ENABLE_SSL ON CACHE BOOL "" FORCE)
    set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
    set(CURL_USE_SECTRANSP OFF CACHE BOOL "" FORCE)   # 不再使用 macOS SecureTransport
    set(CURL_USE_SCHANNEL OFF CACHE BOOL "" FORCE)    # 不再使用 Windows Schannel
    set(CURL_USE_MBEDTLS OFF CACHE BOOL "" FORCE)     # 不使用 mbedTLS
    set(CURL_DEFAULT_SSL_BACKEND "openssl" CACHE STRING "" FORCE)

    # 告诉 curl 的 CMake 在哪里找到我们编译的 OpenSSL
    set(OPENSSL_ROOT_DIR "${OPENSSL_INSTALL_DIR}" CACHE PATH "" FORCE)

    if(EXISTS "${OPENREAD_THIRD_PARTY_CURL_DIR}/CMakeLists.txt")
        message(STATUS "Using local bundled curl source: ${OPENREAD_THIRD_PARTY_CURL_DIR}")
        # 确保 OpenSSL 在 curl 之前编译完成
        # curl 的 find_package(OpenSSL) 会使用我们设置的 OPENSSL_ROOT_DIR
        add_subdirectory(${OPENREAD_THIRD_PARTY_CURL_DIR} ${CMAKE_BINARY_DIR}/_deps/curl-build EXCLUDE_FROM_ALL)
    else()
        message(STATUS "Bundled curl source not found, fallback to FetchContent clone")
        FetchContent_Declare(
            curl
            GIT_REPOSITORY https://github.com/curl/curl.git
            GIT_TAG        curl-8_8_0
            GIT_SHALLOW    TRUE
            UPDATE_DISCONNECTED TRUE
        )
        FetchContent_MakeAvailable(curl)
    endif()

    if(TARGET CURL::libcurl)
        set(CURL_FOUND TRUE)
    elseif(TARGET libcurl)
        add_library(CURL::libcurl ALIAS libcurl)
        set(CURL_FOUND TRUE)
    elseif(TARGET libcurl_static)
        add_library(CURL::libcurl ALIAS libcurl_static)
        set(CURL_FOUND TRUE)
    else()
        message(FATAL_ERROR "Bundled curl did not provide libcurl target")
    endif()

    # 确保 curl 的静态库目标依赖 OpenSSL 编译完成（不能对 alias target 添加依赖）
    foreach(_curl_target libcurl_static libcurl_shared)
        if(TARGET ${_curl_target})
            add_dependencies(${_curl_target} openssl_external)
        endif()
    endforeach()
endif()

if(NOT CURL_FOUND)
    message(FATAL_ERROR "libcurl is required but not found")
endif()

# Gumbo HTML5 解析器（Google 开源，纯 C99，跨平台零依赖）
add_subdirectory(third_party/gumbo)

# QuickJS（本地源码编译）
add_subdirectory(third_party/quickjs)

# SQLite3（amalgamation 编译，跨平台零依赖）
add_subdirectory(third_party/sqlite3_src)

# ──────────────────────────────────────────────
# Aria（C++20 响应式 MVVM 框架，git submodule 源码接入）
# 用于在 C++ 层承载响应式状态/协程异步/ViewModel，供各端 UI 复用同一业务逻辑。
# 以 vendored 源码方式接入（与 openssl/curl 一致），非预编译二进制。
# ──────────────────────────────────────────────
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/aria/CMakeLists.txt")
    message(STATUS "Using bundled Aria (third_party/aria)")
    # 关闭 Aria 自带的测试/示例/基准，避免：
    #   1. doctest target 与 OpenRead 测试重名冲突
    #   2. 编译大量无关的 examples/benchmark 拖慢构建
    set(ARIA_BUILD_TESTS     OFF CACHE BOOL "" FORCE)
    set(ARIA_BUILD_EXAMPLES  OFF CACHE BOOL "" FORCE)
    set(ARIA_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
    # runtime/binding 编为共享库（符合 Aria 单例 ABI 设计）；
    # OpenRead 静态库照常链接其导出符号。
    set(ARIA_BUILD_SHARED    ON  CACHE BOOL "" FORCE)
    # 启用 Aria HTTP/REST/SSE adapter（把 ViewModel 暴露给浏览器，零 Python）。
    # TLS：Aria 的 BuildOpenSSL.cmake 检测到父项目已定义 openssl_external
    # target（+ FindOpenSSL 兼容变量）时自动 early-return，复用父项目的
    # OpenSSL 编译产物。aria_http 的 CMakeLists 自行处理
    # CPPHTTPLIB_OPENSSL_SUPPORT / OpenSSL::SSL 链接，无需父项目手动干预。
    # （Aria 上游 2026-06-09 修复：CMAKE_SOURCE_DIR→PROJECT_SOURCE_DIR、
    #   BuildOpenSSL early-return、native_server() 强类型 escape hatch）
    set(ARIA_BUILD_HTTP        ON  CACHE BOOL "" FORCE)
    set(ARIA_HTTP_ENABLE_TLS   ON  CACHE BOOL "" FORCE)
    add_subdirectory(third_party/aria EXCLUDE_FROM_ALL)
else()
    message(STATUS "Aria submodule not found at third_party/aria. "
        "Run: git submodule update --init --recursive")
endif()

# ──────────────────────────────────────────────
# 核心库
# ──────────────────────────────────────────────
add_subdirectory(src)

# ──────────────────────────────────────────────
# ViewModel 层（基于 Aria，可选——仅当 Aria submodule 存在时构建）
# 先于 tests：tests/ 中的 ViewModel 测试需要 openread_vm target。
# ──────────────────────────────────────────────
if(TARGET aria::binding)
    add_subdirectory(src/viewmodels)
endif()

# ──────────────────────────────────────────────
# 单元测试（引擎 + ViewModel，统一在此引入 doctest）
# ──────────────────────────────────────────────
if(OPENREAD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ──────────────────────────────────────────────
# 安装配置（供 iOS/Android/Qt 集成）
# ──────────────────────────────────────────────
include(GNUInstallDirs)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(TARGETS openread
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# ──────────────────────────────────────────────
# Web 前端静态资源（由 C++ Web Server openread_web_server 提供服务）
# ──────────────────────────────────────────────
