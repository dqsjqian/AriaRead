# AriaRead ViewModel 层（基于 Aria 响应式 MVVM 框架）
#
# 职责分层：
#   - ariaread（C++ 引擎库，src/engine, src/selector 等）：纯业务执行器，不依赖 Aria
#   - ariaread_vm（本 target）：响应式 ViewModel 层，用 Aria 的
#       Property / Computed / ObservableList / Task / AsyncCommand / ViewModel
#       把引擎的回调式异步包装成各端 UI 可直接绑定的响应式状态
#
# 这样 C++17 风格的引擎与 C++20 协程/响应式 VM 层职责清晰分离，
# 各平台（Qt6 / AppKit / UIKit / Web）只需实现 IViewAdapter 即可复用同一 VM。

if(NOT TARGET aria::binding)
    message(STATUS "ariaread_vm skipped: Aria not available")
    return()
endif()

add_library(ariaread_vm STATIC
    search_view_model.cpp
    engine_search_adapter.cpp
    bookshelf_view_model.cpp
    engine_bookshelf_adapter.cpp
    reader_view_model.cpp
    engine_reader_adapter.cpp
    source_view_model.cpp
    engine_source_adapter.cpp
)

target_include_directories(ariaread_vm
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        # engine.h 经 database.h 传递包含 sqlite_modern_cpp.h，
        # 而该头在 src 中是 PRIVATE include（不传播给下游）。
        # 下游消费 engine.h 时需自行提供此路径（SYSTEM 抑制第三方警告）。
        SYSTEM ${ARIAREAD_SQLITE_MODERN_CPP_INCLUDE_DIR}
)

# database.h（被 engine.h 包含）还传递依赖 nlohmann/json.hpp，
# 该依赖在引擎中是 PRIVATE 链接。下游 VM 层消费 engine.h 时需补上。
# 直接链接 target 以获取其 INTERFACE include 路径，比硬编码路径更稳。
if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(ariaread_vm PRIVATE nlohmann_json::nlohmann_json)
endif()

# database.h 经 sqlite_modern_cpp.h 还传递依赖 sqlite3.h；引擎中同样是
# PRIVATE 链接。Ninja/Makefile 生成器下路径恰好经目录级 include 泄漏可见，
# VS 生成器严格执行 PRIVATE 边界不传播 —— 显式补齐，两个生成器行为对齐。
if(TARGET sqlite3)
    target_link_libraries(ariaread_vm PRIVATE sqlite3)
endif()

# 链接引擎 + Aria 三层（core/async 为 header-only 模板，binding 为共享库）
target_link_libraries(ariaread_vm
    PUBLIC
        ariaread
        aria::core
        aria::async
        aria::binding
)

target_compile_features(ariaread_vm PUBLIC cxx_std_20)

# 抑制 sqlite_modern_cpp 头中 std::codecvt 的 C++20 弃用警告（与 src 一致）
if(NOT MSVC)
    target_compile_options(ariaread_vm PRIVATE -Wno-deprecated-declarations)
endif()

# ──────────────────────────────────────────────
# Web Server（Mira HTTP/1.1 + ViewModel，取代 Aria HttpAdapter）
# ──────────────────────────────────────────────
if(TARGET Mira::http)
    add_subdirectory(${CMAKE_SOURCE_DIR}/src/apps/web_server ${CMAKE_BINARY_DIR}/apps/web_server)
endif()
