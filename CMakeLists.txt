cmake_minimum_required(VERSION 3.21)
project(AriaRead VERSION 0.3.1 LANGUAGES CXX C)
include(cmake/CompilerRequirements.cmake)

# The installed dependency SDK uses Release. Pick it for ordinary single-config
# builds and reject Windows Debug before reaching a CRT-mismatch linker error.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)
endif()
if(WIN32)
    if(CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_CONFIGURATION_TYPES Release CACHE STRING "Supported SDK configuration" FORCE)
    elseif(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        message(FATAL_ERROR "The Windows dependency SDK uses Release. Configure with -DCMAKE_BUILD_TYPE=Release.")
    endif()
endif()

# ──────────────────────────────────────────────
# C++23 标准，全平台统一，与 Aria 和 Mira 的公开接口要求一致。
# 第三方库各自锁定自身标准，不受此设置影响。
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
set(ARIAREAD_TLS_BACKEND auto CACHE STRING "curl TLS backend: auto, openssl, schannel")
set_property(CACHE ARIAREAD_TLS_BACKEND PROPERTY STRINGS auto openssl schannel)
if(ARIAREAD_BUILD_TESTS)
    set(_ariaread_deps_profile tests)
else()
    set(_ariaread_deps_profile runtime)
endif()
# ARIAREAD_BUILD_PYTHON removed — C++ Web Server (Mira) fully replaces Python/Flask
# ARIAREAD_ENFORCE_SELF_CONTAINED / ARIAREAD_USE_SYSTEM_CURL removed — 依赖现在只有
# 一个来源（scripts/build.py 管理的依赖前缀），不再有"系统库 vs
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
# 第三方依赖：唯一来源是 scripts/build.py 构建的前缀
#
# 与 Mira 一致的依赖哲学：
#   * 初次来源由依赖文件 + SHA256 固定，源码平铺 deps/，产物放 build/；
#   * CMake 侧只 find_package，配置时不联网、不 FetchContent、不 add_subdirectory
#     第三方源码；
#   * 没有前缀就直接失败并提示先跑脚本——不再维护 vendored third_party 回退。
#     aria 也一样：由 scripts/build.py 与其他依赖一起装进前缀。
# ──────────────────────────────────────────────
set(ARIAREAD_DEPS_PREFIX "" CACHE PATH
    "scripts/build.py 的安装前缀；留空则自动探测 <repo>/build/deps/prefix")
set(ARIAREAD_DEPENDENCIES_FILE "${CMAKE_SOURCE_DIR}/dependencies.json" CACHE FILEPATH
    "Dependency declarations and initial source selections")
set(ARIAREAD_DEPS_SOURCE_DIR "${CMAKE_SOURCE_DIR}/deps" CACHE PATH
    "Editable dependency source workspace")
if(NOT ARIAREAD_DEPS_PREFIX
        AND EXISTS "${CMAKE_SOURCE_DIR}/build/deps/prefix/share/ariaread-deps/manifest.json")
    set(ARIAREAD_DEPS_PREFIX "${CMAKE_SOURCE_DIR}/build/deps/prefix")
endif()
if(NOT ARIAREAD_DEPS_PREFIX)
    message(FATAL_ERROR
        "未找到依赖前缀。请先执行：python scripts/build.py deps --profile tests\n"
        "再用 -DARIAREAD_DEPS_PREFIX=<prefix> 配置（留空会自动探测 build/deps/prefix）。")
endif()
if(NOT EXISTS "${ARIAREAD_DEPS_PREFIX}/share/ariaread-deps/manifest.json")
    message(FATAL_ERROR
        "ARIAREAD_DEPS_PREFIX 缺少 manifest.json，不像依赖前缀：${ARIAREAD_DEPS_PREFIX}")
endif()
# Offline validation also detects edits/pulls made after the last dependency build.
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/build.py" deps-check
        --prefix "${ARIAREAD_DEPS_PREFIX}"
        --profile "${_ariaread_deps_profile}" --tls-backend "${ARIAREAD_TLS_BACKEND}"
        --file "${ARIAREAD_DEPENDENCIES_FILE}" --source-dir "${ARIAREAD_DEPS_SOURCE_DIR}"
        --c-compiler "${CMAKE_C_COMPILER}" --cxx-compiler "${CMAKE_CXX_COMPILER}"
        "--c-compiler-arg1=${CMAKE_C_COMPILER_ARG1}" "--cxx-compiler-arg1=${CMAKE_CXX_COMPILER_ARG1}"
        --toolchain-file "${CMAKE_TOOLCHAIN_FILE}" --cmake-platform "${CMAKE_GENERATOR_PLATFORM}"
    RESULT_VARIABLE _ariaread_deps_result OUTPUT_VARIABLE _ariaread_deps_output
    ERROR_VARIABLE _ariaread_deps_error ENCODING UTF-8)
if(NOT "${_ariaread_deps_result}" STREQUAL "0")
    message(FATAL_ERROR "Dependency prefix validation failed:\n${_ariaread_deps_output}${_ariaread_deps_error}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${ARIAREAD_DEPENDENCIES_FILE}")
list(PREPEND CMAKE_PREFIX_PATH "${ARIAREAD_DEPS_PREFIX}")
message(STATUS "Using verified dependency prefix: ${ARIAREAD_DEPS_PREFIX}")

# 前缀里同时有静态库和动态库时优先静态：产物是"拷走就能跑"的目录，
# 不希望运行时再去 @rpath 找 dylib（zlib 的 CMake 两种都编）。
if(UNIX)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a" ${CMAKE_FIND_LIBRARY_SUFFIXES})
elseif(WIN32)
    # FindZLIB 在 Windows 上对 zlib.lib（动态 import）与 zlibstatic.lib 的
    # 选择不确定：选中动态会让 zlib.dll 成为隐形运行时依赖，而没有任何
    # 打包脚本会把它带进运行时目录。显式钉死静态版。
    find_library(ZLIB_LIBRARY NAMES zlibstatic
        PATHS "${ARIAREAD_DEPS_PREFIX}/lib" NO_DEFAULT_PATH
        CACHE FILEPATH "Prefer the static zlib from the pinned prefix")
endif()

# Mira（C++23 协程网络库）：Web Server 的 HTTP/1.1 引擎，取代 cpp-httplib
find_package(Mira 1.0 REQUIRED COMPONENTS core transport http)
# nlohmann/json（header-only JSON 库）
find_package(nlohmann_json 3.12 REQUIRED)
# zlib：libcurl 的 gzip 解压后端，缺了书源会 CURLE_BAD_CONTENT_ENCODING
find_package(ZLIB REQUIRED)
# libcurl exports the selected TLS backend: Schannel on Windows, OpenSSL on
# other platforms. CONFIG avoids FindCURL silently selecting a system library.
find_package(CURL REQUIRED CONFIG PATHS "${ARIAREAD_DEPS_PREFIX}" NO_DEFAULT_PATH)
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
# Aria（C++23 响应式 MVVM 框架）
# 用于在 C++ 层承载响应式状态/协程异步/ViewModel，供各端 UI 复用同一业务逻辑。
# 与其他依赖完全一致：scripts/build.py 从 dependencies.json 取到 deps/aria，
# 装进依赖前缀，这里只 find_package。上游 ARIA_BUILD_SHARED=ON 保留其
# 单例 ABI 设计（前缀里是共享库，其余依赖均为静态）。
# ──────────────────────────────────────────────
find_package(aria REQUIRED CONFIG)
message(STATUS "Using aria ${aria_VERSION} (installed dependency)")

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

# Reuse the selected prefix/source in packaging scripts, including --skip-cmake.
file(WRITE "${CMAKE_BINARY_DIR}/AriaReadRuntimePaths.cmake"
    "set(ARIAREAD_DEPS_PREFIX [==[${ARIAREAD_DEPS_PREFIX}]==])\n")
set(ARIAREAD_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(ARIAREAD_LICENSE_OUTPUT "${CMAKE_BINARY_DIR}/license-bundle")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/StageLicenses.cmake")
install(DIRECTORY "${ARIAREAD_LICENSE_OUTPUT}/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/ariaread")


install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(TARGETS ariaread
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# ──────────────────────────────────────────────
# Web 前端静态资源（由 C++ Web Server ariaread_web_server 提供服务）
# ──────────────────────────────────────────────
