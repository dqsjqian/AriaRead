# AriaRead Web Server（Mira HTTP/1.1 + ViewModel 替代 Python Flask）
#
# 完整 C++ Web 后端：把 AriaRead 的业务逻辑直接暴露给浏览器——零 Python、
# 零 Flask，也不再 vendored cpp-httplib（原先经 Aria HttpAdapter 带入）。
#
# 运行：
#   ./ariaread_web_server [port] [static_root] [db_path]
#   例：./ariaread_web_server 9091 ../web ariaread.db
#   然后浏览器打开 http://127.0.0.1:9091

if(NOT TARGET Mira::http)
    message(FATAL_ERROR "ariaread_web_server 需要 Mira："
        "先执行 python scripts/build.py deps，再配置（prefix 留空自动探测 "
        "build/deps/prefix）")
endif()

add_executable(ariaread_web_server
    main.cpp
    mira_server.cpp
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

target_link_libraries(ariaread_web_server
    PRIVATE
        ariaread_vm       # ViewModel 层（PUBLIC 传递 ariaread/aria::core/aria::binding）
        Mira::http    # HTTP/1.1（传递 Mira::core）
        Mira::transport  # TCP 监听与连接
        nlohmann_json::nlohmann_json  # JSON 序列化
        # SQLite is linked through ariaread's static-link interface.
)

# sqlite_modern_cpp 是本项目的直接依赖（header-only）。
target_include_directories(ariaread_web_server
    PRIVATE
        SYSTEM ${ARIAREAD_SQLITE_MODERN_CPP_INCLUDE_DIR}
)

    if(WIN32 AND ARIA_BUILD_SHARED)
        # 明确按目标拷贝运行时 DLL：Aria 是 SHARED，MinGW 的发现测试执行
        # exe 时必须能在同目录找到它们。$<TARGET_RUNTIME_DLLS> 在 MinGW 上
        # 可能展开为空，会让 copy_if_different 报用法错误，因此不用它。
        add_custom_command(TARGET ariaread_web_server POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:aria_abi>
                $<TARGET_FILE:aria_runtime>
                $<TARGET_FILE:aria_binding>
                $<TARGET_FILE_DIR:ariaread_web_server>
            COMMAND_EXPAND_LISTS)
    endif()

# Mira 和 Aria 的公开头要求 C++23；整个工程统一使用该标准。
target_compile_features(ariaread_web_server PRIVATE cxx_std_23)

# 每次构建服务时同步资源，包括仅修改 JS/CSS、没有触发重新链接的情况。
# 多配置生成器将资源放到 bin/<配置>/web，与可执行文件、Aria DLL 同目录。
set(ARIAREAD_WEB_OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
if(CMAKE_CONFIGURATION_TYPES)
    string(APPEND ARIAREAD_WEB_OUTPUT_DIR "/$<CONFIG>")
endif()
add_custom_target(ariaread_web_assets
    COMMAND "${CMAKE_COMMAND}"
        "-DARIAREAD_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "-DARIAREAD_OUTPUT_DIR=${ARIAREAD_WEB_OUTPUT_DIR}"
        "-DARIAREAD_MINGW=${MINGW}"
        "-DARIAREAD_DEPS_PREFIX=${ARIAREAD_DEPS_PREFIX}"
        "-DARIAREAD_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        -P "${CMAKE_SOURCE_DIR}/cmake/SyncWebRuntime.cmake"
    COMMENT "Syncing Web runtime in ${ARIAREAD_WEB_OUTPUT_DIR}"
    VERBATIM
)
add_dependencies(ariaread_web_server ariaread_web_assets)

# 构建目录本身就是可分发目录，加载同目录动态库，不依赖开发机绝对路径。
if(APPLE)
    set_target_properties(ariaread_web_server PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "@executable_path")
elseif(UNIX)
    set_target_properties(ariaread_web_server PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN")
endif()

# 抑制 sqlite_modern_cpp 的弃用警告
if(NOT MSVC)
    target_compile_options(ariaread_web_server PRIVATE -Wno-deprecated-declarations)
endif()
