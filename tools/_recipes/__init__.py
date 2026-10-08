"""AriaRead dependency recipes for aria-deps.

This module supplies everything project-specific: the recipe table,
the profile/TLS policy, post-install hooks and extra providers.
The aria-deps package provides only the mechanism.
"""

from __future__ import annotations

import sys
from dataclasses import replace
from pathlib import Path

from aria_deps import Dependency, ProjectConfig
from aria_deps import providers as _providers


# ── Post-install hooks (were hardcoded name branches in copy_licenses) ──

def json_attributions(prefix: Path, source: Path, dep: Dependency) -> list[str]:
    notices = set()
    for header in (source / 'include/nlohmann').rglob('*.hpp'):
        for line in header.read_text(encoding='utf-8').splitlines():
            if 'SPDX-FileCopyrightText:' in line or 'SPDX-License-Identifier:' in line:
                notices.add(line)
    if not notices:
        raise ValueError('JSON embedded copyright notices are missing')
    target = prefix / "share/licenses" / dep.name / 'ATTRIBUTIONS.txt'
    target.write_text('\n'.join(sorted(notices)) + '\n', encoding='utf-8')
    return ['ATTRIBUTIONS.txt']


def sqlite3_dedication(prefix: Path, source: Path, dep: Dependency) -> list[str]:
    # The official amalgamation carries a public-domain dedication, not a LICENSE.
    header = (source / 'sqlite3.h').read_text(encoding='utf-8')
    dedication, separator, _ = header.partition('*************************************************************************')
    if not separator or 'author disclaims copyright' not in dedication:
        raise ValueError('SQLite public-domain dedication needs review')
    target = prefix / "share/licenses" / dep.name / 'PUBLIC-DOMAIN.txt'
    target.write_text(dedication, encoding='utf-8')
    return ['PUBLIC-DOMAIN.txt']


def zlib_windows_fixup(prefix: Path, source: Path, dep: Dependency) -> None:
    if sys.platform != 'win32':
        return
    from aria_deps.deps_build import normalize_zlib_static
    normalize_zlib_static(prefix)
    for junk in ('lib/zlib.lib', 'lib/zlib.dll', 'lib/zlib1.dll',
                 'lib/libzlib.dll.a', 'bin/zlib.dll', 'bin/zlib1.dll', 'bin/libzlib.dll'):
        (prefix / junk).unlink(missing_ok=True)


# ── Generated CMakeLists for upstreams without a CMake build ──

GUMBO_CMAKE = """\
cmake_minimum_required(VERSION 3.16)
project(gumbo C)
# Upstream 0.10.1 sources live in src/; the public header is src/gumbo.h.
add_library(gumbo STATIC
    src/attribute.c src/char_ref.c src/error.c src/parser.c src/string_buffer.c
    src/string_piece.c src/tag.c src/tokenizer.c src/utf8.c src/util.c
    src/vector.c)
target_include_directories(gumbo PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
                                        $<INSTALL_INTERFACE:include>)
set_target_properties(gumbo PROPERTIES POSITION_INDEPENDENT_CODE ON)
install(TARGETS gumbo EXPORT GumboTargets ARCHIVE DESTINATION lib)
# gumbo.h includes tag_enum.h and other internal headers, so install them all.
install(DIRECTORY src/ DESTINATION include FILES_MATCHING PATTERN "*.h")
install(EXPORT GumboTargets FILE GumboTargets.cmake NAMESPACE gumbo::
        DESTINATION lib/cmake/Gumbo)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/GumboConfig.cmake
     "include(\\"\\${CMAKE_CURRENT_LIST_DIR}/GumboTargets.cmake\\")\\n")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/GumboConfig.cmake
        DESTINATION lib/cmake/Gumbo)
"""

SQLITE_CMAKE = """\
cmake_minimum_required(VERSION 3.16)
project(sqlite3 C)
add_library(sqlite3 STATIC sqlite3.c)
target_include_directories(sqlite3 PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                                          $<INSTALL_INTERFACE:include>)
set_target_properties(sqlite3 PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_ENABLE_COLUMN_METADATA SQLITE_ENABLE_JSON1 SQLITE_THREADSAFE=1)
if(UNIX AND NOT APPLE)
    find_library(SQLITE_DL dl)
    if(SQLITE_DL)
        target_link_libraries(sqlite3 PRIVATE ${SQLITE_DL})
    endif()
endif()
if(UNIX)
    find_package(Threads QUIET)
    if(Threads_FOUND)
        target_link_libraries(sqlite3 PRIVATE Threads::Threads)
    endif()
    find_library(SQLITE_M m)
    if(SQLITE_M)
        target_link_libraries(sqlite3 PRIVATE ${SQLITE_M})
    endif()
endif()
install(TARGETS sqlite3 EXPORT SQLite3Targets ARCHIVE DESTINATION lib)
install(FILES sqlite3.h sqlite3ext.h DESTINATION include)
install(EXPORT SQLite3Targets FILE SQLite3Targets.cmake NAMESPACE sqlite3::
        DESTINATION lib/cmake/SQLite3Amalgamation)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/SQLite3AmalgamationConfig.cmake
     "include(\\"\\${CMAKE_CURRENT_LIST_DIR}/SQLite3Targets.cmake\\")\\n"
     "add_library(sqlite3 ALIAS sqlite3::sqlite3)\\n")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/SQLite3AmalgamationConfig.cmake
        DESTINATION lib/cmake/SQLite3Amalgamation)
"""

SQLITE_MODERN_CPP_CMAKE = """\
cmake_minimum_required(VERSION 3.16)
project(sqlite_modern_cpp CXX)
add_library(sqlite_modern_cpp INTERFACE)
target_include_directories(sqlite_modern_cpp INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/hdr>
    $<INSTALL_INTERFACE:include>)
install(TARGETS sqlite_modern_cpp EXPORT SqliteModernCppTargets)
install(DIRECTORY hdr/ DESTINATION include FILES_MATCHING PATTERN "*.h")
install(EXPORT SqliteModernCppTargets FILE SqliteModernCppTargets.cmake
        NAMESPACE sqlite_modern_cpp:: DESTINATION lib/cmake/sqlite_modern_cpp)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/sqlite_modern_cpp-config.cmake
     "include(\\"\\${CMAKE_CURRENT_LIST_DIR}/SqliteModernCppTargets.cmake\\")\\n")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sqlite_modern_cpp-config.cmake
        DESTINATION lib/cmake/sqlite_modern_cpp)
"""

QUICKJS_CMAKE = """\
cmake_minimum_required(VERSION 3.16)
project(quickjs C)
add_library(quickjs STATIC
    quickjs.c libregexp.c libunicode.c cutils.c dtoa.c)
# AriaRead embeds the JS engine only; the CLI's os/std helpers are unused.
target_include_directories(quickjs PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                                          $<INSTALL_INTERFACE:include>)
set_target_properties(quickjs PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON
                                        POSITION_INDEPENDENT_CODE ON)
# CONFIG_VERSION is normally passed by the upstream Makefile (used directly
# inside quickjs.c); the CMake build must provide it. Keep GNU extensions
# visible on POSIX platforms.
target_compile_definitions(quickjs PRIVATE CONFIG_VERSION="@QUICKJS_VERSION@")
if(NOT MSVC)
    target_compile_definitions(quickjs PRIVATE _GNU_SOURCE)
endif()
if(MSVC)
    target_compile_options(quickjs PRIVATE /utf-8
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/quickjs_msvc_shim.h")
else()
    target_compile_options(quickjs PRIVATE -w)
endif()
if(UNIX AND NOT APPLE)
    find_library(QUICKJS_M m)
    if(QUICKJS_M)
        target_link_libraries(quickjs PRIVATE ${QUICKJS_M})
    endif()
endif()
install(TARGETS quickjs EXPORT QuickJSTargets ARCHIVE DESTINATION lib)
install(FILES quickjs.h cutils.h DESTINATION include)
install(EXPORT QuickJSTargets FILE QuickJSTargets.cmake NAMESPACE quickjs::
        DESTINATION lib/cmake/QuickJS)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/QuickJSConfig.cmake
     "include(\\"\\${CMAKE_CURRENT_LIST_DIR}/QuickJSTargets.cmake\\")\\n")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/QuickJSConfig.cmake
        DESTINATION lib/cmake/QuickJS)
"""




# ── Recipe table ──

RECIPES: tuple[Dependency, ...] = (
    Dependency(
        name="zlib", version="",
        url="",
        sha256="",
        license="Zlib", license_files=("LICENSE",), root="", kind="cmake",
        # Static only: zlib's CMake also produces a shared library whose import
        # library makes Windows consumers depend on zlib1.dll at runtime.
        options=("-DZLIB_BUILD_SHARED=OFF", "-DZLIB_BUILD_EXAMPLES=OFF",
                 "-DSKIP_INSTALL_FILES=OFF"),
        artifacts=("lib/libz.a", "include/zlib.h"),  # zlibstatic.lib on Windows; see artifact_present
        post_build=zlib_windows_fixup,
    ),
    Dependency(
        name="openssl", version="",
        url="",
        sha256="",
        license="Apache-2.0", license_files=("LICENSE.txt",), root="",
        kind="openssl",
        artifacts=("lib/libssl.a", "lib/libcrypto.a", "include/openssl/ssl.h"),
    ),
    Dependency(
        name="curl", version="",
        url="",
        sha256="",
        license="curl", license_files=("COPYING",), root="", kind="cmake",
        uses_libdir=True,
        options=(
            "-DBUILD_CURL_EXE=OFF", "-DBUILD_TESTING=OFF", "-DCURL_DISABLE_INSTALL=OFF",
            "-DBUILD_LIBCURL_DOCS=OFF", "-DBUILD_MISC_DOCS=OFF", "-DENABLE_CURL_MANUAL=OFF",
            "-DHTTP_ONLY=ON",
            "-DCURL_ENABLE_SSL=ON", "-DCURL_USE_OPENSSL=ON", "-DCURL_ZLIB=ON",
            "-DCURL_USE_LIBPSL=OFF", "-DCURL_USE_LIBSSH2=OFF", "-DCURL_ZSTD=OFF",
            "-DCURL_BROTLI=OFF", "-DUSE_NGHTTP2=OFF", "-DUSE_LIBIDN2=OFF",
            "-DCURL_DISABLE_LDAP=ON", "-DCURL_DISABLE_LDAPS=ON",
            '-DCURL_CA_PATH=none', '-DCURL_CA_BUNDLE=none',
        ),
        artifacts=("lib/libcurl.a", "include/curl/curl.h"),
        requires=("zlib", "openssl"),
    ),
    Dependency(
        name="json", version="",
        url="",
        sha256="",
        license="MIT", license_files=("LICENSE.MIT",), root="", kind="cmake",
        uses_libdir=True,
        options=("-DJSON_BuildTests=OFF",),
        artifacts=("include/nlohmann/json.hpp",),
        post_install=json_attributions,
    ),
    Dependency(
        name="sqlite3", version="",
        url="",
        sha256="",
        license="blessing (Public Domain)",
        license_files=(), root="", kind="generated",
        cmake_lists=SQLITE_CMAKE,
        artifacts=("lib/libsqlite3.a", "include/sqlite3.h"),
        post_install=sqlite3_dedication,
    ),
    Dependency(
        name="gumbo", version="",
        url="",
        sha256="",
        license="Apache-2.0", license_files=("COPYING",), root="",
        kind="generated", cmake_lists=GUMBO_CMAKE, patch="gumbo-0.10.1-msvc.patch",
        artifacts=("lib/libgumbo.a", "include/gumbo.h"),
    ),
    Dependency(
        name="quickjs", version="",
        url="",
        sha256="",
        license="MIT", license_files=("LICENSE",), root="",
        kind="generated", cmake_lists=QUICKJS_CMAKE,
        patch="quickjs-msvc-2026-06-04.patch",
        extra_files=("quickjs_msvc_shim.h",),
        artifacts=("lib/libquickjs.a", "include/quickjs.h"),
    ),
    Dependency(
        name="sqlite_modern_cpp", version="",
        url="",
        sha256="",
        license="MIT", license_files=("License.txt",), root="",
        kind="generated", cmake_lists=SQLITE_MODERN_CPP_CMAKE,
        patch="sqlite_modern_cpp-v3.2-ariaread.patch",
        artifacts=("include/sqlite_modern_cpp.h",),
    ),
    Dependency(
        name="doctest", version="",
        url="",
        sha256="",
        license="MIT", license_files=("LICENSE.txt",), root="",
        kind="cmake", uses_libdir=True,
        patch="doctest-2.5.3-cmake-utf8-discovery.patch",
        options=("-DDOCTEST_WITH_TESTS=OFF", "-DDOCTEST_WITH_MAIN_IN_STATIC_LIB=OFF"),
        artifacts=("include/doctest/doctest.h",),
    ),
    # aria is installed exactly like every other dependency. It differs only in
    # recipe options: its singleton ABI design keeps the compiled modules
    # shared, so the distribution directory carries aria's dylib/dll.
    Dependency(
        name="aria", version="",
        url="",
        sha256="",
        license="MIT", license_files=("LICENSE", "THIRD_PARTY_NOTICES.md"), root="", kind="git",
        uses_libdir=True,
        options=("-DARIA_BUILD_TESTS=OFF", "-DARIA_BUILD_BENCHMARK=OFF",
                 "-DARIA_BUILD_DOCS=OFF", "-DARIA_BUILD_SHARED=ON",
                 "-DARIA_BUILD_QT6=OFF", "-DARIA_BUILD_HTTP=OFF",
                 "-DARIA_HTTP_ENABLE_TLS=OFF", "-DARIA_BUILD_JNI=OFF",
                 "-DARIA_BUILD_APPKIT=OFF", "-DARIA_BUILD_UIKIT=OFF"),
        artifacts=("lib/cmake/aria/ariaConfig.cmake", "include/aria/aria.hpp"),
    ),
    Dependency(
        name="Mira", version="",
        url="",
        sha256="",
        license="MIT", license_files=("LICENSE", "THIRD_PARTY_NOTICES.md"), root="", kind="git",
        uses_libdir=True,
        options=("-DMIRA_BUILD_TESTS=OFF", "-DMIRA_BUILD_EXAMPLES=OFF", "-DMIRA_BUILD_BENCH=OFF",
                 "-DMIRA_ENABLE_TLS=OFF", "-DMIRA_ENABLE_WEBSOCKET=OFF",
                 "-DMIRA_ENABLE_HTTP2=OFF", "-DMIRA_ENABLE_HTTP3=OFF"),
        artifacts=("lib/cmake/Mira/MiraConfig.cmake", "include/mira/http/connection.hpp"),
    ),
)


# ── Patch version whitelist ──

PATCH_VERSIONS = {'gumbo': {'0.10.1'}, 'quickjs': {'2026-06-04'},
                  'sqlite_modern_cpp': {'3.2'}, 'doctest': {'2.5.3'}}




# ── Profile / TLS policy ──

def configure(recipes, profile='tests', tls_backend='auto'):
    """Select required components before resolving or downloading any sources."""
    if profile not in ('runtime', 'tests'):
        raise ValueError(f'Unknown dependency profile: {profile}')
    if tls_backend == 'auto':
        tls_backend = 'schannel' if sys.platform == 'win32' else 'openssl'
    if tls_backend not in ('openssl', 'schannel'):
        raise ValueError(f'Unknown TLS backend: {tls_backend}')
    if tls_backend == 'schannel' and sys.platform != 'win32':
        raise ValueError('Schannel requires Windows')
    result = []
    for recipe in recipes:
        if recipe.name == 'doctest' and profile == 'runtime':
            continue
        if recipe.name == 'openssl' and tls_backend == 'schannel':
            continue
        if recipe.name == 'curl':
            options = tuple(option for option in recipe.options
                            if not option.startswith('-DCURL_USE_OPENSSL='))
            options += (f'-DCURL_USE_OPENSSL={"ON" if tls_backend == "openssl" else "OFF"}',
                        f'-DCURL_USE_SCHANNEL={"ON" if tls_backend == "schannel" else "OFF"}')
            if sys.platform == 'darwin':
                options += ('-DUSE_APPLE_SECTRUST=ON',)
            recipe = replace(recipe, options=options,
                             requires=('zlib', 'openssl') if tls_backend == 'openssl' else ('zlib',))
        result.append(recipe)
    return result


# ── Project wiring ──

PROVIDERS = {
    "sqlite": _providers.resolve_sqlite,
    "quickjs": _providers.resolve_quickjs,
}


def make_config(patches_dir: Path | None = None) -> ProjectConfig:
    """Build the ProjectConfig for AriaRead."""
    return ProjectConfig(
        name="ariaread",
        recipes=RECIPES,
        configure=configure,
        providers=PROVIDERS,
        patches_dir=patches_dir,
        patch_versions=PATCH_VERSIONS,
    )
