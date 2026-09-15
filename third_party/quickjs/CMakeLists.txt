# ──────────────────────────────────────────────
# QuickJS 2026-06-04 (Official)
# 移除了 bignum 扩展，不再需要 libbf.c / CONFIG_BIGNUM
# ──────────────────────────────────────────────

set(QUICKJS_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set(QUICKJS_SOURCES
    ${QUICKJS_SOURCE_DIR}/quickjs.c
    ${QUICKJS_SOURCE_DIR}/libregexp.c
    ${QUICKJS_SOURCE_DIR}/libunicode.c
    ${QUICKJS_SOURCE_DIR}/cutils.c
    ${QUICKJS_SOURCE_DIR}/dtoa.c
)

# quickjs-libc.c 提供 os/std JS 模块（dup2/readlink/信号等 POSIX API），
# MSVC 下不可移植。OpenRead 的 JsRuntime 不使用 js_init_module_os/std 与
# js_module_loader_set 符号（src/ grep 零命中），MSVC 构建直接排除。
if(NOT MSVC)
    list(APPEND QUICKJS_SOURCES ${QUICKJS_SOURCE_DIR}/quickjs-libc.c)
endif()

# QuickJS 是纯 C 代码
add_library(quickjs STATIC ${QUICKJS_SOURCES})

set_target_properties(quickjs PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

# 编译定义：不用 CONFIG_BIGNUM（2025 版已移除 bignum 扩展）
if(NOT MSVC)
    target_compile_options(quickjs PRIVATE -w)
    target_compile_definitions(quickjs PRIVATE
        _GNU_SOURCE
        CONFIG_VERSION="2026-06-04"
    )
else()
    target_compile_definitions(quickjs PRIVATE
        CONFIG_VERSION="2026-06-04"
    )
    # Sources are UTF-8; without this flag MSVC mis-decodes them under the
    # GBK system codepage (C4819) and corrupts string literals.
    # The shim wraps C99 math functions whose dllimport addresses (under /MD)
    # cannot be used in quickjs.c's static initializer tables (C2099/C4232).
    target_compile_options(quickjs PRIVATE /utf-8 "/FI${QUICKJS_SOURCE_DIR}/quickjs_msvc_shim.h")
endif()

# 只暴露 quickjs.h 给需要它的源文件（PRIVATE，防止 _GNU_SOURCE 传播到 C++ 目标）
target_include_directories(quickjs SYSTEM PRIVATE ${QUICKJS_SOURCE_DIR})
