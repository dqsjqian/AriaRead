#!/usr/bin/env python3
"""显式构建 OpenRead 的固定版本第三方依赖；不由项目 CMake 自动调用。

设计原则与 Continuo 的 tools/ci/build_protocol_deps.py 一致：

  * **固定版本 + SHA256**：每个依赖锁定到一份官方发行归档，下载前后都校验
    哈希；哈希来自官方 release asset 摘要或本机实测（见 MANIFEST 注释）。
  * **许可证留档**：每份归档的许可证原文复制到 <prefix>/share/licenses/<名>/，
    并在 <prefix>/share/openread-deps/manifest.json 里记录来源与哈希，
    便于供应链审计。
  * **只写仓库 build 目录**：默认输出 <repo>/build/deps，绝不安装到系统目录。
  * **不由 CMake 触发**：配置前显式跑一次；CMake 侧只 find_package，不联网。

支持 Linux/macOS/Windows、Python 3.9+（仅标准库）、CMake 与 C/C++ 工具链；
OpenSSL 另需 perl 与 make（Windows 上为 nmake）。Windows 分支按 MSVC 编写，
由 OpenRead 的 Windows CI 验证，本机只验证过 macOS。

这是依赖的**唯一来源**：CMake 侧不再有 vendored third_party 回退，
没有前缀就配置失败并提示先跑本脚本。

用法：
    python3 tools/ci/build_openread_deps.py                 # 全部依赖
    python3 tools/ci/build_openread_deps.py --only zlib,json
    python3 tools/ci/build_openread_deps.py --offline       # 只用已缓存归档
之后配置项目：
    cmake -S . -B build -DCMAKE_PREFIX_PATH=$PWD/build/deps/prefix
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request
import zipfile
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath

REPO = Path(__file__).resolve().parents[2]
PATCHES = Path(__file__).resolve().parent / "patches"
CONTINUO_REPO = "dqsjqian/continuo"
# 固定到 v0.1.0 这个 tag 指向的 commit。不用 release asset 的字节哈希：
# 实测 GitHub 对同一 asset 两次下载给出的字节不同（263172 → 248968），哈希
# 钉不住。改用 git 按 commit SHA 校验——标签可以被挪动，commit 不能。
CONTINUO_TAG = "v0.1.5"
CONTINUO_REF = "1a749ff8e4bc74db54a264be47d001b5ec05e5b9"


@dataclass(frozen=True)
class Dependency:
    """一份固定版本的依赖来源。"""

    name: str
    version: str
    url: str
    sha256: str
    license: str
    #: 归档内许可证文件（相对归档根目录），会被复制到 share/licenses/<name>/
    license_files: tuple[str, ...]
    #: 归档内的顶层目录名；空串表示解压后自行定位唯一顶层目录
    root: str
    #: cmake | openssl | generated | continuo
    kind: str
    #: 传给 cmake 的额外参数
    options: tuple[str, ...] = ()
    #: kind == "generated" 时写入源码根目录的 CMakeLists.txt 内容
    cmake_lists: str = ""
    #: 安装后必须存在的文件（相对 prefix），用于自检
    artifacts: tuple[str, ...] = ()
    #: 构建前应用到源码树的补丁（tools/ci/patches 下）
    patch: str = ""
    #: 构建前复制到源码树的额外文件（tools/ci/patches 下 → 源码根同名文件）
    extra_files: tuple[str, ...] = ()
    #: 哈希来源说明，写进 manifest
    hash_note: str = "官方 release asset 摘要（GitHub API digest）"

    @property
    def archive_name(self) -> str:
        return self.url.rsplit("/", 1)[-1]


# ── 生成的 CMakeLists：gumbo / sqlite / quickjs 上游没有 CMake 构建 ──────────
#
# 放在脚本里而不是写回仓库源码树，是因为这些文件描述的是"如何把上游源码编成
# 一个静态库并安装"，属于取依赖的过程，不属于 OpenRead 自己的构建逻辑。

GUMBO_CMAKE = """\
cmake_minimum_required(VERSION 3.16)
project(gumbo C)
# 上游 0.10.1 的源文件在 src/ 下，公开头是 src/gumbo.h。
add_library(gumbo STATIC
    src/attribute.c src/char_ref.c src/error.c src/parser.c src/string_buffer.c
    src/string_piece.c src/tag.c src/tokenizer.c src/utf8.c src/util.c
    src/vector.c)
target_include_directories(gumbo PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
                                        $<INSTALL_INTERFACE:include>)
set_target_properties(gumbo PROPERTIES POSITION_INDEPENDENT_CODE ON)
install(TARGETS gumbo EXPORT GumboTargets ARCHIVE DESTINATION lib)
# gumbo.h 还会 include tag_enum.h 等内部头，所以整 src/ 的头一起装。
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
# quickjs-libc.c 提供 os/std 模块，依赖 POSIX API，MSVC 下不可移植；
# OpenRead 的 JsRuntime 不使用这些符号。
if(NOT MSVC)
    target_sources(quickjs PRIVATE quickjs-libc.c)
endif()
target_include_directories(quickjs PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                                          $<INSTALL_INTERFACE:include>)
set_target_properties(quickjs PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON
                                        POSITION_INDEPENDENT_CODE ON)
# CONFIG_VERSION 上游由 Makefile 传入（quickjs.c 里直接用），CMake 侧必须补上。
# _GNU_SOURCE：quickjs-libc.c 用到 environ 与 sighandler_t，strict c11 下不可见。
target_compile_definitions(quickjs PRIVATE CONFIG_VERSION="2026-06-04")
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
install(FILES quickjs.h quickjs-libc.h cutils.h DESTINATION include)
install(EXPORT QuickJSTargets FILE QuickJSTargets.cmake NAMESPACE quickjs::
        DESTINATION lib/cmake/QuickJS)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/QuickJSConfig.cmake
     "include(\\"\\${CMAKE_CURRENT_LIST_DIR}/QuickJSTargets.cmake\\")\\n")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/QuickJSConfig.cmake
        DESTINATION lib/cmake/QuickJS)
"""


DEPENDENCIES: tuple[Dependency, ...] = (
    Dependency(
        name="zlib", version="1.3.1",
        url="https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz",
        sha256="9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23",
        license="Zlib", license_files=("LICENSE",), root="zlib-1.3.1", kind="cmake",
        # 仅静态：zlib 的 CMake 默认同时产出共享库，Windows 消费端会通过
        # 导入库依赖 zlib1.dll，测试发现与服务器启动都得额外带 DLL。
        options=("-DZLIB_BUILD_SHARED=OFF", "-DZLIB_BUILD_EXAMPLES=OFF",
                 "-DSKIP_INSTALL_FILES=OFF"),
        artifacts=("lib/libz.a", "include/zlib.h"),  # Windows 上是 zlibstatic.lib，见 artifact_present
        hash_note="本机实测（上游 release 未发布摘要）",
    ),
    Dependency(
        name="openssl", version="4.0.2",
        url="https://github.com/openssl/openssl/releases/download/openssl-4.0.2/openssl-4.0.2.tar.gz",
        sha256="736b467530f916737b7031310ccb21d8218c6229e61e8e160cd1d3458cd543a8",
        license="Apache-2.0", license_files=("LICENSE.txt",), root="openssl-4.0.2",
        kind="openssl",
        artifacts=("lib/libssl.a", "lib/libcrypto.a", "include/openssl/ssl.h"),
    ),
    Dependency(
        name="curl", version="8.22.0",
        url="https://github.com/curl/curl/releases/download/curl-8_22_0/curl-8.22.0.tar.xz",
        sha256="f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7",
        license="curl", license_files=("COPYING",), root="curl-8.22.0", kind="cmake",
        options=(
            "-DBUILD_CURL_EXE=OFF", "-DBUILD_TESTING=OFF", "-DCURL_DISABLE_INSTALL=OFF",
            "-DCURL_ENABLE_SSL=ON", "-DCURL_USE_OPENSSL=ON", "-DCURL_ZLIB=ON",
            "-DCURL_USE_LIBPSL=OFF", "-DCURL_USE_LIBSSH2=OFF", "-DCURL_ZSTD=OFF",
            "-DCURL_BROTLI=OFF", "-DUSE_NGHTTP2=OFF", "-DUSE_LIBIDN2=OFF",
            "-DCURL_DISABLE_LDAP=ON", "-DCURL_DISABLE_LDAPS=ON",
            '-DCURL_CA_PATH=none', '-DCURL_CA_BUNDLE=none',
        ),
        artifacts=("lib/libcurl.a", "include/curl/curl.h"),
    ),
    Dependency(
        name="json", version="3.12.0",
        url="https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz",
        sha256="42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa",
        license="MIT", license_files=("LICENSE.MIT",), root="json", kind="cmake",
        options=("-DJSON_BuildTests=OFF",),
        artifacts=("include/nlohmann/json.hpp",),
        hash_note="本机实测（上游 release 未发布摘要）",
    ),
    Dependency(
        name="sqlite3", version="3.53.4",
        url="https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip",
        sha256="1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d",
        license="blessing（Public Domain）",
        license_files=(), root="sqlite-amalgamation-3530400", kind="generated",
        cmake_lists=SQLITE_CMAKE,
        artifacts=("lib/libsqlite3.a", "include/sqlite3.h"),
        hash_note="本机实测（sqlite.org 只发布 SHA3-256）",
    ),
    Dependency(
        name="gumbo", version="0.10.1",
        url="https://github.com/google/gumbo-parser/archive/refs/tags/v0.10.1.tar.gz",
        sha256="28463053d44a5dfbc4b77bcf49c8cee119338ffa636cc17fc3378421d714efad",
        license="Apache-2.0", license_files=("COPYING",), root="gumbo-parser-0.10.1",
        kind="generated", cmake_lists=GUMBO_CMAKE, patch="gumbo-0.10.1-msvc.patch",
        artifacts=("lib/libgumbo.a", "include/gumbo.h"),
        hash_note="本机实测（GitHub 源码归档，无官方摘要）",
    ),
    Dependency(
        name="quickjs", version="2026-06-04",
        url="https://bellard.org/quickjs/quickjs-2026-06-04.tar.xz",
        sha256="b376e839b322978313d929fd20663b11ba58b75df5a46c126dd19ea2fa70ad2a",
        license="MIT", license_files=("LICENSE",), root="quickjs-2026-06-04",
        kind="generated", cmake_lists=QUICKJS_CMAKE,
        patch="quickjs-msvc-2026-06-04.patch",
        extra_files=("quickjs_msvc_shim.h",),
        artifacts=("lib/libquickjs.a", "include/quickjs.h"),
        hash_note="本机实测（bellard.org 只提供 tarball）",
    ),
    Dependency(
        name="sqlite_modern_cpp", version="3.2",
        url="https://github.com/aminroosta/sqlite_modern_cpp/archive/refs/tags/v3.2.tar.gz",
        sha256="6a741482c0ef474adfc84260b5507d480d5b8e528e67e059284c2f1f9f986d74",
        license="MIT", license_files=("License.txt",), root="sqlite_modern_cpp-3.2",
        kind="generated", cmake_lists=SQLITE_MODERN_CPP_CMAKE,
        patch="sqlite_modern_cpp-v3.2-openread.patch",
        artifacts=("include/sqlite_modern_cpp.h",),
        hash_note="本机实测（GitHub 源码归档，无官方摘要）",
    ),
    Dependency(
        name="doctest", version="2.4.12",
        url="https://github.com/doctest/doctest/archive/refs/tags/v2.4.12.tar.gz",
        sha256="73381c7aa4dee704bd935609668cf41880ea7f19fa0504a200e13b74999c2d70",
        license="MIT", license_files=("LICENSE.txt",), root="doctest-2.4.12",
        kind="cmake",
        options=("-DDOCTEST_WITH_TESTS=OFF", "-DDOCTEST_WITH_MAIN_IN_STATIC_LIB=OFF"),
        artifacts=("include/doctest/doctest.h",),
        hash_note="本机实测（GitHub 源码归档，无官方摘要）",
    ),
    Dependency(
        name="continuo", version=CONTINUO_TAG,
        url=f"https://github.com/{CONTINUO_REPO}.git",
        sha256="",  # 由 git 按 CONTINUO_REF 校验，不用归档字节哈希
        license="MIT", license_files=("LICENSE",), root="", kind="git",
        hash_note=f"git 检出校验：tag {CONTINUO_TAG} 必须指向 commit {CONTINUO_REF}",
    ),
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(cache: Path, dependency: Dependency, offline: bool) -> Path:
    """下载（或复用）归档并校验 SHA256；Continuo 走 GitHub API 以取到私有仓库。"""
    archive = cache / dependency.archive_name
    expected = dependency.sha256
    if archive.exists():
        if not archive.is_file() or archive.is_symlink():
            raise ValueError(f"缓存路径不是普通文件：{archive}")
        if expected and sha256(archive) != expected:
            raise ValueError(f"缓存 SHA256 校验失败，未覆盖文件：{archive}")
        print(f"复用已校验缓存：{archive.name}", flush=True)
        return archive
    if offline:
        raise ValueError(f"离线缓存缺失：{archive}")

    request = urllib.request.Request(dependency.url,
                                     headers={"User-Agent": "openread-deps"})
    print(f"下载：{dependency.url}\n期望 SHA256：{expected or '(未固定，稍后打印实测值)'}",
          flush=True)
    # 下载中断或校验失败只删除本次临时文件，不覆盖已有归档。
    with tempfile.TemporaryDirectory(prefix="download-", dir=cache) as temporary:
        candidate = Path(temporary) / dependency.archive_name
        try:
            with urllib.request.urlopen(request, timeout=120) as response, \
                    candidate.open("wb") as output:
                shutil.copyfileobj(response, output)
        except urllib.error.HTTPError as error:  # noqa: PERF203
            raise ValueError(f"下载失败（HTTP {error.code}）：{dependency.url}") from error
        actual = sha256(candidate)
        if expected and actual != expected:
            raise ValueError(f"下载 SHA256 校验失败：{dependency.url}\n实测：{actual}")
        if not expected:
            print(f"未固定哈希，实测 SHA256：{actual}", flush=True)
        candidate.replace(archive)
    return archive


def extract(archive: Path, destination: Path, root_name: str) -> Path:
    """安全解压：拒绝绝对路径、越界成员与非常规文件类型。"""
    def safe(member_name: str) -> PurePosixPath:
        path = PurePosixPath(member_name)
        if (path.is_absolute() or ".." in path.parts or "\\" in member_name
                or not path.parts):
            raise ValueError(f"拒绝不安全归档成员：{member_name}")
        target = (destination / member_name).resolve()
        if destination.resolve() not in target.parents:
            raise ValueError(f"拒绝路径越界：{member_name}")
        return path

    def write(target: Path, data, executable: bool) -> None:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        target.chmod(0o755 if executable else 0o644)

    if archive.suffix == ".zip" or archive.name.endswith(".zip"):
        with zipfile.ZipFile(archive) as package:
            for info in package.infolist():
                safe(info.filename)
            for info in package.infolist():
                if info.is_dir():
                    continue
                write(destination / info.filename, package.read(info),
                      bool(info.external_attr >> 16 & 0o111))
    else:
        with tarfile.open(archive) as package:
            for member in package.getmembers():
                safe(member.name)
            for member in package.getmembers():
                if member.isdir():
                    continue
                if not member.isfile():
                    raise ValueError(f"拒绝非常规归档成员：{member.name}")
                source = package.extractfile(member)
                if source is None:
                    raise ValueError(f"无法读取归档成员：{member.name}")
                with source:
                    write(destination / member.name, source.read(),
                          bool(member.mode & 0o111))
    if root_name:
        return destination / root_name
    entries = [entry for entry in destination.iterdir() if entry.is_dir()]
    if len(entries) != 1:
        raise ValueError(f"无法定位归档顶层目录：{destination}")
    return entries[0]


def run(command: list[str], cwd: Path | None = None) -> None:
    print("+ " + shlex.join(command), flush=True)
    subprocess.run(command, check=True, cwd=str(cwd) if cwd else None)


def apply_patch(source: Path, patch_file: Path) -> None:
    """应用 unified diff（-p1）。

    自己实现而不是调 `patch`：脚本只依赖 Python 标准库，`patch` 在 Windows 上
    并不总是存在。已经套用过的 hunk 直接跳过，所以可以重复运行。
    """
    lines = patch_file.read_text(encoding="utf-8").splitlines()
    index = 0
    while index < len(lines):
        while index < len(lines) and not lines[index].startswith("--- "):
            index += 1
        if index >= len(lines):
            return
        old_name = lines[index][4:].strip()
        new_name = lines[index + 1][4:].strip() if index + 1 < len(lines) else ""
        index += 2

        def strip_prefix(name: str) -> str:
            # a/quickjs.c → quickjs.c；时间戳后缀一并去掉。
            name = name.split("\t", 1)[0]
            return name[2:] if name.startswith(("a/", "b/")) else name

        target = source / strip_prefix(new_name or old_name)
        original = target.read_text(encoding="utf-8").splitlines() if target.is_file() else []
        updated = list(original)

        while index < len(lines) and lines[index].startswith("@@"):
            header = lines[index]
            index += 1
            numbers = header.split("@@")[1].strip()
            old_start = int(numbers.split(",")[0].lstrip("-")) - 1
            removed, added = [], []
            while index < len(lines) and not lines[index].startswith(("@@", "--- ", "diff ")):
                line = lines[index]
                index += 1
                if line.startswith("\\"):
                    continue
                if line.startswith("+"):
                    added.append(line[1:])
                elif line.startswith("-"):
                    removed.append(line[1:])
                elif line.startswith(" "):
                    added.append(line[1:])
                    removed.append(line[1:])
                elif line == "":
                    added.append("")
                    removed.append("")
                else:
                    break
            if not removed:
                # 新增文件的 hunk：内容已在就跳过（否则每次运行都会再插一份）。
                if any(updated[start:start + len(added)] == added
                       for start in range(len(updated) - len(added) + 1)):
                    continue
                updated[old_start:old_start] = added
                continue
            position = next((start for start in range(len(updated) - len(removed) + 1)
                             if updated[start:start + len(removed)] == removed), None)
            if position is None:
                # 已套用过：文件里已经能找到这段新内容，跳过即可（可重复运行）。
                if any(updated[start:start + len(added)] == added
                       for start in range(len(updated) - len(added) + 1)):
                    continue
                raise ValueError(f"补丁无法应用：{patch_file.name} → {target.name}")
            updated[position:position + len(removed)] = added

        target.write_text("\n".join(updated) + ("\n" if updated else ""), encoding="utf-8")


def prepare_source(source: Path, dependency: Dependency) -> None:
    if dependency.patch:
        patch = PATCHES / dependency.patch
        if not patch.is_file():
            raise ValueError(f"补丁缺失：{patch}")
        apply_patch(source, patch)
    for name in dependency.extra_files:
        extra = PATCHES / name
        if not extra.is_file():
            raise ValueError(f"额外文件缺失：{extra}")
        shutil.copyfile(extra, source / name)
    if dependency.cmake_lists:
        (source / "CMakeLists.txt").write_text(dependency.cmake_lists, encoding="utf-8")


def copy_licenses(prefix: Path, source: Path, dependency: Dependency) -> list[str]:
    target = prefix / "share/licenses" / dependency.name
    target.mkdir(parents=True, exist_ok=True)
    copied = []
    for relative in dependency.license_files:
        origin = source / relative
        if not origin.is_file():
            raise ValueError(f"{dependency.name} 许可证文件缺失：{origin}")
        shutil.copyfile(origin, target / origin.name)
        copied.append(origin.name)
    return copied


def windows_toolchain() -> str:
    """Windows 上区分 MSVC 与 MinGW/MSYS2：两者的 OpenSSL 构建方式不同。"""
    if shutil.which("cl") and shutil.which("nmake"):
        return "msvc"
    return "mingw"


def build_cmake(source: Path, build: Path, prefix: Path, jobs: int,
                dependency: Dependency, common: list[str]) -> None:
    # 允许调用方用 CMAKE_GENERATOR 环境变量换生成器（如本机 Ninja + cl）；
    # CI 不设该变量，默认行为不变。
    env_gen = os.environ.get("CMAKE_GENERATOR", "").strip().lower()
    msvc_vs = (sys.platform == "win32" and windows_toolchain() == "msvc"
               and env_gen != "ninja")
    configure = ["cmake"]
    if msvc_vs:
        # Visual Studio 生成器默认出 Win32；本项目全平台只要 x64。
        configure += ["-A", "x64"]
    run([*configure, "-S", str(source), "-B", str(build), *common, *dependency.options])
    # Visual Studio 是多配置生成器：不指定 --config 会编出 Debug，而 install
    # 默认按 Release 去找，两者对不上就直接失败。
    config = ["--config", "Release"] if msvc_vs else []
    run(["cmake", "--build", str(build), "--parallel", str(jobs), *config])
    run(["cmake", "--install", str(build), *config])


def build_openssl(source: Path, prefix: Path, jobs: int) -> None:
    if shutil.which("perl") is None:
        raise ValueError("OpenSSL 的 Configure 需要 perl（Windows 上可用 Strawberry Perl）")
    windows = sys.platform == "win32"
    toolchain = windows_toolchain() if windows else ""
    make = "nmake" if toolchain == "msvc" else "make"
    if shutil.which(make) is None:
        raise ValueError(f"OpenSSL 源码构建需要 {make}")
    if toolchain == "msvc":
        target = ["VC-WIN64A"]
    elif toolchain == "mingw":
        target = ["mingw64"]
    else:
        target = []
    # no-asm：避免 Windows 上再依赖 NASM；静态库只给 libcurl 用，慢一点无所谓。
    run(["perl", str(source / "Configure"), *target, f"--prefix={prefix}",
         f"--openssldir={prefix}/ssl", "--libdir=lib", "no-shared", "no-tests",
         # no-winstore: the Windows certificate-store provider pulls
         # crypt32 into every static consumer, and OpenSSL's exported
         # CMake interface lists crypt32 *before* libcrypto.a, which GNU
         # ld (left-to-right) cannot use. Nothing in OpenRead reads the
         # system store — curl ships its own CA bundle.
         "no-winstore",
         "no-docs", "no-apps", "no-asm"], cwd=source)
    if toolchain == "msvc":
        run([make], cwd=source)
        run([make, "install_sw"], cwd=source)
    else:
        run([make, "-j", str(jobs)], cwd=source)
        run([make, "install_sw"], cwd=source)


def artifact_present(prefix: Path, artifact: str) -> bool:
    """产物存在性检查：头文件精确匹配，库文件按平台后缀模糊匹配。

    Linux 的 OpenSSL 默认装进 lib64，MSVC 的静态库叫 zlibstatic.lib、动态库
    带 d 后缀——逐个平台列清单会把脚本变成一张维护不完的表，所以库只比对
    文件名主干。
    """
    candidate = prefix / artifact
    if candidate.exists():
        return True
    if artifact.endswith(".h"):
        return False
    directory = candidate.parent
    if not directory.is_dir():
        return False
    stem = candidate.name
    for marker in ("lib",):
        if stem.startswith(marker):
            stem = stem[len(marker):]
            break
    stem = stem.rsplit(".", 1)[0]
    suffixes = (".a", ".lib", ".so", ".dylib")
    return any(entry.is_file() and stem in entry.name.lower()
               and entry.name.lower().endswith(suffixes)
               for entry in directory.iterdir())


def fetch_git(work: Path, dependency: Dependency, offline: bool) -> Path:
    """按固定 commit 取 Continuo。

    不下载归档而是 git clone，是因为归档的字节在 GitHub 侧不稳定；git 用
    commit SHA 做完整性校验，比"下载后比对哈希"更强：标签可以被挪动，
    commit 不能。
    """
    target = work / "src" / dependency.name

    def head() -> str:
        if not (target / ".git").exists():
            return ""
        completed = subprocess.run(["git", "-C", str(target), "rev-parse", "HEAD"],
                                   capture_output=True, text=True)
        return completed.stdout.strip() if completed.returncode == 0 else ""

    if head() == CONTINUO_REF:
        print(f"复用已校验的 Continuo 检出：{target}", flush=True)
        return target
    if offline:
        raise ValueError(f"离线模式缺少已校验的 Continuo 检出：{target}")
    if target.exists():
        raise ValueError(f"{target} 与固定 commit 不一致，请手动删除后重跑")
    run(["git", "clone", "--depth", "1", "--branch", dependency.version,
         dependency.url, str(target)])
    if head() != CONTINUO_REF:
        raise ValueError(f"Continuo tag {dependency.version} 指向 {head()}，"
                         f"与固定 commit {CONTINUO_REF} 不一致")
    return target


def positive_jobs(value: str) -> int:
    try:
        number = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("--jobs 必须是 1 到 256 的整数") from error
    if not 1 <= number <= 256:
        raise argparse.ArgumentTypeError("--jobs 必须是 1 到 256 的整数")
    return number


def output_path(value: Path) -> Path:
    path = value.expanduser().resolve()
    if path in (Path.home(), REPO) or path in REPO.parents:
        raise ValueError(f"拒绝以主目录或仓库根目录作为输出目录：{path}")
    for system in ("/usr", "/bin", "/sbin", "/etc", "/System", "/Library", "/opt"):
        root = Path(system)
        if path == root or root in path.parents:
            raise ValueError(f"拒绝向系统目录安装：{path}")
    if path.exists() and not path.is_dir():
        raise ValueError(f"输出路径不是目录：{path}")
    if ";" in str(path):
        raise ValueError("输出路径不能包含 CMake 列表分隔符 ';'")
    return path


def main() -> None:
    # Windows 控制台默认是 cp1252 之类的本地编码，脚本里的中文日志会直接
    # UnicodeEncodeError。强制 UTF-8（失败时退化为替换字符，不因此中断构建）。
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, OSError, ValueError):
            pass
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--path", type=Path, default=REPO / "build/deps",
                        help="缓存与临时构建根目录，默认仓库 build/deps")
    parser.add_argument("--prefix", type=Path,
                        help="安装目录，默认 <path>/prefix；不得指向系统目录")
    parser.add_argument("--jobs", type=positive_jobs,
                        default=min(os.cpu_count() or 1, 8), help="并行任务数，1 到 256")
    parser.add_argument("--only", default="",
                        help="只构建指定依赖，逗号分隔；默认全部")
    parser.add_argument("--offline", action="store_true",
                        help="禁止下载，只使用 <path>/cache 中已校验的归档")
    args = parser.parse_args()

    if not (sys.platform.startswith("linux") or sys.platform == "darwin"
            or sys.platform == "win32"):
        parser.error(f"未支持的平台：{sys.platform}")
    if sys.platform == "win32":
        print("注意：Windows 分支按 MSVC + nmake 编写，尚未在本机验证；"
              "出问题请以 Windows CI 的输出为准。", flush=True)
    if shutil.which("cmake") is None:
        parser.error("请先安装 CMake 和 C/C++ 工具链")

    selected = {item.strip() for item in args.only.split(",") if item.strip()}
    dependencies = [item for item in DEPENDENCIES
                    if not selected or item.name in selected]
    unknown = selected - {item.name for item in DEPENDENCIES}
    if unknown:
        parser.error(f"未知依赖：{', '.join(sorted(unknown))}")

    work = output_path(args.path)
    prefix = output_path(args.prefix or work / "prefix")
    cache = work / "cache"
    if prefix == cache or cache in prefix.parents or prefix in cache.parents:
        parser.error("--prefix 不得与归档缓存目录重叠")
    cache.mkdir(parents=True, exist_ok=True)

    common = [f"-DCMAKE_INSTALL_PREFIX={prefix}", "-DCMAKE_INSTALL_LIBDIR=lib",
              "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
              "-DBUILD_SHARED_LIBS=OFF",
              f"-DCMAKE_PREFIX_PATH={prefix}"]
    if shutil.which("cl"):
        # CJK-locale Windows (cp936): cl defaults to the ANSI code page and
        # UTF-8 sources trip C4819, a hard error under continuo's /WX. Inject
        # /utf-8 through the CL env var -- replacing CMAKE_CXX_FLAGS instead
        # would wipe CMake's /EHsc /GR defaults and turn C4530 into a hard
        # error for any exception-using target.
        os.environ["CL"] = (os.environ.get("CL", "") + " /utf-8").strip()
        print("CL=/utf-8 injected (CJK-locale source-encoding safety)",
              flush=True)

    manifest = []
    # 源码与中间产物放在 <work>/src、<work>/build 下复用，而不是每次解压到临时
    # 目录再整棵删除：批量删除上万文件既慢又容易被安全策略拦下。版本变化由
    # 戳文件检测，检测到不一致就要求人工确认后再删。
    sources = work / "src"
    builds = work / "build"
    sources.mkdir(parents=True, exist_ok=True)
    builds.mkdir(parents=True, exist_ok=True)
    for dependency in dependencies:
        print(f"\n=== {dependency.name} {dependency.version} ===", flush=True)
        # 断点续跑：前缀已有全部预期产物就不再重建（openssl 全量重建约 40 分钟），
        # 只补 manifest 记录；manifest.json 在 CMake 侧仅做存在性检查。
        # 注意 artifacts 为空的依赖（如 continuo）不能跳过，必须每次构建。
        if dependency.artifacts and all(artifact_present(prefix, artifact)
                                        for artifact in dependency.artifacts):
            license_dir = prefix / "share" / "licenses" / dependency.name
            licenses = (sorted(p.name for p in license_dir.rglob("*")
                               if p.is_file()) if license_dir.is_dir() else [])
            version = dependency.version
            if dependency.kind == "git":
                version = f"{dependency.version} ({CONTINUO_REF})"
                digest = CONTINUO_REF
            else:
                digest = dependency.sha256 or "installed"
            manifest.append({
                "name": dependency.name,
                "version": version,
                "url": dependency.url,
                "sha256": digest,
                "sha256_source": dependency.hash_note,
                "license": dependency.license,
                "license_files": licenses,
                "artifacts": list(dependency.artifacts),
                "expected_sha256": digest,
            })
            print(f"跳过 {dependency.name}（前缀已有产物）", flush=True)
            continue
        if dependency.kind == "git":
            source = fetch_git(work, dependency, args.offline)
            build_cmake(source, builds / dependency.name, prefix, args.jobs,
                        dependency, common)
            licenses = copy_licenses(prefix, source, dependency)
            missing = [artifact for artifact in dependency.artifacts
                       if not artifact_present(prefix, artifact)]
            if missing:
                raise ValueError(f"{dependency.name} 缺少预期产物：{missing}")
            manifest.append({
                "name": dependency.name,
                "version": f"{dependency.version} ({CONTINUO_REF})",
                "url": dependency.url,
                "sha256": CONTINUO_REF,
                "sha256_source": dependency.hash_note,
                "license": dependency.license,
                "license_files": licenses,
                "artifacts": list(dependency.artifacts),
                "expected_sha256": CONTINUO_REF,
            })
            continue
        archive = download(cache, dependency, args.offline)
        digest = sha256(archive)
        stamp = sources / f"{dependency.name}.stamp"
        source = None
        if stamp.is_file():
            fields = stamp.read_text(encoding="utf-8").split()
            if len(fields) == 3 and fields[0] == dependency.version and fields[1] == digest:
                candidate = sources / fields[2]
                if not candidate.is_dir():
                    raise ValueError(f"戳文件指向的源码目录缺失：{candidate}")
                source = candidate
                print(f"复用已解压源码：{source}", flush=True)
            else:
                raise ValueError(
                    f"{dependency.name} 已解压的版本与固定版本不一致：请先手动删除 "
                    f"{sources / (dependency.root or dependency.name)} 与 {stamp} 后重跑")
        if source is None:
            if dependency.root:
                source = extract(archive, sources, dependency.root)
            else:  # 顶层目录名未知（GitHub 按 commit 生成的归档）
                holder = sources / dependency.name
                holder.mkdir(parents=True, exist_ok=True)
                source = extract(archive, holder, "")
            stamp.write_text(
                f"{dependency.version} {digest} {source.relative_to(sources).as_posix()}",
                encoding="utf-8")
        # 生成的 CMakeLists / 本地补丁每次都重新写入与套用：内容由本脚本决定，
        # 源码树里的旧版本不该被默默沿用（patch 已套过时 --forward 会跳过）。
        prepare_source(source, dependency)
        if dependency.kind in ("cmake", "generated", "continuo"):
            build_cmake(source, builds / dependency.name, prefix, args.jobs,
                        dependency, common)
        elif dependency.kind == "openssl":
            build_openssl(source, prefix, args.jobs)
        else:
            raise ValueError(f"未知构建方式：{dependency.kind}")
        licenses = copy_licenses(prefix, source, dependency)
        if dependency.name == "zlib" and sys.platform == "win32":
            # zlib 的 CMake 在 Windows 上即使关掉 ZLIB_BUILD_SHARED 也会装出
            # 共享库与导入库；FindZLIB 会优先链到导入库，让所有消费端在运行
            # 时依赖 zlib.dll。删掉共享产物，只留静态库，让 FindZLIB 落到
            # zlibstatic/libz。
            for junk in ("lib/zlib.lib", "lib/zlib.dll", "lib/zlib1.dll",
                         "lib/libzlib.dll.a", "bin/zlib.dll",
                         "bin/zlib1.dll", "bin/libzlib.dll"):
                stale = prefix / junk
                if stale.exists():
                    stale.unlink()
                    print(f"  移除共享产物：{stale}")
        missing = [artifact for artifact in dependency.artifacts
                   if not artifact_present(prefix, artifact)]
        if missing:
            raise ValueError(f"{dependency.name} 缺少预期产物：{missing}")
        manifest.append({
            "name": dependency.name,
            "version": dependency.version,
            "url": dependency.url,
            "sha256": digest,
            "sha256_source": dependency.hash_note,
            "license": dependency.license,
            "license_files": licenses,
            "artifacts": list(dependency.artifacts),
            "expected_sha256": dependency.sha256 or None,
        })

    manifest_dir = prefix / "share/openread-deps"
    manifest_dir.mkdir(parents=True, exist_ok=True)
    payload = {
        "generated_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "generator": "tools/ci/build_openread_deps.py",
        "dependencies": manifest,
    }
    (manifest_dir / "manifest.json").write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    print(f"\n完成。头文件、静态库与许可证位于：{prefix}", flush=True)
    print("配置项目时显式传入：", flush=True)
    print(f"  -DCMAKE_PREFIX_PATH={shlex.quote(str(prefix))}", flush=True)
    unpinned = [item["name"] for item in manifest if not item["expected_sha256"]]
    if unpinned:
        print(f"提示：{', '.join(unpinned)} 未固定哈希，"
              f"可把 manifest.json 中的实测值写入脚本后再重跑一次。", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, tarfile.TarError, zipfile.BadZipFile,
            subprocess.CalledProcessError) as error:
        print(f"错误：{error}", file=sys.stderr)
        sys.exit(1)
