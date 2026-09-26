# AriaRead Core Library

set(ARIAREAD_SOURCES
    # 引擎核心（拆分自原始 engine.cpp）
    engine/engine_utils.cpp
    engine/engine_core.cpp
    engine/engine_validate.cpp
    engine/engine_search_all.cpp
    engine/engine_bookshelf.cpp
    engine/engine_download.cpp
    engine/engine_update_check.cpp
    engine/engine_export.cpp
    engine/engine_auto_source.cpp
    engine/engine_rss.cpp
    # 选择器（拆分自原始 selector.cpp）
    selector/gumbo_helper.cpp
    selector/selector_impl.cpp
    selector/selector_factory.cpp
    selector/css_engine.cpp
    selector/xpath_engine.cpp
    # 规则解析器
    rule/rule_analyzer.cpp
    rule/elements_single.cpp
    rule/inline_rule.cpp
    # 基础设施
    infra/bridge.cpp
    infra/js_runtime.cpp
    infra/http_proxy.cpp
    infra/http_client_curl.cpp
    infra/source_parser.cpp
    infra/analyze_url.cpp
    infra/database.cpp
)

# ──────────────────────────────────────────────
# 默认静态库
# ──────────────────────────────────────────────
if(ARIAREAD_BUILD_SHARED)
    add_library(ariaread SHARED ${ARIAREAD_SOURCES})
else()
    add_library(ariaread STATIC ${ARIAREAD_SOURCES})
endif()

# ──────────────────────────────────────────────
# 头文件搜索路径
# ──────────────────────────────────────────────
target_include_directories(ariaread
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        SYSTEM ${ARIAREAD_SQLITE_MODERN_CPP_INCLUDE_DIR}  # sqlite_modern_cpp（SYSTEM 抑制第三方警告）
)

# ──────────────────────────────────────────────
# 链接依赖
# ──────────────────────────────────────────────
target_link_libraries(ariaread
    PRIVATE ${ARIAREAD_QUICKJS_TARGET}
    PRIVATE nlohmann_json::nlohmann_json
    PRIVATE ${ARIAREAD_SQLITE_TARGET}
    PRIVATE ${ARIAREAD_GUMBO_TARGET}
)

# ──────────────────────────────────────────────
# 平台特定
# ──────────────────────────────────────────────
# C++20：抑制 sqlite_modern_cpp 头文件中 std::codecvt<char16_t> 的弃用警告
# （第三方 header-only 库，无法改其源码；codecvt 自 C++17 起弃用，C++20 告警更显著）
if(NOT MSVC)
    target_compile_options(ariaread PRIVATE -Wno-deprecated-declarations)
endif()

if(UNIX)
    target_link_libraries(ariaread PRIVATE pthread dl m)
    # macOS: iconv 在 libSystem 中，但需要显式链接
    if(APPLE)
        target_link_libraries(ariaread PRIVATE iconv)
    endif()
endif()

if(WIN32)
    target_link_libraries(ariaread PRIVATE ws2_32)
endif()

if(CURL_FOUND)
    target_link_libraries(ariaread PRIVATE CURL::libcurl)
    target_compile_definitions(ariaread PRIVATE ARIAREAD_HAS_CURL=1)
endif()

# 共享库：导出所有符号（macOS/Linux 默认 hidden）
if(ARIAREAD_BUILD_SHARED)
    # 不用 hidden visibility，C++ 类和 C API 都要导出
    set_target_properties(ariaread PROPERTIES
        CXX_VISIBILITY_PRESET "default"
        VISIBILITY_INLINES_HIDDEN OFF
    )
    if(WIN32)
        target_compile_definitions(ariaread PRIVATE ARIAREAD_EXPORTS)
    endif()
endif()
