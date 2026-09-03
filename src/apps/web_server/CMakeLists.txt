# OpenRead Web Server（Aria HttpAdapter + ViewModel 替代 Python Flask）
#
# 完整 C++ Web 后端：用 Aria 的 HTTP adapter + ViewModel 把 OpenRead 的
# 业务逻辑直接暴露给浏览器——零 Python、零 Flask。
#
# 运行：
#   ./openread_web_server [port] [static_root] [db_path]
#   例：./openread_web_server 9091 ../web openread.db
#   然后浏览器打开 http://127.0.0.1:9091

if(NOT TARGET aria::http)
    message(STATUS "openread_web_server skipped: aria::http not available (set ARIA_BUILD_HTTP=ON)")
    return()
endif()

add_executable(openread_web_server
    main.cpp
    json_helpers.cpp
    port_utils.cpp
    routes.cpp
    routes_misc.cpp
    routes_search.cpp
    routes_catalog.cpp
    routes_bookshelf.cpp
    routes_sources.cpp
    routes_rss.cpp
)

target_link_libraries(openread_web_server
    PRIVATE
        openread_vm       # ViewModel 层（PUBLIC 传递 openread/aria::core/aria::binding）
        aria::http        # HttpAdapter（PUBLIC 传递 aria::core/aria::abi/aria::binding）
        nlohmann_json::nlohmann_json  # JSON 序列化
        # libcurl_static 不需要显式链接：openread_vm → openread → CURL::libcurl 已传递
)

# cpp-httplib 由 aria::http 通过 PUBLIC BUILD_INTERFACE 传递；
# 这里的显式 SYSTEM include 是兜底，确保即使 Aria 改回 PRIVATE
# 也能编译。sqlite_modern_cpp 是本项目的直接依赖。
target_include_directories(openread_web_server
    PRIVATE
        SYSTEM ${CMAKE_SOURCE_DIR}/third_party/aria/third_party/cpp-httplib
        SYSTEM ${CMAKE_SOURCE_DIR}/third_party/sqlite_modern_cpp/hdr
)

target_compile_features(openread_web_server PRIVATE cxx_std_20)

# 把前端静态资源目录路径作为编译期常量传入
# 指向 bindings/web/openread/web/（完整前端）
target_compile_definitions(openread_web_server PRIVATE
    OPENREAD_WEB_ROOT="${CMAKE_SOURCE_DIR}/bindings/web/openread/web"
)

# 抑制 sqlite_modern_cpp / httplib 的弃用警告
if(NOT MSVC)
    target_compile_options(openread_web_server PRIVATE -Wno-deprecated-declarations)
endif()

# Apple ld "ignoring duplicate libraries" 警告：
# aria::http → aria::abi + aria::core → aria::abi 菱形依赖，Aria 上游问题，此处无害。
if(APPLE)
    target_link_options(openread_web_server PRIVATE -Wl,-w)
endif()
