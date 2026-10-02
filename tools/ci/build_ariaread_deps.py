#!/usr/bin/env python3
"""Resolve, lock and explicitly build AriaRead's third-party dependencies.

Normal runs reuse resolved entries in dependencies.json. Missing entries select the latest stable
release; --version name=version selects an explicit release, --update refreshes
latest entries, and --offline requires exact locked sources in the cache.
Verified installations are reused only with identical sources, recipes, toolchain
and installed contents. Replacements preserve the old prefix and roll back on
failure. CMake only verifies the prefix and never downloads dependencies.
"""

from __future__ import annotations

import argparse
import hashlib
import inspect
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
from dataclasses import asdict, dataclass, replace
from pathlib import Path, PurePosixPath

import dependency_cache as cache_state
import dependencies as dependency_resolver

REPO = Path(__file__).resolve().parents[2]
PATCHES = Path(__file__).resolve().parent / "patches"


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
    #: cmake | openssl | generated | Mira
    kind: str
    #: 传给 cmake 的额外参数
    options: tuple[str, ...] = ()
    #: kind == "generated" 时写入源码根目录的 CMakeLists.txt 内容
    cmake_lists: str = ""
    #: 安装后必须存在的文件（相对 prefix），用于自检
    artifacts: tuple[str, ...] = ()
    #: 构建前应用到源码树的补丁（tools/ci/patches 下）
    patch: str = ""
    #: 该依赖的 CMake 工程是否消费 CMAKE_INSTALL_LIBDIR（即包含
    #: GNUInstallDirs）。不消费的依赖（generated 自写 CMakeLists 硬编码
    #: DESTINATION lib、zlib 自定义安装目录）传了也只是 CMake 噪音告警。
    uses_libdir: bool = False
    #: 构建前复制到源码树的额外文件（tools/ci/patches 下 → 源码根同名文件）
    extra_files: tuple[str, ...] = ()
    #: 哈希来源说明，写进 manifest
    hash_note: str = "SHA256 verified against dependencies.json resolved metadata"
    revision: str = ""
    #: Libraries consumed by this recipe; changes invalidate its static consumers.
    requires: tuple[str, ...] = ()

    @property
    def archive_name(self) -> str:
        return self.url.rsplit("/", 1)[-1]


# ── 生成的 CMakeLists：gumbo / sqlite / quickjs 上游没有 CMake 构建 ──────────
#
# 放在脚本里而不是写回仓库源码树，是因为这些文件描述的是"如何把上游源码编成
# 一个静态库并安装"，属于取依赖的过程，不属于 AriaRead 自己的构建逻辑。

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
# AriaRead embeds the JS engine only; the CLI's os/std helpers are unused.
target_include_directories(quickjs PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                                          $<INSTALL_INTERFACE:include>)
set_target_properties(quickjs PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON
                                        POSITION_INDEPENDENT_CODE ON)
# CONFIG_VERSION 上游由 Makefile 传入（quickjs.c 里直接用），CMake 侧必须补上。
# Keep the engine's GNU extensions visible on POSIX platforms.
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


RECIPES: tuple[Dependency, ...] = (
    Dependency(
        name="zlib", version="",
        url="",
        sha256="",
        license="Zlib", license_files=("LICENSE",), root="", kind="cmake",
        # 仅静态：zlib 的 CMake 默认同时产出共享库，Windows 消费端会通过
        # 导入库依赖 zlib1.dll，测试发现与服务器启动都得额外带 DLL。
        options=("-DZLIB_BUILD_SHARED=OFF", "-DZLIB_BUILD_EXAMPLES=OFF",
                 "-DSKIP_INSTALL_FILES=OFF"),
        artifacts=("lib/libz.a", "include/zlib.h"),  # Windows 上是 zlibstatic.lib，见 artifact_present
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
    ),
    Dependency(
        name="sqlite3", version="",
        url="",
        sha256="",
        license="blessing（Public Domain）",
        license_files=(), root="", kind="generated",
        cmake_lists=SQLITE_CMAKE,
        artifacts=("lib/libsqlite3.a", "include/sqlite3.h"),
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


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(cache: Path, dependency: Dependency, offline: bool) -> Path:
    """下载（或复用）归档并校验 SHA256；Mira 走 GitHub API 以取到私有仓库。"""
    archive = cache / (dependency.sha256 + "-" + dependency.archive_name)
    if archive.is_symlink():
        raise ValueError(f"Refusing symbolic-link archive cache: {archive}")
    content = cache / dependency.sha256
    if not archive.exists() and content.is_file() and not content.is_symlink():
        if sha256(content) != dependency.sha256:
            raise ValueError(f"Content cache SHA256 mismatch; preserved: {content}")
        shutil.copyfile(content, archive)
    legacy = cache / dependency.archive_name
    if not archive.exists() and legacy.is_file() and not legacy.is_symlink() and sha256(legacy) == dependency.sha256:
        shutil.copyfile(legacy, archive)
    expected = dependency.sha256
    if archive.exists():
        if not archive.is_file() or archive.is_symlink():
            raise ValueError(f"缓存路径不是普通文件：{archive}")
        if expected and sha256(archive) != expected:
            raise ValueError(f"缓存 SHA256 校验失败，未覆盖文件：{archive}")
        print(f"Reusing verified archive: {archive.name}", flush=True)
        return archive
    if offline:
        raise ValueError(f"离线缓存缺失：{archive}")

    request = urllib.request.Request(dependency.url,
                                     headers={"User-Agent": "ariaread-deps"})
    print(f"Downloading: {dependency.url}\nExpected SHA256: {expected or '(not pinned)'}",
          flush=True)
    # 下载中断或校验失败只删除本次临时文件，不覆盖已有归档。
    with tempfile.TemporaryDirectory(prefix="download-", dir=cache) as temporary:
        candidate = Path(temporary) / dependency.archive_name
        try:
            with urllib.request.urlopen(request, timeout=120) as response, \
                    candidate.open("wb") as output:
                dependency_resolver.https_url(response.url)
                shutil.copyfileobj(response, output)
        except urllib.error.HTTPError as error:  # noqa: PERF203
            raise ValueError(f"下载失败（HTTP {error.code}）：{dependency.url}") from error
        actual = sha256(candidate)
        if expected and actual != expected:
            raise ValueError(f"下载 SHA256 校验失败：{dependency.url}\n实测：{actual}")
        if not expected:
            print(f"Downloaded SHA256: {actual}", flush=True)
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
    # Preserve optional upstream notices without inventing one when absent.
    for filename in ('NOTICE', 'NOTICE.txt'):
        origin = source / filename
        if origin.is_file() and filename not in copied:
            shutil.copyfile(origin, target / filename)
            copied.append(filename)
    if dependency.name == 'json':
        notices = set()
        for header in (source / 'include/nlohmann').rglob('*.hpp'):
            for line in header.read_text(encoding='utf-8').splitlines():
                if 'SPDX-FileCopyrightText:' in line or 'SPDX-License-Identifier:' in line:
                    notices.add(line)
        if not notices:
            raise ValueError('JSON embedded copyright notices are missing')
        (target / 'ATTRIBUTIONS.txt').write_text('\n'.join(sorted(notices)) + '\n', encoding='utf-8')
        copied.append('ATTRIBUTIONS.txt')
    if dependency.name == 'sqlite3':
        # The official amalgamation has a public-domain dedication, not LICENSE.
        header = (source / 'sqlite3.h').read_text(encoding='utf-8')
        dedication, separator, _ = header.partition('*************************************************************************')
        if not separator or 'author disclaims copyright' not in dedication:
            raise ValueError('SQLite public-domain dedication needs review')
        (target / 'PUBLIC-DOMAIN.txt').write_text(dedication, encoding='utf-8')
        copied.append('PUBLIC-DOMAIN.txt')
    return copied


def windows_toolchain() -> str:
    """Classify the selected compiler, independent of unrelated PATH entries."""
    selected = os.environ.get('CC', '')
    if selected:
        parts = [selected] if Path(selected).is_file() else shlex.split(selected, posix=os.name != 'nt')
        name = Path(parts[0].strip(chr(34))).stem.lower()
        return 'msvc' if name in ('cl', 'clang-cl') else 'mingw'
    return 'msvc' if shutil.which('cl') and shutil.which('nmake') else 'mingw'


def build_cmake(source: Path, build: Path, prefix: Path, jobs: int,
                dependency: Dependency, common: list[str]) -> None:
    generator = os.environ.get('CMAKE_GENERATOR', '').strip()
    selected = windows_toolchain() if sys.platform == 'win32' else ''
    visual_studio = generator.lower().startswith('visual studio') or (not generator and selected == 'msvc')
    configure = ['cmake']
    if visual_studio:
        if selected != 'msvc':
            raise ValueError('Visual Studio generator requires an MSVC-compatible compiler')
        configure += ['-A', os.environ.get('CMAKE_GENERATOR_PLATFORM') or 'x64']
    elif not generator and selected == 'mingw':
        configure += ['-G', 'Ninja' if shutil.which('ninja') else 'MinGW Makefiles']
    libdir = ['-DCMAKE_INSTALL_LIBDIR=lib'] if dependency.uses_libdir else []
    run([*configure, '-S', str(source), '-B', str(build), *common, *libdir, *dependency.options])
    # --config is accepted by single-config generators and required by VS,
    # Xcode and Ninja Multi-Config, including MSVC builds without a VS generator.
    run(['cmake', '--build', str(build), '--parallel', str(jobs), '--config', 'Release'])
    run(['cmake', '--install', str(build), '--config', 'Release'])


def msys2_tool(name: str, compiler: str | None = None) -> str:
    """Locate a tool from the MSYS2 installation that owns `compiler`.

    OpenSSL's mingw64 recipe is a Unix recipe: Perl must emit forward-slash
    paths and the makefiles call sh utilities. Running it from native Windows
    Python with a MSWin32 perl fails ("This perl implementation doesn't
    produce Unix like paths"), while the MSYS2 perl cannot be spawned across
    the Windows/MSYS boundary. Aria's BuildOpenSSL.cmake solves this by
    driving the whole recipe through MSYS2 bash; resolve the same toolchain
    here so both projects share one approach.
    """
    roots: list[Path] = []
    if compiler:
        roots.append(Path(compiler).resolve().parent.parent.parent / "usr" / "bin")
    for variable in ('MSYS2_ROOT',):
        if os.environ.get(variable):
            roots.append(Path(os.environ[variable]) / "usr" / "bin")
    roots += [Path(r) / "usr" / "bin" for r in ("C:/msys64", "D:/msys64", "D:/worksoft/msys64")]
    for root in roots:
        candidate = root / f"{name}.exe"
        if candidate.is_file():
            return str(candidate)
    raise ValueError(
        f"OpenSSL 的 MinGW 构建需要 MSYS2 的 {name}（{', '.join(str(r) for r in roots)}）；"
        "请安装 MSYS2 或设置 MSYS2_ROOT")


def build_openssl(source: Path, prefix: Path, jobs: int) -> None:
    windows = sys.platform == "win32"
    toolchain = windows_toolchain() if windows else ""
    if toolchain == "mingw":
        build_openssl_mingw(source, prefix, jobs)
        return
    if shutil.which("perl") is None:
        raise ValueError("OpenSSL 的 Configure 需要 perl（Windows 上可用 Strawberry Perl）")
    make = "nmake" if toolchain == "msvc" else "make"
    if shutil.which(make) is None:
        raise ValueError(f"OpenSSL 源码构建需要 {make}")
    if toolchain == "msvc":
        target = ["VC-WIN64A"]
    else:
        target = []
    compiler_options = []
    if toolchain == 'msvc':
        # OpenSSL's Windows makefile template quotes CC itself. Passing the
        # shell-quoted CC used by CMake would produce ""C:\Program Files\..."".
        selected = cache_state.compiler(os.environ.get('CC') or 'cl')
        compiler_options.append('CC=' + selected['path'])
        if selected['arguments']:
            flags = subprocess.list2cmdline(selected['arguments'])
            compiler_options.append('CFLAGS=' + (flags + ' ' + os.environ.get('CFLAGS', '')).strip())
    # no-asm：避免 Windows 上再依赖 NASM；静态库只给 libcurl 用，慢一点无所谓。
    run(["perl", str(source / "Configure"), *target, f"--prefix={prefix}",
         f"--openssldir={prefix}/ssl", "--libdir=lib", "no-shared", "no-tests",
         # no-winstore: the Windows certificate-store provider pulls
         # crypt32 into every static consumer, and OpenSSL's exported
         # CMake interface lists crypt32 *before* libcrypto.a, which GNU
         # ld (left-to-right) cannot use. Nothing in AriaRead reads the
         # system store — curl ships its own CA bundle.
         "no-winstore",
         "no-docs", "no-apps", "no-asm", *compiler_options], cwd=source)
    if toolchain == "msvc":
        run([make], cwd=source)
        run([make, "install_sw"], cwd=source)
    else:
        run([make, "-j", str(jobs)], cwd=source)
        run([make, "install_sw"], cwd=source)


def build_openssl_mingw(source: Path, prefix: Path, jobs: int) -> None:
    """Build OpenSSL's mingw64 target entirely inside MSYS2 bash.

    The compiler output is a Windows static library either way, but Perl,
    make and the generated makefiles must agree on the filesystem, so the
    recipe cannot be driven from native Windows Python.
    """
    compiler = os.environ.get('CC') or shutil.which('gcc') or shutil.which('g++')
    if not compiler:
        raise ValueError("OpenSSL 的 MinGW 构建需要 gcc")
    bash = msys2_tool('bash', compiler)
    perl = msys2_tool('perl', compiler)
    make = msys2_tool('make', compiler)
    recipe = source / "ariaread-openssl-mingw.sh"
    recipe.write_text(
        "#!/usr/bin/env bash\n"
        "# Generated by tools/ci/build_ariaread_deps.py. OpenSSL's mingw64 recipe is\n"
        "# a Unix recipe, so shell, make and Perl must share one filesystem view.\n"
        "set -euo pipefail\n"
        f'export PATH="$(cygpath -u "{Path(compiler).resolve().parent}"):'
        f'$(dirname "$(cygpath -u "{perl}")"):$PATH"\n'
        f'source_dir="$(cygpath -u "{source}")"\n'
        f'prefix_dir="$(cygpath -u "{prefix}")"\n'
        'mkdir -p "$prefix_dir"\n'
        'cd "$source_dir"\n'
        f'exec "$(cygpath -u "{perl}")" "$source_dir/Configure" mingw64 \\\n'
        f'    "--prefix=$prefix_dir" "--openssldir=$prefix_dir/ssl" --libdir=lib \\\n'
        '    no-shared no-tests no-winstore no-docs no-apps no-asm\n',
        encoding='utf-8', newline='\n')
    run([bash, str(recipe)], cwd=source)
    run([bash, '-lc', f'cd "$(cygpath -u "{source}")" && '
                      f'PATH="$(cygpath -u "{Path(compiler).resolve().parent}"):$PATH" '
                      f'"{make}" -j{jobs} && "{make}" install_sw'], cwd=source)


def artifact_present(prefix: Path, artifact: str) -> bool:
    """Require a real header/config or an exact static-library platform spelling."""
    candidate = prefix / artifact
    if candidate.is_file():
        return True
    if not artifact.endswith('.a') or not candidate.parent.is_dir():
        return False
    stem = candidate.stem.removeprefix('lib')
    stems = {'z': ('z', 'zlibstatic')}.get(stem, (stem, 'lib' + stem))
    names = {name + suffix for name in stems for suffix in ('.a', '.lib')}
    return any(entry.is_file() and entry.name.lower() in names for entry in candidate.parent.iterdir())


def normalize_zlib_static(prefix: Path) -> None:
    """Keep zlib's exports intact and supply names understood by FindZLIB.

    zlib 1.3.2 names Windows static libraries zs.lib/libzs.a. Older CMake
    FindZLIB modules do not search these names, so retain the original export
    location and add a byte-identical static-library compatibility name.
    """
    for original, compatible in [('zs.lib', 'zlibstatic.lib'), ('libzs.a', 'libz.a')]:
        source, target = prefix / 'lib' / original, prefix / 'lib' / compatible
        if source.is_file():
            if target.exists() or target.is_symlink():
                if target.is_symlink() or not target.is_file() or sha256(source) != sha256(target):
                    raise ValueError(f'Conflicting zlib static library preserved: {target}')
            else:
                shutil.copy2(source, target)


def fetch_git(work: Path, dependency: Dependency, offline: bool) -> Path:
    """Cache a clean checkout by immutable revision; never overwrite edited sources."""
    target = work / "git" / dependency.name / dependency.revision
    def git(*arguments):
        return subprocess.check_output(["git", "-C", str(target), *arguments], text=True,
                                       encoding="utf-8", errors="replace").strip()
    if target.is_symlink():
        raise ValueError(f"Refusing symbolic-link Git cache: {target}")
    if target.exists():
        if target.is_symlink() or not (target / ".git").is_dir():
            raise ValueError(f"Refusing unknown Git source cache: {target}")
        if git("rev-parse", "HEAD") != dependency.revision or git("status", "--porcelain", "--untracked-files=all", "--ignored"):
            raise ValueError(f"Git source cache has local changes; preserved: {target}")
        return target
    if offline:
        raise ValueError(f"Missing exact offline Git checkout: {target}")
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix="checkout-", dir=target.parent))
    run(["git", "init", str(temporary)])
    run(["git", "-C", str(temporary), "remote", "add", "origin", dependency.url])
    run(["git", "-C", str(temporary), "fetch", "--depth", "1", "origin", dependency.revision])
    run(["git", "-C", str(temporary), "checkout", "--detach", "FETCH_HEAD"])
    actual = subprocess.check_output(["git", "-C", str(temporary), "rev-parse", "HEAD"], text=True,
                                     encoding="utf-8", errors="replace").strip()
    if actual != dependency.revision:
        raise ValueError(f"Git checkout revision mismatch; preserved: {temporary}")
    temporary.rename(target)
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


# A patch is approved for a specific upstream version, never blindly carried forward.
PATCH_VERSIONS = {'gumbo': {'0.10.1'}, 'quickjs': {'2026-06-04'},
                  'sqlite_modern_cpp': {'3.2'}, 'doctest': {'2.5.3'}}


def configured_recipes(profile='tests', tls_backend='auto'):
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
    for recipe in RECIPES:
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


def locked_recipes(lock, recipes=None):
    entries = lock.get('dependencies', {})
    result = []
    for recipe in RECIPES if recipes is None else recipes:
        entry = entries.get(recipe.name)
        if not entry:
            raise ValueError(f'Missing locked dependency: {recipe.name}')
        dependency_resolver.validate_record(entry)
        version = entry['version']
        if recipe.patch and version not in PATCH_VERSIONS[recipe.name]:
            raise ValueError(f'{recipe.name} {version} needs an explicitly reviewed build recipe/patch; '
                             f'supported: {sorted(PATCH_VERSIONS[recipe.name])}')
        revision = entry.get('revision', '')
        digest = entry.get('sha256', '')
        if recipe.kind == 'git':
            if len(revision) != 40 or any(c not in '0123456789abcdef' for c in revision):
                raise ValueError(f'{recipe.name} requires a full locked Git revision')
        elif len(digest) != 64 or any(c not in '0123456789abcdef' for c in digest):
            raise ValueError(f'{recipe.name} requires a locked SHA256')
        result.append(replace(recipe, version=version, url=entry['url'], sha256=digest,
                              revision=revision,
                              cmake_lists=recipe.cmake_lists.replace('@QUICKJS_VERSION@', version),
                              hash_note=entry.get('checksum_source', recipe.hash_note)))
    return result


def recipe_digest(dependencies):
    """Fingerprint the actual source/build pipeline, independent of CLI storage syntax."""
    patches = {name: sha256(PATCHES / name) for dep in dependencies
               for name in (dep.patch, *dep.extra_files) if name}
    functions = (run, extract, apply_patch, prepare_source,
                 copy_licenses, windows_toolchain,
                 artifact_present, build_environment, cmake_arguments, install_component,
                 dependency_selection, cache_state.compiler, cache_state.build_context)
    functions += (build_openssl, build_openssl_mingw, msys2_tool) if any(dep.kind == 'openssl' for dep in dependencies) else ()
    functions += (build_cmake,) if any(dep.kind != 'openssl' for dep in dependencies) else ()
    functions += (fetch_git,) if any(dep.kind == 'git' for dep in dependencies) else ()
    functions += (download,) if any(dep.kind != 'git' for dep in dependencies) else ()
    functions += (normalize_zlib_static,) if any(dep.name == 'zlib' for dep in dependencies) else ()
    return cache_state.fingerprint({'recipes': [asdict(dep) for dep in dependencies],
                                    'patches': patches,
                                    'pipeline': {fn.__name__: inspect.getsource(fn)
                                                 for fn in functions}})


def dependency_selection(names, dependencies):
    by_name = {dep.name: dep for dep in dependencies}
    all_names = set(by_name)
    selected = {name.strip() for name in names.split(',') if name.strip()} or all_names
    if selected - all_names:
        raise ValueError(f'Unknown dependencies: {sorted(selected - all_names)}')
    # Static consumers must be built against this installation's matching libraries.
    while True:
        required = {name for selected_name in selected for name in by_name[selected_name].requires}
        if required - all_names:
            raise ValueError(f'Missing dependency recipes: {sorted(required - all_names)}')
        if required <= selected:
            break
        selected.update(required)
    return selected


def component_identities(dependencies, context):
    """Version/patch changes rebuild the component and its consumers, not its peers."""
    if 'environment' in context:
        # The selected compiler records already cover the executable, its hash,
        # version and arguments. CC/CXX only retain the caller's spelling (or
        # absence), which changes when switching between the standalone builder
        # and tools/build.py. A generator alone does not change the target ABI;
        # retain its platform/toolset and every other build environment input.
        context = {**context, 'environment': {
            key: value for key, value in context['environment'].items()
            if key not in ('CC', 'CXX', 'CMAKE_GENERATOR')}}
    identities = {}
    for dep in dependencies:
        missing = set(dep.requires) - identities.keys()
        if missing:
            raise ValueError(f'Dependency recipes are not topologically ordered: {dep.name}: {sorted(missing)}')
        identities[dep.name] = cache_state.fingerprint({
            'recipe': recipe_digest([dep]), 'context': context,
            'requires': {name: identities[name] for name in dep.requires}})
    return identities


def reusable_components(previous, identities):
    if not previous:
        return set()
    components = previous.get('components', {})
    # Older installations lack ownership receipts. Preserve the old prefix as a
    # backup, but rebuild it once rather than guessing which files belong to whom.
    owned = set()
    for name in previous.get('completed', []):
        component = components.get(name, {})
        files = component.get('files', [])
        if not files or owned.intersection(files):
            return set()
        owned.update(files)
    if owned != set(previous.get('files', {})):
        return set()
    return {name for name in previous.get('completed', [])
            if components[name].get('identity') == identities.get(name)}


def copy_component_files(source, destination, files):
    """Copy verified owned files; retain symlinks for their final prefix location."""
    for relative in files:
        origin, target = source / relative, destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if origin.is_symlink():
            target.symlink_to(os.readlink(origin), target_is_directory=origin.is_dir())
        else:
            shutil.copy2(origin, target)


def verify_prefix(prefix, resolution, dependencies, selected, c=None, cxx=None, toolchain=None,
                  c_arg1='', cxx_arg1=''):
    state = cache_state.read_state(prefix)
    if not state:
        raise ValueError('Unverified/legacy prefix: run tools/ci/build_ariaread_deps.py first')
    if state.get('components'):
        identities = component_identities(dependencies, state['context'])
        if not selected <= reusable_components(state, identities):
            raise ValueError('Dependency resolution or recipe changed: run tools/ci/build_ariaread_deps.py')
    elif state.get('resolution') != cache_state.fingerprint(resolution) or state.get('recipe') != recipe_digest(dependencies):
        raise ValueError('Dependency resolution or recipe changed: run tools/ci/build_ariaread_deps.py')
    if not selected <= set(state.get('completed', [])):
        raise ValueError('Dependency prefix is incomplete: run tools/ci/build_ariaread_deps.py')
    context = state['context']
    cache_state.verify_environment(context, toolchain)
    cache_state.verify_location(context, prefix)
    for name, command, arguments in [('c', c, c_arg1), ('cxx', cxx, cxx_arg1)]:
        if command:
            if arguments:
                # CMake exposes the executable and its pre-command arguments
                # separately. Preserve the executable as one token, then let
                # compiler() parse the argument string with host quoting rules.
                quote = subprocess.list2cmdline if os.name == 'nt' else shlex.join
                command = quote([command]) + ' ' + arguments
            actual = cache_state.compiler(command)
            if cache_state.same_compiler(context[name], actual):
                continue
            raise ValueError(f'Dependency {name} compiler differs from project compiler; rebuild required; '
                             f'recorded={context[name]!r}, actual={actual!r}')
    return state


def build_environment(prefix, c=None, cxx=None, toolchain=None):
    if toolchain is not None:
        os.environ['CMAKE_TOOLCHAIN_FILE'] = toolchain
    if shutil.which('cmake') is None:
        raise ValueError('CMake and a C/C++ toolchain are required')
    if shutil.which('cl') and '/utf-8' not in os.environ.get('CL', ''):
        os.environ['CL'] = (os.environ.get('CL', '') + ' /utf-8').strip()
    if sys.platform == 'win32' and os.environ.get('CMAKE_GENERATOR_PLATFORM', '').lower() not in ('', 'x64'):
        raise ValueError('Windows dependency recipes currently target x64')
    context = cache_state.build_context(prefix, c, cxx)
    if sys.platform == 'win32':
        families = {'msvc' if Path(context[key]['path']).stem.lower() in ('cl', 'clang-cl') else 'mingw'
                    for key in ('c', 'cxx')}
        if len(families) != 1:
            raise ValueError('C and C++ compilers must use the same Windows toolchain (MSVC or MinGW)')
    # OpenSSL Configure and CMake must consume the same selected compiler.
    for variable, key in [('CC', 'c'), ('CXX', 'cxx')]:
        command = [context[key]['path'], *context[key]['arguments']]
        os.environ[variable] = subprocess.list2cmdline(command) if os.name == 'nt' else shlex.join(command)
    return context


def cmake_arguments(prefix, context):
    common = [f'-DCMAKE_INSTALL_PREFIX={prefix}', '-DCMAKE_BUILD_TYPE=Release',
              '-DCMAKE_POSITION_INDEPENDENT_CODE=ON', '-DBUILD_SHARED_LIBS=OFF',
              f'-DCMAKE_PREFIX_PATH={prefix}',
              f"-DCMAKE_C_COMPILER={context['c']['path']}",
              f"-DCMAKE_CXX_COMPILER={context['cxx']['path']}"]
    if context['environment']['CMAKE_TOOLCHAIN_FILE']:
        common.append('-DCMAKE_TOOLCHAIN_FILE=' + context['environment']['CMAKE_TOOLCHAIN_FILE'])
    for name, key in [('C', 'c'), ('CXX', 'cxx')]:
        if context[key]['arguments']:
            arguments = context[key]['arguments']
            quoted = subprocess.list2cmdline(arguments) if os.name == 'nt' else shlex.join(arguments)
            common.append(f'-DCMAKE_{name}_COMPILER_ARG1=' + quoted)
    return common


def install_component(dep, available, run_root, prefix, jobs, common):
    holder = run_root / dep.name
    holder.mkdir()
    if dep.kind == 'git':
        source = holder / 'source'
        shutil.copytree(available, source, ignore=shutil.ignore_patterns('.git'))
    else:
        source = extract(available, holder, '')
    prepare_source(source, dep)
    if dep.kind == 'openssl':
        build_openssl(source, prefix, jobs)
    else:
        build_cmake(source, holder / 'build', prefix, jobs, dep, common)
    licenses = copy_licenses(prefix, source, dep)
    if dep.name == 'zlib' and sys.platform == 'win32':
        normalize_zlib_static(prefix)
        for junk in ('lib/zlib.lib', 'lib/zlib.dll', 'lib/zlib1.dll',
                     'lib/libzlib.dll.a', 'bin/zlib.dll', 'bin/zlib1.dll', 'bin/libzlib.dll'):
            (prefix / junk).unlink(missing_ok=True)
    missing = [artifact for artifact in dep.artifacts if not artifact_present(prefix, artifact)]
    if missing:
        raise ValueError(f'{dep.name} missing expected artifacts: {missing}')
    return {'name': dep.name, 'version': dep.version, 'url': dep.url,
            'sha256': dep.sha256, 'revision': dep.revision,
            'license': dep.license, 'license_files': licenses}


def main():
    # Keep Python child processes (including the resolver) on the same protocol
    # when output is redirected by CMake, CI, or a parent process on Windows.
    os.environ['PYTHONIOENCODING'] = 'utf-8'
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding='utf-8', errors='replace')
        except (AttributeError, OSError, ValueError):
            pass
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--path', type=Path, default=REPO / 'build/deps')
    parser.add_argument('--prefix', type=Path)
    parser.add_argument('--jobs', type=positive_jobs, default=min(os.cpu_count() or 1, 8))
    parser.add_argument('--only', default='')
    parser.add_argument('--profile', choices=('runtime', 'tests'), default='tests',
                        help='Runtime dependencies only, or include the test framework (default)')
    parser.add_argument('--tls-backend', choices=('auto', 'openssl', 'schannel'), default='auto',
                        help='auto uses Windows Schannel; other platforms use OpenSSL')
    parser.add_argument('--offline', action='store_true')
    parser.add_argument('--update', action='store_true', help='Explicitly refresh latest stable dependencies')
    parser.add_argument('--version', action='append', default=[], metavar='NAME=VERSION')
    parser.add_argument('--file', type=Path, default=REPO / 'dependencies.json',
                        help='Single dependency declaration and resolved-state file')
    parser.add_argument('--verify-prefix', action='store_true', help='Offline installed-prefix verification only')
    parser.add_argument('--c-compiler')
    parser.add_argument('--cxx-compiler')
    parser.add_argument('--c-compiler-arg1', default='', help='CMake C compiler arguments for --verify-prefix')
    parser.add_argument('--cxx-compiler-arg1', default='', help='CMake C++ compiler arguments for --verify-prefix')
    parser.add_argument('--toolchain-file', default=None)
    parser.add_argument('--cmake-platform', default='')
    args = parser.parse_args()
    recipes = configured_recipes(args.profile, args.tls_backend)
    selected = dependency_selection(args.only, recipes)
    work = output_path(args.path)
    raw_prefix = args.prefix or work / 'prefix'
    if raw_prefix.is_symlink():
        raise ValueError(f'Refusing symlink installation prefix: {raw_prefix}')
    prefix = output_path(raw_prefix)
    for directory in ('cache', 'git', 'runs'):
        other = work / directory
        if prefix == other or prefix in other.parents or other in prefix.parents:
            raise ValueError('--prefix overlaps the source/build cache')
    if not args.verify_prefix:
        names = {dep.name for dep in recipes}
        for version in args.version:
            if version.partition('=')[0] not in names:
                raise ValueError(f'Unknown third-party dependency override: {version}')
        command = [sys.executable, str(Path(__file__).with_name('dependencies.py')),
                   'update' if args.update else 'resolve', '--file', str(args.file),
                   '--cache-dir', str(work / 'cache')]
        for name in sorted(names):
            command += ['--only', name]
        if args.offline:
            command.append('--offline')
        for version in args.version:
            command += ['--version', version]
        run(command)
    # Aria has its own bootstrap checkout. Only validate the installed third-party set.
    resolution = dependency_resolver.read_resolved(args.file, only=[dep.name for dep in recipes])
    dependencies = locked_recipes(resolution, recipes)
    if args.verify_prefix:
        verify_prefix(prefix, resolution, dependencies, selected, args.c_compiler, args.cxx_compiler,
                      args.toolchain_file, args.c_compiler_arg1, args.cxx_compiler_arg1)
        if sys.platform == 'win32' and args.cmake_platform.lower() not in ('', 'x64'):
            raise ValueError('The Windows dependency prefix targets x64; configure the project for x64')
        print(f'Verified locked dependency prefix: {prefix}')
        return
    context = build_environment(prefix, args.c_compiler, args.cxx_compiler, args.toolchain_file)

    metadata = {'resolution': cache_state.fingerprint(resolution), 'recipe': recipe_digest(dependencies),
                'context': context, 'generator': 'tools/ci/build_ariaread_deps.py'}
    identities = component_identities(dependencies, context)
    # A change to component identity rules must also invalidate the transaction
    # receipt. Otherwise installation() can trust an old completed set even
    # after reusable_components() correctly rejected those components.
    identity = cache_state.fingerprint({'metadata': metadata, 'components': identities})
    (work / 'cache').mkdir(parents=True, exist_ok=True)
    with cache_state.prefix_lock(prefix):
        previous = cache_state.read_state(prefix) if prefix.exists() else None
        completed = reusable_components(previous, identities)
        if selected <= completed:
            print(f'Reusing verified dependency prefix: {prefix}')
            return
        # All downloads happen before the old installation is moved.
        available = {}
        for dep in dependencies:
            if dep.name in selected - completed:
                available[dep.name] = fetch_git(work, dep, args.offline) if dep.kind == 'git' else download(work / 'cache', dep, args.offline)
        runs = work / 'runs'
        runs.mkdir(parents=True, exist_ok=True)
        run_root = Path(tempfile.mkdtemp(prefix=identity[:12] + '-', dir=runs))
        # The transaction builds at the final prefix, because some upstream
        # exports embed it. Stage only unchanged, inventoried component files
        # before it moves the old prefix, keeping rollback and local-edit guards.
        staged = run_root / 'reused'
        if completed and previous.get('identity') != identity:
            for name in sorted(completed):
                copy_component_files(prefix, staged, previous['components'][name]['files'])
        common = cmake_arguments(prefix, context)
        with cache_state.installation(prefix, identity, selected, metadata) as (done, state):
            if done is None:
                return
            records = [record for record in previous.get('dependencies', []) if record['name'] in completed] if previous else []
            components = {name: previous['components'][name] for name in completed}
            if staged.exists():
                for name in sorted(completed):
                    copy_component_files(staged, prefix, components[name]['files'])
                done.update(completed)
            for name in sorted(completed):
                print(f'Reusing verified component: {name}', flush=True)
            for dep in dependencies:
                if dep.name not in selected - done:
                    continue
                before = cache_state.inventory(prefix)
                records.append(install_component(dep, available[dep.name], run_root, prefix,
                                                 args.jobs, common))
                after = cache_state.inventory(prefix)
                changed = [name for name, content in before.items() if after.get(name) != content]
                if changed:
                    raise ValueError(f'{dep.name} overwrote another component: {changed}')
                components[dep.name] = {'identity': identities[dep.name],
                                        'files': sorted(after.keys() - before.keys())}
                done.add(dep.name)
            state.update(completed=sorted(done), dependencies=records, components=components)
        if staged.exists():
            shutil.rmtree(staged)
        print(f'Installed locked dependency prefix: {prefix}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, tarfile.TarError, zipfile.BadZipFile,
            subprocess.CalledProcessError) as error:
        print(f'Error: {error}', file=sys.stderr)
        sys.exit(1)
