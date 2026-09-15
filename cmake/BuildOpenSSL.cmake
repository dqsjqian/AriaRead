# ──────────────────────────────────────────────
# BuildOpenSSL.cmake
# 从 third_party/openssl 源码编译 OpenSSL 静态库，
# 导出 IMPORTED 目标供 curl 等依赖使用。
# ──────────────────────────────────────────────

include(ExternalProject)

set(OPENSSL_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/openssl")
set(OPENSSL_INSTALL_DIR "${CMAKE_BINARY_DIR}/_deps/openssl-install")
set(OPENSSL_INCLUDE_DIR "${OPENSSL_INSTALL_DIR}/include")

# 预先创建 include 目录，避免 CMake generate 阶段检查 IMPORTED target 的
# INTERFACE_INCLUDE_DIRECTORIES 时因目录不存在而报错
file(MAKE_DIRECTORY "${OPENSSL_INCLUDE_DIR}")

# 根据平台确定静态库路径
if(WIN32 AND NOT MINGW)
    set(OPENSSL_SSL_LIBRARY "${OPENSSL_INSTALL_DIR}/lib/libssl.lib")
    set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_INSTALL_DIR}/lib/libcrypto.lib")
else()
    set(OPENSSL_SSL_LIBRARY "${OPENSSL_INSTALL_DIR}/lib/libssl.a")
    set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_INSTALL_DIR}/lib/libcrypto.a")
endif()

# 根据平台选择 OpenSSL Configure 目标
if(APPLE)
    if(CMAKE_OSX_ARCHITECTURES STREQUAL "arm64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        set(_OPENSSL_TARGET "darwin64-arm64-cc")
    else()
        set(_OPENSSL_TARGET "darwin64-x86_64-cc")
    endif()
elseif(WIN32)
    if(MINGW)
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(_OPENSSL_TARGET "mingw64")
        else()
            set(_OPENSSL_TARGET "mingw")
        endif()
    else()
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(_OPENSSL_TARGET "VC-WIN64A")
        else()
            set(_OPENSSL_TARGET "VC-WIN32")
        endif()
    endif()
elseif(ANDROID)
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(_OPENSSL_TARGET "android-arm64")
    elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
        set(_OPENSSL_TARGET "android-arm")
    elseif(ANDROID_ABI STREQUAL "x86_64")
        set(_OPENSSL_TARGET "android-x86_64")
    elseif(ANDROID_ABI STREQUAL "x86")
        set(_OPENSSL_TARGET "android-x86")
    else()
        set(_OPENSSL_TARGET "android-arm64")
    endif()
else()
    # Linux / 其他 Unix
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        set(_OPENSSL_TARGET "linux-aarch64")
    else()
        set(_OPENSSL_TARGET "linux-x86_64")
    endif()
endif()

# 获取 CPU 核心数用于并行编译
include(ProcessorCount)
ProcessorCount(NPROC)
if(NPROC EQUAL 0)
    set(NPROC 4)
endif()

# 查找 perl（OpenSSL Configure 需要）
# 优先从 MSYS2 的 usr/bin 查找（与 MinGW 编译器配套）
get_filename_component(_COMPILER_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
get_filename_component(_COMPILER_PREFIX "${_COMPILER_DIR}" DIRECTORY)
get_filename_component(_COMPILER_ROOT "${_COMPILER_PREFIX}" DIRECTORY)
set(_MSYS2_PERL_CANDIDATES
    "${_COMPILER_ROOT}/usr/bin/perl.exe"
    "${_COMPILER_DIR}/../usr/bin/perl.exe"
    "C:/msys64/usr/bin/perl.exe"
    "C:/msys2/usr/bin/perl.exe"
)
set(_OPENSSL_PERL_COMMAND "")
foreach(_perl_path IN LISTS _MSYS2_PERL_CANDIDATES)
    get_filename_component(_perl_abs "${_perl_path}" ABSOLUTE)
    if(EXISTS "${_perl_abs}")
        set(_OPENSSL_PERL_COMMAND "${_perl_abs}")
        break()
    endif()
endforeach()
if(NOT _OPENSSL_PERL_COMMAND)
    unset(_OPENSSL_PERL_COMMAND)
    find_program(_OPENSSL_PERL_COMMAND perl REQUIRED)
endif()
get_filename_component(_PERL_DIR "${_OPENSSL_PERL_COMMAND}" DIRECTORY)

# 构建 Configure 参数
set(_OPENSSL_CONFIGURE_ARGS
    "${_OPENSSL_TARGET}"
    "no-shared"           # 只编译静态库
    "no-tests"            # 不编译测试
    "no-apps"             # 不编译 openssl 命令行工具
    "no-docs"             # 不生成文档
    "no-comp"             # 不需要压缩
    "no-dtls"             # 不需要 DTLS
    "no-engine"           # 不需要 ENGINE（OpenSSL 3.x 已弃用）
    "no-legacy"           # 不需要旧算法
    "--prefix=${OPENSSL_INSTALL_DIR}"
    "--libdir=lib"        # 统一输出到 lib/ 而非 lib64/
)

if(WIN32 AND NOT MINGW)
    # MSVC 分支注意三点：
    # 1) cl.exe 不认识 gcc 风格的 -Wno-* 警告抑制参数（D9002 unknown option），
    #    这些参数只为新版 Clang 准备，MSVC 下必须清空；
    # 2) GitHub Windows runner 不预装 NASM，而 VC-WIN64A 默认启用汇编，
    #    缺 nasm 会在 Configure 阶段直接失败。CI gate 场景用 no-asm 换取
    #    零外部依赖（性能损失对回归门禁无意义）。
    # 3) OpenSSL 生成的 makefile 里 LIB_CFLAGS 为
    #      /Zi /Fdossl_static.pdb /MT /Zl $(CNF_CFLAGS) $(CFLAGS)
    #    即所有 obj 共用一个 ossl_static.pdb。nmake 下 mspdbsrv 对同一 PDB
    #    的写入会随机失败，报 fatal error C1090（错误码 3 或 5，每次位置不同），
    #    构建中断且不可稳定复现。这里把 /Z7 通过 CFLAGS 追加到 /Zi 之后：
    #    /Z7 使调试信息内联进 obj、不再写 PDB，从根上消除竞争。
    set(_OPENSSL_EXTRA_CFLAGS "/Z7")
    list(APPEND _OPENSSL_CONFIGURE_ARGS "no-asm")
else()
    # 新版 Clang（Apple Clang 17+ / Clang 16+）对 C99 implicit-int 等更严格，
    # OpenSSL 3.x 的宏展开会触发这些错误，需要通过 CFLAGS 抑制
    set(_OPENSSL_EXTRA_CFLAGS "-Wno-implicit-int -Wno-incompatible-pointer-types -Wno-int-conversion -Wno-deprecated-non-prototype")
endif()

# 如果是 Android，需要设置 NDK 工具链
if(ANDROID)
    list(APPEND _OPENSSL_CONFIGURE_ARGS
        "-D__ANDROID_API__=${ANDROID_NATIVE_API_LEVEL}"
    )
    set(_OPENSSL_ENV "ANDROID_NDK_ROOT=${ANDROID_NDK}" "CFLAGS=${_OPENSSL_EXTRA_CFLAGS}")
else()
    set(_OPENSSL_ENV "CFLAGS=${_OPENSSL_EXTRA_CFLAGS}")
endif()

# 根据工具链选择构建命令，优先从编译器所在目录查找构建工具
get_filename_component(_COMPILER_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
if(WIN32 AND MINGW)
    # MinGW 下使用 MSYS2 bash + make（mingw32-make 无法处理 Makefile 中的 Unix shell 语法）
    set(_MSYS2_BASH_CANDIDATES
        "${_COMPILER_ROOT}/usr/bin/bash.exe"
        "${_COMPILER_DIR}/../usr/bin/bash.exe"
        "C:/msys64/usr/bin/bash.exe"
        "C:/msys2/usr/bin/bash.exe"
    )
    set(_OPENSSL_BASH_COMMAND "")
    foreach(_bash_path IN LISTS _MSYS2_BASH_CANDIDATES)
        get_filename_component(_bash_abs "${_bash_path}" ABSOLUTE)
        if(EXISTS "${_bash_abs}")
            set(_OPENSSL_BASH_COMMAND "${_bash_abs}")
            break()
        endif()
    endforeach()
    if(NOT _OPENSSL_BASH_COMMAND)
        unset(_OPENSSL_BASH_COMMAND)
        find_program(_OPENSSL_BASH_COMMAND bash REQUIRED)
    endif()
    # 注意：不要内嵌 `bash -c "export PATH=\"...\" && make ..."` —— 参数中间的
    # 转义引号会破坏 ExternalProject 生成的 step 脚本（CMake "Argument not
    # separated from preceding token" 警告 → 命令参数粘连 → bash 收到空命令
    # 静默退出 0，OpenSSL 从未被编译）。改调独立脚本，argv 分离，无转义问题。
    set(_OPENSSL_MINGW_HELPER "${CMAKE_SOURCE_DIR}/cmake/openssl-msys2.sh")
    set(_OPENSSL_BUILD_COMMAND
        "${_OPENSSL_BASH_COMMAND}" "${_OPENSSL_MINGW_HELPER}" build "${_COMPILER_DIR}" "${_PERL_DIR}" "${NPROC}"
    )
    set(_OPENSSL_INSTALL_COMMAND
        "${_OPENSSL_BASH_COMMAND}" "${_OPENSSL_MINGW_HELPER}" install "${_COMPILER_DIR}" "${_PERL_DIR}"
    )
    set(_OPENSSL_PATH_ENV "")
elseif(WIN32 AND NOT MINGW)
    find_program(_OPENSSL_MAKE_COMMAND nmake REQUIRED)
    set(_OPENSSL_BUILD_COMMAND ${_OPENSSL_MAKE_COMMAND})
    # 上面的 CFLAGS=/Z7 让每个 obj 自带调试信息、不再产出 ossl_static.pdb，
    # 但 OpenSSL 生成的 makefile 里 install_dev 仍会无条件执行
    #   perl util/copy.pl ossl_static.pdb "$(libdir)"
    # 文件缺失会让 nmake install_sw 以 "Can't Open ossl_static.pdb" 失败。
    # 这里在 install 前补一个占位文件：install 能正常完成，且因为 obj 里已有
    # CodeView 信息，丢掉这个（本来就是空的）PDB 不影响调试与链接。
    set(_OPENSSL_INSTALL_COMMAND
        ${CMAKE_COMMAND} -E touch "<BINARY_DIR>/ossl_static.pdb"
        COMMAND ${_OPENSSL_MAKE_COMMAND} install_sw
    )
    set(_OPENSSL_PATH_ENV "")
else()
    find_program(_OPENSSL_MAKE_COMMAND make REQUIRED)
    set(_OPENSSL_BUILD_COMMAND ${_OPENSSL_MAKE_COMMAND} -j${NPROC})
    set(_OPENSSL_INSTALL_COMMAND ${_OPENSSL_MAKE_COMMAND} install_sw)
    set(_OPENSSL_PATH_ENV "")
endif()

# 组装环境变量（MinGW 需要把编译器和 perl 目录加入 PATH）
set(_OPENSSL_CONFIGURE_COMMAND
    ${CMAKE_COMMAND} -E env
)
if(WIN32 AND MINGW)
    # 设置 PERL 环境变量，让 Configure 生成的 Makefile 使用正确的 perl 路径
    list(APPEND _OPENSSL_CONFIGURE_COMMAND
        --modify PATH=path_list_prepend:${_COMPILER_DIR}
        --modify PATH=path_list_prepend:${_PERL_DIR}
        "PERL=${_OPENSSL_PERL_COMMAND}"
    )
endif()

# 使用 ExternalProject 编译 OpenSSL
ExternalProject_Add(openssl_external
    SOURCE_DIR        "${OPENSSL_SOURCE_DIR}"
    BUILD_IN_SOURCE   0
    # OpenSSL 的 Configure 脚本需要在源码目录运行，但我们用 CONFIGURE_COMMAND 指定
    CONFIGURE_COMMAND ${_OPENSSL_CONFIGURE_COMMAND} ${_OPENSSL_ENV}
        "${_OPENSSL_PERL_COMMAND}" "${OPENSSL_SOURCE_DIR}/Configure" ${_OPENSSL_CONFIGURE_ARGS}
    BUILD_COMMAND     ${_OPENSSL_BUILD_COMMAND}
    INSTALL_COMMAND   ${_OPENSSL_INSTALL_COMMAND}
    BUILD_BYPRODUCTS  "${OPENSSL_SSL_LIBRARY}" "${OPENSSL_CRYPTO_LIBRARY}"
    LOG_CONFIGURE     TRUE
    LOG_BUILD         TRUE
    LOG_INSTALL       TRUE
)

# 创建 IMPORTED 目标
# OpenSSL::Crypto
add_library(OpenSSL::Crypto STATIC IMPORTED GLOBAL)
set_target_properties(OpenSSL::Crypto PROPERTIES
    IMPORTED_LOCATION "${OPENSSL_CRYPTO_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
)
add_dependencies(OpenSSL::Crypto openssl_external)

# OpenSSL::SSL
add_library(OpenSSL::SSL STATIC IMPORTED GLOBAL)
set_target_properties(OpenSSL::SSL PROPERTIES
    IMPORTED_LOCATION "${OPENSSL_SSL_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES OpenSSL::Crypto
)
add_dependencies(OpenSSL::SSL openssl_external)

# 设置 FindOpenSSL 兼容变量，供 curl 的 find_package(OpenSSL) 使用
set(OPENSSL_FOUND TRUE CACHE BOOL "" FORCE)
set(OPENSSL_INCLUDE_DIR "${OPENSSL_INCLUDE_DIR}" CACHE PATH "" FORCE)
set(OPENSSL_SSL_LIBRARY "${OPENSSL_SSL_LIBRARY}" CACHE FILEPATH "" FORCE)
set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_CRYPTO_LIBRARY}" CACHE FILEPATH "" FORCE)
set(OPENSSL_LIBRARIES "${OPENSSL_SSL_LIBRARY};${OPENSSL_CRYPTO_LIBRARY}" CACHE STRING "" FORCE)
set(OPENSSL_VERSION "4.0.2" CACHE STRING "" FORCE)
set(OPENSSL_ROOT_DIR "${OPENSSL_INSTALL_DIR}" CACHE PATH "" FORCE)
if(MINGW)
    set(LIB_EAY "${OPENSSL_CRYPTO_LIBRARY}" CACHE FILEPATH "" FORCE)
    set(SSL_EAY "${OPENSSL_SSL_LIBRARY}" CACHE FILEPATH "" FORCE)
elseif(WIN32)
    # MSVC 下 FindOpenSSL 不用 LIB_EAY/SSL_EAY，而是 find_library(LIB_EAY_RELEASE/
    # LIB_EAY_DEBUG) + SelectLibraryConfigurations(LIB_EAY)，再用其结果覆盖
    # OPENSSL_CRYPTO/SSL_LIBRARY。ExternalProject 构建期产物尚不存在，
    # find_library 必然 NOTFOUND → "missing: OPENSSL_CRYPTO_LIBRARY"。
    # 预填 *_RELEASE cache 让 find_library 跳过搜索，直接采用我们的安装路径。
    set(LIB_EAY_RELEASE "${OPENSSL_CRYPTO_LIBRARY}" CACHE FILEPATH "" FORCE)
    set(SSL_EAY_RELEASE "${OPENSSL_SSL_LIBRARY}" CACHE FILEPATH "" FORCE)
endif()

# 平台特定的链接依赖
if(APPLE)
    # macOS 需要链接 Security 和 CoreFoundation 框架
    set_property(TARGET OpenSSL::Crypto APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES "-framework Security" "-framework CoreFoundation")
elseif(WIN32)
    # Windows 需要链接系统库
    set_property(TARGET OpenSSL::Crypto APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES ws2_32 crypt32)
elseif(UNIX AND NOT ANDROID)
    # Linux 需要 pthread 和 dl
    set_property(TARGET OpenSSL::Crypto APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES pthread dl)
endif()

message(STATUS "OpenSSL will be built from source: ${OPENSSL_SOURCE_DIR}")
message(STATUS "OpenSSL install prefix: ${OPENSSL_INSTALL_DIR}")
message(STATUS "OpenSSL target platform: ${_OPENSSL_TARGET}")
