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

# 每次构建服务时同步资源，包括仅修改 JS/CSS、没有触发重新链接的情况。
# 多配置生成器将资源放到 bin/<配置>/web，与可执行文件、Aria DLL 同目录。
set(OPENREAD_WEB_OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
if(CMAKE_CONFIGURATION_TYPES)
    string(APPEND OPENREAD_WEB_OUTPUT_DIR "/$<CONFIG>")
endif()
add_custom_target(openread_web_assets
    COMMAND "${CMAKE_COMMAND}"
        "-DOPENREAD_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "-DOPENREAD_OUTPUT_DIR=${OPENREAD_WEB_OUTPUT_DIR}"
        "-DOPENREAD_MINGW=${MINGW}"
        "-DOPENREAD_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        -P "${CMAKE_SOURCE_DIR}/cmake/SyncWebRuntime.cmake"
    COMMENT "Syncing Web runtime in ${OPENREAD_WEB_OUTPUT_DIR}"
    VERBATIM
)
add_dependencies(openread_web_server openread_web_assets)

# 构建目录本身就是可分发目录，加载同目录动态库，不依赖开发机绝对路径。
if(APPLE)
    set_target_properties(openread_web_server PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "@executable_path")
elseif(UNIX)
    set_target_properties(openread_web_server PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN")
endif()

# 抑制 sqlite_modern_cpp / httplib 的弃用警告
if(NOT MSVC)
    target_compile_options(openread_web_server PRIVATE -Wno-deprecated-declarations)
endif()

# Apple ld "ignoring duplicate libraries" 警告：
# aria::http → aria::abi + aria::core → aria::abi 菱形依赖，Aria 上游问题，此处无害。
if(APPLE)
    target_link_options(openread_web_server PRIVATE -Wl,-w)
endif()
