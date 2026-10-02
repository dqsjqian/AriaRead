#!/usr/bin/env python3
"""Build AriaRead with one toolchain from dependencies through runtime packaging.

Windows defaults to MSVC x64 (stable 14.51 toolset; a live developer prompt
that selects a preview toolset is rejected). MinGW is an explicit, isolated
alternative. Python 3.10+ only; no Python packages are needed.

Run from any shell:

    python tools/build.py                    # deps + configure + build the server
    python tools/build.py --test             # ... plus tests, then run CTest
    python tools/build.py --toolchain mingw  # Windows: explicit MinGW build
    python tools/build.py --clean            # rebuild the app, keep dependencies
    python tools/build.py --clean-only       # clean app targets, keep everything
    python tools/build.py --configure-only   # deps + configure, no compile
    python tools/build.py --skip-cmake       # refresh assets beside an existing binary

Other knobs: --config (Release/Debug/...), --generator, --jobs, --tls-backend
(openssl/schannel), --offline, --require-web-tests, --build-dir, --deps-prefix.
Environment overrides: ARIAREAD_BUILD_DIR, ARIAREAD_DEPS_PREFIX,
ARIAREAD_BUILD_JOBS, ARIAREAD_VS_ROOT, ARIAREAD_WINDOWS_KITS_ROOT, MSYS2_ROOT.
"""
from __future__ import annotations

import argparse
import ctypes
import os
from contextlib import contextmanager
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def positive_jobs(value: str) -> int:
    try:
        jobs = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("--jobs must be an integer from 1 to 256") from error
    if not 1 <= jobs <= 256:
        raise argparse.ArgumentTypeError("--jobs must be an integer from 1 to 256")
    return jobs


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--toolchain", choices=("auto", "msvc", "mingw"), default="auto",
                        help="Windows: auto means MSVC; MinGW must be selected explicitly")
    parser.add_argument("--config", choices=("Release", "Debug", "RelWithDebInfo", "MinSizeRel"),
                        default="Release")
    parser.add_argument("--generator", help="CMake generator; Windows defaults to Ninja")
    parser.add_argument("--build-dir", type=Path, default=os.environ.get("ARIAREAD_BUILD_DIR"))
    parser.add_argument("--deps-prefix", type=Path, default=os.environ.get("ARIAREAD_DEPS_PREFIX"))
    parser.add_argument("--jobs", type=positive_jobs,
                        default=os.environ.get("ARIAREAD_BUILD_JOBS", str(min(os.cpu_count() or 1, 8))))
    parser.add_argument("--tls-backend", choices=("auto", "openssl", "schannel"), default="auto")
    parser.add_argument("--offline", action="store_true", help="Use only recorded sources and local caches")
    parser.add_argument("--test", action="store_true", help="Build test dependencies, all targets, then run CTest")
    parser.add_argument("--require-web-tests", action="store_true",
                        help="With --test, fail if Node.js or other web-test prerequisites are missing")
    parser.add_argument("--clean", action="store_true", help="Clean application objects before rebuilding; keep dependencies")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--skip-cmake", action="store_true", help="Refresh assets/licenses in an existing runtime")
    mode.add_argument("--configure-only", action="store_true", help="Prepare dependencies and configure without building")
    mode.add_argument("--clean-only", action="store_true", help="Clean existing application targets; keep dependencies")
    args = parser.parse_args(argv)
    if args.clean and (args.skip_cmake or args.configure_only or args.clean_only):
        parser.error("--clean requires a build")
    if args.test and (args.skip_cmake or args.clean_only):
        parser.error("--test requires a build or --configure-only")
    if args.require_web_tests and not args.test:
        parser.error("--require-web-tests requires --test")
    return args


def selected_toolchain(requested: str, windows: bool) -> str:
    if windows:
        return "msvc" if requested == "auto" else requested
    if requested != "auto":
        raise ValueError("--toolchain msvc/mingw is only supported on Windows")
    return "native"


def build_paths(args, toolchain: str, root: Path | None = None) -> tuple[Path, Path]:
    # Keep the existing macOS/Linux cache. Windows compilers must never share
    # a CMake cache or an installed prefix, even when both are on PATH.
    root = ROOT if root is None else root
    default_build = root / "build"
    default_prefix = default_build / "deps/prefix"
    if toolchain in ("msvc", "mingw"):
        default_build /= f"windows-{toolchain}-{args.config.lower()}"
        default_prefix = root / "build/deps" / f"windows-{toolchain}" / "prefix"
    return ((args.build_dir or default_build).expanduser().resolve(),
            (args.deps_prefix or default_prefix).expanduser().resolve())


def read_cache(build: Path) -> dict[str, str]:
    cache = build / "CMakeCache.txt"
    if not cache.exists():
        return {}
    result = {}
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        match = re.match(r"([^/#][^:]*):[^=]+=(.*)$", line)
        if match:
            result[match[1]] = match[2]
    return result


def prepend_path(env: dict[str, str], paths) -> None:
    additions = [str(path) for path in paths if path.is_dir()]
    env["PATH"] = os.pathsep.join([*additions, env.get("PATH", "")])


def executable(name: str, env: dict[str, str]) -> str | None:
    return shutil.which(name, path=env.get("PATH", ""))


def version_directories(parent: Path, required: tuple[str, ...]) -> list[Path]:
    if not parent.is_dir():
        return []
    candidates = [path for path in parent.iterdir() if path.is_dir()
                  and re.fullmatch(r"\d+(?:\.\d+)+", path.name)
                  and all((path / item).exists() for item in required)]
    return sorted(candidates, key=lambda path: tuple(map(int, path.name.split("."))), reverse=True)


def stable_msvc_directory(root: Path) -> Path | None:
    """Honor VS's selected stable toolset, never the largest preview directory."""
    versions = version_directories(root / "VC/Tools/MSVC", ("bin/Hostx64/x64/cl.exe", "include", "lib/x64"))
    default_file = root / "VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt"
    if default_file.is_file():
        version = default_file.read_text(encoding="utf-8-sig").strip()
        if not re.fullmatch(r"14\.51\.\d+(?:\.\d+)*", version):
            raise ValueError(f"{default_file} selects {version!r}; AriaRead requires the stable MSVC 14.51 "
                             "toolset from Visual Studio 2026 18.10.3. Update the stable C++ workload; "
                             "preview toolsets are not selected automatically.")
        selected = root / "VC/Tools/MSVC" / version
        if selected not in versions:
            raise ValueError(f"{default_file} references an incomplete or missing MSVC toolset: {selected}. "
                             "Repair the Visual Studio C++ workload or select another ARIAREAD_VS_ROOT.")
        return selected
    # Older/install-script layouts may omit the selection file. The current
    # stable series is explicit so a side-by-side 14.52 preview cannot win.
    return next((path for path in versions if re.fullmatch(r"14\.51\.\d+(?:\.\d+)*", path.name)), None)


def windows_drive_roots() -> list[Path]:
    return [Path(f"{letter}:/") for letter in "CDEFG"]


def program_files_directories(env: dict[str, str]) -> list[Path]:
    selected = [Path(env[key]) for key in ("PROGRAMFILES(X86)", "PROGRAMFILES") if env.get(key)]
    return list(dict.fromkeys([*selected, *(drive / name for drive in windows_drive_roots()
                                          for name in ("Program Files (x86)", "Program Files"))]))


def msys2_bin_directories(env: dict[str, str]) -> list[Path]:
    roots = [Path(env["MSYS2_ROOT"])] if env.get("MSYS2_ROOT") else []
    roots.extend(drive / name for drive in windows_drive_roots()
                 for name in ("msys64", "msys2", "worksoft/msys64"))
    return list(dict.fromkeys(root / part / "bin" for root in roots for part in ("ucrt64", "mingw64")))


def visual_studio_roots(env: dict[str, str]) -> list[Path]:
    if env.get("ARIAREAD_VS_ROOT"):
        return [Path(env["ARIAREAD_VS_ROOT"])]
    roots = [Path(env["VSINSTALLDIR"])] if env.get("VSINSTALLDIR") else []
    # Developer prompts do not always export VSINSTALLDIR.
    if env.get("VCTOOLSINSTALLDIR"):
        roots.append(Path(env["VCTOOLSINSTALLDIR"]).parents[3])
    program_files = program_files_directories(env)
    vswhere = next((root / "Microsoft Visual Studio/Installer/vswhere.exe" for root in program_files
                    if (root / "Microsoft Visual Studio/Installer/vswhere.exe").is_file()), None)
    if vswhere:
        try:
            found = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires",
                                    "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property",
                                    "installationPath", "-utf8"], env=env, check=True, timeout=10,
                                   capture_output=True, text=True, encoding="utf-8")
            roots.extend(Path(line.strip()) for line in found.stdout.splitlines() if line.strip())
        except (OSError, subprocess.SubprocessError) as error:
            diagnostic = str(getattr(error, "stderr", "") or "").strip()
            print(f"warning: vswhere failed ({error}); probing known installation paths. {diagnostic}",
                  file=sys.stderr)
    bases = [root / "Microsoft Visual Studio" for root in program_files]
    bases.extend(drive / name for drive in windows_drive_roots()
                 for name in ("Microsoft Visual Studio", "worksoft/VS2026"))
    for base in bases:
        roots.append(base)
        roots.extend(sorted(base.glob("[0-9]*"), reverse=True))
        roots.extend(sorted(base.glob("[0-9]*/*"), reverse=True))
    return list(dict.fromkeys(roots))


def msvc_environment(env: dict[str, str]) -> tuple[str, str]:
    roots = visual_studio_roots(env)
    prepend_path(env, [root / part for root in roots for part in (
        "Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja",
        "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin")])
    current = executable("cl", env)
    if current and env.get("ARIAREAD_VS_ROOT") and not Path(current).is_relative_to(Path(env["ARIAREAD_VS_ROOT"])):
        current = None  # An explicit installation takes precedence over another developer prompt.
    if current and env.get("INCLUDE") and env.get("LIB"):
        target = env.get("VSCMD_ARG_TGT_ARCH", "").lower()
        if target and target not in ("x64", "amd64"):
            raise ValueError("AriaRead requires the x64 MSVC environment; open an x64 developer prompt")
        installed_version = next((part for part in Path(current).parts
                                  if re.fullmatch(r"14\.\d+\.\d+(?:\.\d+)*", part)), "")
        versions = [value.strip() for value in (env.get("VCTOOLSVERSION", ""), installed_version) if value.strip()]
        if any(not re.fullmatch(r"14\.51\.\d+(?:\.\d+)*", version) for version in versions):
            raise ValueError("The active developer prompt selects MSVC " + ", ".join(versions) +
                             "; AriaRead requires stable MSVC 14.51. Open a stable Visual Studio 2026 "
                             "18.10.3 x64 prompt or build from a plain shell; preview toolsets are unsupported.")
        if versions and not env.get("ARIAREAD_WINDOWS_KITS_ROOT"):
            env["VCTOOLSVERSION"] = versions[0]
            return current, current
    kits_roots = [Path(env[key]) for key in ("ARIAREAD_WINDOWS_KITS_ROOT", "WINDOWSSDKDIR") if env.get(key)]
    kits_roots.extend(root / "Windows Kits/10" for root in program_files_directories(env))
    kits_roots.extend(root / "Windows Kits/10" for root in windows_drive_roots())
    # SDK upgrades can leave incomplete version directories. Use the newest
    # complete x64 SDK instead of letting a partial install hide a working one.
    sdk = None
    for kits_root in dict.fromkeys(kits_roots):
        sdk = next((path for path in version_directories(kits_root / "Include", ("ucrt", "um", "shared"))
                    if all((kits_root / "Lib" / path.name / part / "x64").is_dir() for part in ("ucrt", "um"))
                    and all((kits_root / "bin" / path.name / "x64" / tool).is_file()
                            for tool in ("rc.exe", "mt.exe"))), None)
        if sdk:
            break
    diagnostics = []
    for root in roots:
        try:
            vc = stable_msvc_directory(root)
            if not vc and env.get("ARIAREAD_VS_ROOT"):
                raise ValueError(f"ARIAREAD_VS_ROOT has no complete stable MSVC 14.51 x64 toolset: {root}")
        except ValueError as error:
            if env.get("ARIAREAD_VS_ROOT"):
                raise
            diagnostics.append(str(error))
            continue
        if not vc or not sdk:
            continue
        version = sdk.name
        prepend_path(env, [vc / "bin/Hostx64/x64", kits_root / "bin" / version / "x64",
                           root / "Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja",
                           root / "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin",
                           root / "Common7/IDE", root / "Common7/Tools"])
        env["INCLUDE"] = ";".join(str(path) for path in [vc / "include", *(
            sdk / name for name in ("ucrt", "um", "shared", "winrt", "cppwinrt")
            if (sdk / name).is_dir())])
        env["LIB"] = ";".join(map(str, [vc / "lib/x64", kits_root / "Lib" / version / "ucrt/x64",
                                        kits_root / "Lib" / version / "um/x64"]))
        env["WINDOWSSDKDIR"] = str(kits_root) + "\\"
        env["WINDOWSSDKVERSION"] = version + "\\"
        env["VCTOOLSINSTALLDIR"] = str(vc) + "\\"
        env["VCTOOLSVERSION"] = vc.name
        env["VSCMD_ARG_TGT_ARCH"] = "x64"
        compiler = str(vc / "bin/Hostx64/x64/cl.exe")
        return compiler, compiler
    raise ValueError("Stable MSVC x64 and the Windows SDK were not found. Install Visual Studio 2026 "
                     "18.10.3 Build Tools (MSVC 14.51) "
                     "with Desktop development with C++ and C++ CMake tools, or set ARIAREAD_VS_ROOT. "
                     "To use MSYS2 UCRT64 explicitly, pass --toolchain mingw. " + " | ".join(diagnostics))


def mingw_environment(env: dict[str, str]) -> tuple[str, str]:
    candidates = msys2_bin_directories(env)
    c = cxx = None
    if env.get("MSYS2_ROOT"):
        for path in candidates[:2]:
            if (path / "gcc.exe").is_file() and (path / "g++.exe").is_file():
                prepend_path(env, [path])
                c, cxx = str(path / "gcc.exe"), str(path / "g++.exe")
                break
    # Preserve an explicitly selected UCRT64/MINGW64 shell before probing defaults.
    if not (c and cxx):
        c, cxx = executable("gcc", env), executable("g++", env)
    if not (c and cxx):
        for path in candidates:
            if (path / "gcc.exe").is_file() and (path / "g++.exe").is_file():
                prepend_path(env, [path])
                c, cxx = str(path / "gcc.exe"), str(path / "g++.exe")
                break
    if not (c and cxx):
        raise ValueError("MinGW was explicitly selected but gcc/g++ are missing. Install the MSYS2 "
                         "UCRT64 toolchain, CMake and Ninja; set MSYS2_ROOT for a custom installation.")
    target = subprocess.run([c, "-dumpmachine"], env=env, check=True,
                            capture_output=True, text=True).stdout.strip()
    if not target.startswith("x86_64-") or not target.endswith("mingw32"):
        raise ValueError(f"Expected a MinGW x64 compiler, got {target!r} at {c}")
    return c, cxx


def compiler_command(value: str, env: dict[str, str], windows: bool) -> tuple[str, list[str]]:
    parts = [value] if Path(value).is_file() else shlex.split(value, posix=not windows)
    parts = [part.strip('"') for part in parts]
    path = executable(parts[0], env) if parts else None
    if not path:
        raise ValueError(f"Compiler is unavailable: {value}")
    return str(Path(path).resolve()), parts[1:]


def prepare_windows_perl(env: dict[str, str]) -> None:
    explicit = Path(env["ARIAREAD_PERL_DIR"]) if env.get("ARIAREAD_PERL_DIR") else None
    if explicit and (explicit / "perl.exe").is_file():
        prepend_path(env, [explicit])
        return
    if executable("perl", env):
        return
    candidates = [root / "Strawberry/perl/bin" for root in windows_drive_roots()]
    candidates.extend(root / "Strawberry/perl/bin" for root in program_files_directories(env))
    candidates.extend(msys2_bin_directories(env))
    for path in candidates:
        if (path / "perl.exe").is_file():
            prepend_path(env, [path])
            return


def prepare_environment(args, toolchain: str, cache: dict[str, str], source_env=None):
    windows = toolchain != "native"
    env = dict(os.environ if source_env is None else source_env)
    if windows:
        # Avoid duplicate Path/PATH keys inherited through mixed shell hosts.
        env = {key.upper(): value for key, value in env.items()}
        c, cxx = msvc_environment(env) if toolchain == "msvc" else mingw_environment(env)
        env["CMAKE_GENERATOR"] = args.generator or cache.get("CMAKE_GENERATOR") or "Ninja"
        visual_studio = env["CMAKE_GENERATOR"].lower().startswith("visual studio")
        if visual_studio and toolchain != "msvc":
            raise ValueError("Visual Studio generators require --toolchain msvc")
        if visual_studio:
            env["CMAKE_GENERATOR_PLATFORM"] = "x64"
            env["CMAKE_GENERATOR_TOOLSET"] = "version=" + env["VCTOOLSVERSION"]
        else:
            env.pop("CMAKE_GENERATOR_PLATFORM", None)
            env.pop("CMAKE_GENERATOR_TOOLSET", None)
        if "ninja" in env["CMAKE_GENERATOR"].lower() and not executable("ninja", env):
            raise ValueError("Ninja is missing. Install Visual Studio C++ CMake tools or the MSYS2 UCRT64 ninja package.")
    else:
        c = env.get("CC") or cache.get("CMAKE_C_COMPILER") or "cc"
        cxx = env.get("CXX") or cache.get("CMAKE_CXX_COMPILER") or "c++"
        if args.generator:
            env["CMAKE_GENERATOR"] = args.generator
        elif not env.get("CMAKE_GENERATOR") and cache.get("CMAKE_GENERATOR"):
            env["CMAKE_GENERATOR"] = cache["CMAKE_GENERATOR"]
    cc, cargs = compiler_command(c, env, windows)
    cxx, cxxargs = compiler_command(cxx, env, windows)
    if not windows:
        # CMake stores compiler options separately from its executable. A later
        # invocation without CC/CXX must preserve both for the dependency build.
        if not env.get("CC") and cache.get("CMAKE_C_COMPILER"):
            cargs = shlex.split(cache.get("CMAKE_C_COMPILER_ARG1", ""))
        if not env.get("CXX") and cache.get("CMAKE_CXX_COMPILER"):
            cxxargs = shlex.split(cache.get("CMAKE_CXX_COMPILER_ARG1", ""))
    quote = subprocess.list2cmdline if windows else shlex.join
    env.update(CC=quote([cc, *cargs]), CXX=quote([cxx, *cxxargs]), PYTHONIOENCODING="utf-8")
    # OpenSSL is optional on Windows; Perl is only relevant when explicitly requested.
    if windows and args.tls_backend == "openssl":
        prepare_windows_perl(env)
    if windows and not executable("cmake", env):
        prepend_path(env, [root / "CMake/bin" for root in program_files_directories(env)])
    if not executable("cmake", env):
        raise ValueError("CMake 3.21+ is required; install it or enable Visual Studio C++ CMake tools")
    generator = env.get("CMAKE_GENERATOR")
    if cache.get("CMAKE_GENERATOR") and generator and cache["CMAKE_GENERATOR"] != generator:
        raise ValueError("The build directory uses a different CMake generator. Choose a new --build-dir; "
                         "existing builds and dependencies have been preserved.")
    if ("CMAKE_GENERATOR_TOOLSET" in cache
            and cache["CMAKE_GENERATOR_TOOLSET"] != env.get("CMAKE_GENERATOR_TOOLSET", "")):
        raise ValueError("The build directory uses a different CMake toolset. Choose a new --build-dir; "
                         "existing builds and dependencies have been preserved.")
    for key, selected in (("CMAKE_C_COMPILER", cc), ("CMAKE_CXX_COMPILER", cxx)):
        if cache.get(key) and Path(cache[key]).resolve() != Path(selected).resolve():
            raise ValueError("The build directory uses a different compiler. Choose a new --build-dir; "
                             "MSVC and MinGW cannot share a CMake cache.")
    return env, (cc, cargs), (cxx, cxxargs)


@contextmanager
def utf8_console():
    """Decode CTest's UTF-8 output correctly on Chinese Windows consoles.

    CTest writes test names and logs as UTF-8, but a console on the legacy
    code page (936 on Chinese systems) renders them as mojibake. Switch the
    console output code page to UTF-8 for the CTest run and restore it
    afterwards, so the compiler's own localized diagnostics stay readable
    during the build phase. No-op when stdout is not a console: redirected
    files and pipes already carry the raw UTF-8 bytes.
    """
    if os.name != "nt":
        yield
        return
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    previous = kernel32.GetConsoleOutputCP()
    active = bool(previous)
    if active:
        kernel32.SetConsoleOutputCP(65001)
    try:
        yield
    finally:
        if active:
            kernel32.SetConsoleOutputCP(previous)


def run(command, env, *, quiet=False):
    command = list(map(str, command))
    print("+ " + (subprocess.list2cmdline(command) if os.name == "nt" else shlex.join(command)), flush=True)
    subprocess.run(command, cwd=ROOT, env=env, check=True,
                   stdout=subprocess.DEVNULL if quiet else None)


def compiler_preflight(env, compiler_args):
    """Run the shared CMake policy before fetching or compiling dependencies."""
    with tempfile.TemporaryDirectory(prefix="ariaread-compiler-check-") as temporary:
        source = Path(temporary)
        (source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.21)\n'
            'project(AriaReadCompilerCheck LANGUAGES C CXX)\n'
            'include("${ARIAREAD_COMPILER_REQUIREMENTS}")\n', encoding="utf-8")
        run(["cmake", "-S", source, "-B", source / "build", *compiler_args,
             f"-DARIAREAD_COMPILER_REQUIREMENTS={ROOT / 'cmake/CompilerRequirements.cmake'}"], env)


def runtime_path(build: Path, config: str, windows: bool) -> tuple[Path, Path]:
    name = "ariaread_web_server.exe" if windows else "ariaread_web_server"
    folder = build / "bin"
    if (folder / config / name).is_file():
        folder /= config
    binary = folder / name
    if not binary.is_file():
        raise ValueError(f"Server executable is missing: {binary}")
    return folder, binary


def main(argv=None):
    args = parse_args(argv)
    windows = sys.platform == "win32"
    toolchain = selected_toolchain(args.toolchain, windows)
    if windows and args.config != "Release":
        raise ValueError("Windows dependency libraries use the Release CRT; --config Release is required")
    if not windows and args.tls_backend == "schannel":
        raise ValueError("Schannel is only available on Windows")
    build, prefix = build_paths(args, toolchain)
    cache = read_cache(build)
    if args.skip_cmake:
        env = dict(os.environ)
        folder, binary = runtime_path(build, args.config, windows)
        compiler = cache.get("CMAKE_CXX_COMPILER", "")
        mingw = cache.get("MINGW", "") or ("mingw" in cache.get("CMAKE_GENERATOR", "").lower())
        mingw = mingw or bool(re.search(r"(?:g\+\+|clang\+\+)(?:\.exe)?$", compiler, re.I) and windows)
        run(["cmake", f"-DARIAREAD_SOURCE_DIR={ROOT}", f"-DARIAREAD_OUTPUT_DIR={folder}",
             f"-DARIAREAD_RUNTIME_CONFIG={build / 'AriaReadRuntimePaths.cmake'}",
             f"-DARIAREAD_MINGW={'ON' if mingw else 'OFF'}", f"-DARIAREAD_CXX_COMPILER={compiler}",
             "-P", ROOT / "cmake/SyncWebRuntime.cmake"], env)
    else:
        if args.clean_only and not cache:
            print(f"Nothing to clean: {build}")
            return 0
        env, (cc, cargs), (cxx, cxxargs) = prepare_environment(args, toolchain, cache)
        if args.clean_only:
            run(["cmake", "--build", build, "--config", args.config, "--target", "clean"], env)
            print(f"Cleaned application targets; dependency cache retained: {prefix}")
            return 0
        quote = subprocess.list2cmdline if windows else shlex.join
        compiler_args = [f"-DCMAKE_BUILD_TYPE={args.config}", f"-DCMAKE_C_COMPILER={cc}",
                         f"-DCMAKE_CXX_COMPILER={cxx}",
                         "-DCMAKE_C_COMPILER_ARG1=" + quote(cargs),
                         "-DCMAKE_CXX_COMPILER_ARG1=" + quote(cxxargs)]
        # Existing CMake configurations keep these flags even if the calling
        # shell's CFLAGS/CXXFLAGS changed. Probe with that same configuration.
        for key, value in cache.items():
            if (re.fullmatch(r"CMAKE_(?:C|CXX|EXE_LINKER)_FLAGS(?:_[A-Z]+)?", key)
                    or key in ("CMAKE_OSX_ARCHITECTURES", "CMAKE_OSX_SYSROOT",
                               "CMAKE_OSX_DEPLOYMENT_TARGET", "CMAKE_SYSROOT")):
                compiler_args.append(f"-D{key}={value}")
        compiler_preflight(env, compiler_args)
        offline = ["--offline"] if args.offline else []
        run([sys.executable, ROOT / "tools/ci/fetch_aria.py", *offline], env)
        run([sys.executable, ROOT / "tools/ci/build_ariaread_deps.py", "--path", ROOT / "build/deps",
             "--prefix", prefix, "--jobs", args.jobs, "--profile", "tests" if args.test else "runtime",
             "--tls-backend", args.tls_backend, "--c-compiler", env["CC"],
             "--cxx-compiler", env["CXX"], *offline], env)
        cmake_args = ["cmake", "-S", ROOT, "-B", build, *compiler_args,
                      f"-DARIAREAD_DEPS_PREFIX={prefix}", f"-DARIAREAD_TLS_BACKEND={args.tls_backend}",
                      f"-DARIAREAD_BUILD_TESTS={'ON' if args.test else 'OFF'}",
                      f"-DARIAREAD_BUILD_WEB_TESTS={'ON' if args.test else 'OFF'}",
                      f"-DARIAREAD_REQUIRE_WEB_TESTS={'ON' if args.require_web_tests else 'OFF'}"]
        run(cmake_args, env)
        if args.configure_only:
            print(f"Configured: {build}")
            return 0
        command = ["cmake", "--build", build, "--config", args.config, "--parallel", args.jobs]
        if not args.test:
            command += ["--target", "ariaread_web_server"]
        if args.clean:
            command.append("--clean-first")
        run(command, env)
        if args.test:
            with utf8_console():
                run(["ctest", "--test-dir", build, "-C", args.config, "--output-on-failure",
                     "--no-tests=error", "--timeout", "120", "--parallel", args.jobs], env)
        folder, binary = runtime_path(build, args.config, windows)
    run([binary, "--help"], env, quiet=True)
    print(f"\nReady: {folder}\nRun: \"{binary}\"\n"
          "Distribute this directory with its libraries, web/ assets and licenses/. "
          "The target system must provide compatible compiler and system runtimes. "
          "See THIRD_PARTY_NOTICES.md for redistribution terms.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
