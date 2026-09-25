# OpenRead Web Server（Continuo HTTP/1.1 + ViewModel 替代 Python Flask）
#
# 完整 C++ Web 后端：把 OpenRead 的业务逻辑直接暴露给浏览器——零 Python、
# 零 Flask，也不再 vendored cpp-httplib（原先经 Aria HttpAdapter 带入）。
#
# 运行：
#   ./openread_web_server [port] [static_root] [db_path]
#   例：./openread_web_server 9091 ../web openread.db
#   然后浏览器打开 http://127.0.0.1:9091

if(NOT TARGET continuo::http)
    message(FATAL_ERROR "openread_web_server 需要 Continuo："
        "先执行 python3 tools/ci/build_openread_deps.py，再用 "
        "-DCMAKE_PREFIX_PATH=<prefix> 配置")
endif()

add_executable(openread_web_server
    main.cpp
    continuo_server.cpp
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
        continuo::http    # HTTP/1.1（传递 continuo::core）
        continuo::transport  # TCP 监听与连接
        nlohmann_json::nlohmann_json  # JSON 序列化
        CURL::libcurl     # json_helpers.cpp 直接使用 curl 头文件与 API
        sqlite3           # sqlite_modern_cpp.h → sqlite3.h 的传递包含；引擎 PRIVATE
                          # 链接在 VS 生成器下不传播，与 viewmodels 同模式显式补齐
)

# 静态 OpenSSL（Continuo TLS 传递）在 Windows 上会拉到证书库后端
# winstore_store，它需要系统的 crypt32；MinGW 静态链接不会自动补系统库。
if(MINGW)
    # OpenSSL's exported interface places crypt32 *before* libcrypto.a,
    # which GNU ld (left-to-right) cannot use for libcrypto's own
    # CertOpenSystemStoreW references. Append it after the archives.
    target_link_options(openread_web_server PRIVATE -lcrypt32)
endif()

# sqlite_modern_cpp 是本项目的直接依赖（header-only）。
target_include_directories(openread_web_server
    PRIVATE
        SYSTEM ${OPENREAD_SQLITE_MODERN_CPP_INCLUDE_DIR}
)

    if(WIN32 AND ARIA_BUILD_SHARED)
        # 明确按目标拷贝运行时 DLL：Aria 是 SHARED，MinGW 的发现测试执行
        # exe 时必须能在同目录找到它们。$<TARGET_RUNTIME_DLLS> 在 MinGW 上
        # 可能展开为空，会让 copy_if_different 报用法错误，因此不用它。
        add_custom_command(TARGET openread_web_server POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:aria_abi>
                $<TARGET_FILE:aria_runtime>
                $<TARGET_FILE:aria_binding>
                $<TARGET_FILE:ZLIB::ZLIB>
                $<TARGET_FILE_DIR:openread_web_server>
            COMMAND_EXPAND_LISTS)
    endif()

# Continuo 以 C++23 发布（公开头用 std::expected），本目标的源码按 C++23 编译；
# 其余目标仍是 C++20，二者通过 libc++/libstdc++ 的稳定 ABI 链接。
target_compile_features(openread_web_server PRIVATE cxx_std_23)

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

# 抑制 sqlite_modern_cpp 的弃用警告
if(NOT MSVC)
    target_compile_options(openread_web_server PRIVATE -Wno-deprecated-declarations)
endif()

# Apple ld "ignoring duplicate libraries" 警告：
# aria::http → aria::abi + aria::core → aria::abi 菱形依赖，Aria 上游问题，此处无害。
if(APPLE)
    target_link_options(openread_web_server PRIVATE -Wl,-w)
endif()
