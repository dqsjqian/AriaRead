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


# ─────────────────────────────────────────────────────────────────────────────
# Sources → snapshots → builds
# ─────────────────────────────────────────────────────────────────────────────


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(cache: Path, dependency: Dependency, offline: bool) -> Path:
    """Download (or reuse) the archive and verify its SHA256."""
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
            raise ValueError(f"Cache path is not a regular file: {archive}")
        if expected and sha256(archive) != expected:
            raise ValueError(f"Cache SHA256 verification failed; preserved: {archive}")
        print(f"Reusing verified archive: {archive.name}", flush=True)
        return archive
    if offline:
        raise ValueError(f"Offline cache missing: {archive}")

    request = urllib.request.Request(dependency.url,
                                     headers={"User-Agent": "ariaread-deps"})
    print(f"Downloading: {dependency.url}\nExpected SHA256: {expected or '(not pinned)'}",
          flush=True)
    # An interrupted download or a failed verification only removes this run's
    # temporary file; an existing archive is never overwritten.
    with tempfile.TemporaryDirectory(prefix="download-", dir=cache) as temporary:
        candidate = Path(temporary) / dependency.archive_name
        try:
            with urllib.request.urlopen(request, timeout=120) as response, \
                    candidate.open("wb") as output:
                https_url(response.url)
                shutil.copyfileobj(response, output)
        except urllib.error.HTTPError as error:  # noqa: PERF203
            raise ValueError(f"Download failed (HTTP {error.code}): {dependency.url}") from error
        actual = sha256(candidate)
        if expected and actual != expected:
            raise ValueError(f"Download SHA256 verification failed: {dependency.url}\nActual: {actual}")
        if not expected:
            print(f"Downloaded SHA256: {actual}", flush=True)
        candidate.replace(archive)
    return archive


def extract(archive: Path, destination: Path, root_name: str) -> Path:
    """Safe extraction: refuse absolute paths, escapes and non-regular members."""
    def safe(member_name: str) -> PurePosixPath:
        path = PurePosixPath(member_name)
        if (path.is_absolute() or ".." in path.parts or "\\" in member_name
                or not path.parts):
            raise ValueError(f"Refusing unsafe archive member: {member_name}")
        target = (destination / member_name).resolve()
        if destination.resolve() not in target.parents:
            raise ValueError(f"Refusing escaping archive member: {member_name}")
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
                    raise ValueError(f"Refusing non-regular archive member: {member.name}")
                source = package.extractfile(member)
                if source is None:
                    raise ValueError(f"Cannot read archive member: {member.name}")
                with source:
                    write(destination / member.name, source.read(),
                          bool(member.mode & 0o111))
    if root_name:
        return destination / root_name
    entries = [entry for entry in destination.iterdir() if entry.is_dir()]
    if len(entries) != 1:
        raise ValueError(f"Cannot locate the archive's top-level directory: {destination}")
    return entries[0]


def run(command: list[str], cwd: Path | None = None) -> None:
    print("+ " + shlex.join(command), flush=True)
    subprocess.run(command, check=True, cwd=str(cwd) if cwd else None)


def apply_patch(source: Path, patch_file: Path) -> None:
    """Apply a unified diff (-p1).

    Implemented locally instead of calling `patch`: the pipeline depends only on
    the Python standard library and `patch` is not always present on Windows.
    Already-applied hunks are skipped, so the result is idempotent.
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
            # a/quickjs.c -> quickjs.c; a trailing timestamp is removed as well.
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
                # A hunk adding a new file: skip if the content is already there
                # (otherwise every run would insert another copy).
                if any(updated[start:start + len(added)] == added
                       for start in range(len(updated) - len(added) + 1)):
                    continue
                updated[old_start:old_start] = added
                continue
            position = next((start for start in range(len(updated) - len(removed) + 1)
                             if updated[start:start + len(removed)] == removed), None)
            if position is None:
                # Already applied: the new content is present; skip (idempotent).
                if any(updated[start:start + len(added)] == added
                       for start in range(len(updated) - len(added) + 1)):
                    continue
                raise ValueError(f"Patch cannot be applied: {patch_file.name} -> {target.name}")
            updated[position:position + len(removed)] = added

        target.write_text("\n".join(updated) + ("\n" if updated else ""), encoding="utf-8")


def prepare_source(source: Path, dependency: Dependency) -> None:
    if dependency.patch:
        patch = PATCHES / dependency.patch
        if not patch.is_file():
            raise ValueError(f"Patch missing: {patch}")
        apply_patch(source, patch)
    for name in dependency.extra_files:
        extra = PATCHES / name
        if not extra.is_file():
            raise ValueError(f"Extra file missing: {extra}")
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
            raise ValueError(f"{dependency.name} license file missing: {origin}")
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
        # The official amalgamation carries a public-domain dedication, not a LICENSE.
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
        f"OpenSSL's MinGW build needs MSYS2's {name} ({', '.join(str(r) for r in roots)}); "
        "install MSYS2 or set MSYS2_ROOT")


def build_openssl(source: Path, prefix: Path, jobs: int) -> None:
    windows = sys.platform == "win32"
    toolchain = windows_toolchain() if windows else ""
    if toolchain == "mingw":
        build_openssl_mingw(source, prefix, jobs)
        return
    if shutil.which("perl") is None:
        raise ValueError("OpenSSL's Configure requires perl (Strawberry Perl on Windows)")
    make = "nmake" if toolchain == "msvc" else "make"
    if shutil.which(make) is None:
        raise ValueError(f"Building OpenSSL from source requires {make}")
    if toolchain == "msvc":
        target = ["VC-WIN64A"]
    else:
        target = []
    compiler_options = []
    if toolchain == 'msvc':
        # OpenSSL's Windows makefile template quotes CC itself. Passing the
        # shell-quoted CC used by CMake would produce ""C:\Program Files\..."".
        selected = compiler(os.environ.get('CC') or 'cl')
        compiler_options.append('CC=' + selected['path'])
        if selected['arguments']:
            flags = subprocess.list2cmdline(selected['arguments'])
            compiler_options.append('CFLAGS=' + (flags + ' ' + os.environ.get('CFLAGS', '')).strip())
    # no-asm avoids a NASM dependency on Windows; the static libraries only
    # serve libcurl, so the slowdown is acceptable.
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
        raise ValueError("OpenSSL's MinGW build requires gcc")
    bash = msys2_tool('bash', compiler)
    perl = msys2_tool('perl', compiler)
    make = msys2_tool('make', compiler)
    recipe = source / "ariaread-openssl-mingw.sh"
    recipe.write_text(
        "#!/usr/bin/env bash\n"
        "# Generated by tools/build.py. OpenSSL's mingw64 recipe is a Unix\n"
        "# recipe, so shell, make and Perl must share one filesystem view.\n"
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


def fetch_git(work: Path, workspace: Path, dependency: Dependency, offline: bool) -> None:
    """Clone the locked revision straight into a new editable workspace.

    build/deps/git/<name>/<revision> belongs to the former layout. A clean
    copy there is reused once for migration (and left for older build trees);
    no second per-revision tree is created any more.
    """
    legacy = work / "git" / dependency.name / dependency.revision
    if sources.copy_legacy_clone(legacy, workspace, dependency.revision):
        return
    if offline:
        raise ValueError(f"Offline source missing for {dependency.name}: create {workspace} "
                         "online once, or provide it manually")
    sources.clone(workspace, dependency.url, dependency.revision)


def output_path(value: Path) -> Path:
    path = value.expanduser().resolve()
    if path in (Path.home(), REPO) or path in REPO.parents:
        raise ValueError(f"Refusing the home directory or repository root as output: {path}")
    for system in ("/usr", "/bin", "/sbin", "/etc", "/System", "/Library", "/opt"):
        root = Path(system)
        if path == root or root in path.parents:
            raise ValueError(f"Refusing to install into a system directory: {path}")
    if path.exists() and not path.is_dir():
        raise ValueError(f"Output path is not a directory: {path}")
    if ";" in str(path):
        raise ValueError("Output paths cannot contain CMake's list separator ';'")
    return path


# ─────────────────────────────────────────────────────────────────────────────
# Identity, reuse and verification
# ─────────────────────────────────────────────────────────────────────────────


# A patch is approved for a specific upstream version, never blindly carried forward.


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
