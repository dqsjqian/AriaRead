#!/usr/bin/env python3
"""Offline build-entry regressions using real toolchain layouts and mocked processes.

These checks run on every host. They exercise discovery and orchestration, but
cannot substitute for compiling/linking on the Windows MSVC and MinGW CI jobs.
"""
from contextlib import ExitStack, redirect_stderr, redirect_stdout
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

# The public entry point is tools/build.py; this suite lives with its internals.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build  # noqa: E402


class BuildTests(unittest.TestCase):
    def setUp(self):
        self.original_env = dict(os.environ)
        self.original_path = os.environ.get("PATH", "")
        self.temporary = tempfile.TemporaryDirectory(prefix="ariaread build tests ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.drives = [self.root / "drives" / letter for letter in "CDEFG"]
        self.vs = self.root / "Program Files/Microsoft Visual Studio/2026/BuildTools"
        self.kits = self.root / "Program Files (x86)/Windows Kits/10"
        self.vc = self.make_vc("14.51.36247")
        self.default_vc = self.touch(self.vs / "VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt",
                                     self.vc.name + "\n")
        self.make_sdk("10.0.26100.0")
        self.cmake_bin = self.vs / "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin"
        self.ninja_bin = self.vs / "Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja"
        self.touch(self.cmake_bin / "cmake.exe")
        self.touch(self.cmake_bin / "ctest.exe")
        self.touch(self.ninja_bin / "ninja.exe")
        self.msys = self.root / "msys64"
        self.mingw = self.msys / "ucrt64/bin"
        for tool in ("gcc", "g++", "ninja", "cmake", "ctest"):
            self.touch(self.mingw / (tool + ".exe"))
        self.env = {"PATH": "", "ARIAREAD_VS_ROOT": str(self.vs),
                    "ARIAREAD_WINDOWS_KITS_ROOT": str(self.kits)}
        self.commands = []
        self.installs = []
        self.verifies = []
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(patch.object(build, "ROOT", self.root))
        self.stack.enter_context(patch.object(build, "windows_drive_roots", return_value=self.drives))
        self.stack.enter_context(patch.object(build, "executable", side_effect=self.which))
        self.stack.enter_context(patch.dict(os.environ, {}, clear=True))
        self.stack.enter_context(redirect_stdout(io.StringIO()))
        self.stack.enter_context(redirect_stderr(io.StringIO()))

    def touch(self, path, content="fixture"):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return path

    def make_vc(self, version):
        root = self.vs / "VC/Tools/MSVC" / version
        self.touch(root / "bin/Hostx64/x64/cl.exe")
        (root / "include").mkdir(parents=True)
        (root / "lib/x64").mkdir(parents=True)
        return root

    def make_sdk(self, version, complete=True):
        for name in ("ucrt", "um", "shared", "winrt"):
            (self.kits / "Include" / version / name).mkdir(parents=True)
        if complete:
            for part in ("ucrt", "um"):
                (self.kits / "Lib" / version / part / "x64").mkdir(parents=True)
            for name in ("rc.exe", "mt.exe"):
                self.touch(self.kits / "bin" / version / "x64" / name)

    @staticmethod
    def which(name, env):
        """Model Windows .exe lookup while testing its real directory layout."""
        path = Path(name)
        if path.is_absolute():
            return str(path) if path.is_file() else None
        for folder in env.get("PATH", "").split(os.pathsep):
            if not folder:
                continue
            for candidate in (Path(folder) / name, Path(folder) / (name + ".exe")):
                if candidate.is_file():
                    return str(candidate)
        return None

    def record(self, command, env, **kwargs):
        self.commands.append((list(map(str, command)), dict(env), kwargs))
        return None

    def record_install(self, file, work, prefix, source_dir, **options):
        """The dependency library shares this process; snapshot the environment."""
        self.installs.append(((str(file), str(work), str(prefix), str(source_dir)), dict(os.environ), options))
        return prefix

    def record_verify(self, file, work, prefix, source_dir, **options):
        self.verifies.append(((str(file), str(work), str(prefix), str(source_dir)), dict(os.environ), options))
        return prefix

    def invoke(self, arguments=(), *, platform="win32", env=None, cache=None):
        args = build.parse_args(list(arguments))
        toolchain = build.selected_toolchain(args.toolchain, platform == "win32")
        build_dir, prefix = build.build_paths(args, toolchain)
        if cache:
            self.touch(build_dir / "CMakeCache.txt", "\n".join(
                f"{key}:STRING={value}" for key, value in cache.items()))
        binary = build_dir / "bin" / ("ariaread_web_server.exe" if platform == "win32"
                                     else "ariaread_web_server")
        self.touch(binary)
        with patch.object(build.sys, "platform", platform), \
                patch.dict(os.environ, self.env if env is None else env, clear=True), \
                patch.object(build, "run", side_effect=self.record), \
                patch.object(build.deps, "install", side_effect=self.record_install), \
                patch.object(build.deps, "verify", side_effect=self.record_verify):
            result = build.main(list(arguments))
        self.assertEqual(result, 0)
        return build_dir, prefix, binary

    def dependency_install(self):
        _, environment, options = self.installs[0]
        return environment, options

    def configure_call(self):
        return next((command, env) for command, env, _ in self.commands
                    if command[:3] == ["cmake", "-S", str(self.root)])

    def test_cmake_uses_the_interpreter_that_loaded_build_dependencies(self):
        self.invoke()
        command, _ = self.configure_call()
        self.assertIn(f"-DPython3_EXECUTABLE={sys.executable}", command)

    def test_windows_defaults_to_msvc_with_separate_mingw_build_and_prefix(self):
        args = build.parse_args([])
        self.assertEqual(build.selected_toolchain("auto", True), "msvc")
        msvc_build, msvc_prefix = build.build_paths(args, "msvc")
        mingw_build, mingw_prefix = build.build_paths(args, "mingw")
        # Unified scheme: build/unified/native-<toolchain>-release-<arch>
        self.assertTrue(str(msvc_build).endswith("build/unified/native-msvc-release") or
                        "build/unified/native-msvc-release" in str(msvc_build))
        self.assertTrue(str(mingw_build).endswith("build/unified/native-mingw-release") or
                        "build/unified/native-mingw-release" in str(mingw_build))
        self.assertNotEqual(msvc_build, mingw_build)
        self.assertNotEqual(msvc_prefix, mingw_prefix)

    def test_native_keeps_existing_build_prefix_and_accepts_environment_overrides(self):
        args = build.parse_args([])
        self.assertEqual(build.build_paths(args, "native"),
                         (self.root / "build", self.root / "build/deps/prefix"))
        with patch.dict(os.environ, {"ARIAREAD_BUILD_DIR": str(self.root / "custom build"),
                                     "ARIAREAD_DEPS_PREFIX": str(self.root / "custom deps"),
                                     "ARIAREAD_BUILD_JOBS": "3"}):
            args = build.parse_args([])
        self.assertEqual(build.build_paths(args, "native"),
                         (self.root / "custom build", self.root / "custom deps"))
        self.assertEqual(args.jobs, 3)

    def test_plain_msvc_shell_bootstraps_without_msys_or_perl(self):
        perl_bin = self.drives[0] / "Strawberry/perl/bin"
        self.touch(perl_bin / "perl.exe")
        source = dict(self.env, MSYS2_ROOT=str(self.msys), ARIAREAD_PERL_DIR=str(perl_bin))
        env, (cc, _), (cxx, _) = build.prepare_environment(build.parse_args([]), "msvc", {}, source)
        self.assertEqual(cc, str(self.vc / "bin/Hostx64/x64/cl.exe"))
        self.assertEqual(cc, cxx)
        self.assertIn(str(self.vc / "include"), env["INCLUDE"])
        self.assertIn(str(self.kits / "Lib/10.0.26100.0/um/x64"), env["LIB"])
        self.assertEqual(env["VSCMD_ARG_TGT_ARCH"], "x64")
        self.assertEqual(env["CMAKE_GENERATOR"], "Ninja")
        # The host's temporary directory can itself live under MSYS2. Check
        # actual tool directories rather than words in their parent paths.
        path_entries = {Path(entry) for entry in env["PATH"].split(os.pathsep) if entry}
        self.assertIn(self.ninja_bin, path_entries)
        for directory in (self.mingw, self.msys / "mingw64/bin", self.msys / "usr/bin", perl_bin):
            self.assertNotIn(directory, path_entries)
        self.assertIsNone(self.which("perl", env))
        self.assertEqual(env["CC"], subprocess.list2cmdline([cc]))

    def test_msvc_wins_even_with_mingw_and_compiler_overrides_in_shell(self):
        env = dict(self.env, Path=str(self.mingw), CC="gcc", CXX="g++",
                   CMAKE_GENERATOR_PLATFORM="Win32", CMAKE_GENERATOR_TOOLSET="host=x86")
        env.pop("PATH")
        result, (cc, _), _ = build.prepare_environment(build.parse_args([]), "msvc", {}, env)
        self.assertEqual(cc, str(self.vc / "bin/Hostx64/x64/cl.exe"))
        self.assertNotIn("Path", result)
        self.assertNotIn("CMAKE_GENERATOR_PLATFORM", result)
        self.assertNotIn("CMAKE_GENERATOR_TOOLSET", result)

    def test_msvc_discovery_uses_numeric_versions_and_ignores_incomplete_sdk(self):
        self.make_vc("14.9.99999")
        self.make_sdk("10.0.99999.0", complete=False)
        env, (cc, _), _ = build.prepare_environment(build.parse_args([]), "msvc", {}, self.env)
        self.assertEqual(cc, str(self.vc / "bin/Hostx64/x64/cl.exe"))
        self.assertEqual(env["WINDOWSSDKVERSION"], "10.0.26100.0\\")

    def test_msvc_prefers_default_stable_toolset_over_newer_side_by_side_versions(self):
        self.make_vc("14.51.99999")
        self.make_vc("14.52.10000")
        env, (cc, _), _ = build.prepare_environment(build.parse_args([]), "msvc", {}, self.env)
        self.assertEqual(cc, str(self.vc / "bin/Hostx64/x64/cl.exe"))
        self.assertEqual(env["VCTOOLSVERSION"], "14.51.36247")

    def test_msvc_without_default_file_falls_back_only_to_latest_stable_series(self):
        self.default_vc.unlink()
        newest_stable = self.make_vc("14.51.40000")
        self.make_vc("14.52.10000")
        _, (cc, _), _ = build.prepare_environment(build.parse_args([]), "msvc", {}, self.env)
        self.assertEqual(cc, str(newest_stable / "bin/Hostx64/x64/cl.exe"))

    def test_invalid_default_toolset_file_is_actionable_without_silent_fallback(self):
        self.make_vc("14.52.10000")
        for version, expected in (("14.51.99999", "incomplete or missing"),
                                  ("14.52.10000", "requires the stable MSVC 14.51"),
                                  ("not-a-version", "requires the stable MSVC 14.51")):
            self.default_vc.write_text(version)
            with self.subTest(version=version), self.assertRaisesRegex(ValueError, expected) as error:
                build.prepare_environment(build.parse_args([]), "msvc", {}, self.env)
            self.assertIn("Microsoft.VCToolsVersion.default.txt", str(error.exception))

    def test_developer_prompt_rejects_preview_version_in_environment_or_compiler_path(self):
        preview = self.make_vc("14.52.10000")
        for compiler, version in ((self.vc, preview.name), (preview, ""), (preview, self.vc.name)):
            env = dict(self.env, PATH=str(compiler / "bin/Hostx64/x64"), INCLUDE="include", LIB="lib",
                       VSCMD_ARG_TGT_ARCH="x64", VCTOOLSVERSION=version)
            with self.subTest(compiler=compiler, version=version), \
                    self.assertRaisesRegex(ValueError, "preview toolsets are unsupported"):
                build.prepare_environment(build.parse_args([]), "msvc", {}, env)

    def test_plain_shell_finds_visual_studio_and_sdk_without_overrides(self):
        env = {"Path": "", "ProgramFiles": str(self.root / "Program Files"),
               "ProgramFiles(x86)": str(self.root / "Program Files (x86)")}
        _, (cc, _), _ = build.prepare_environment(build.parse_args([]), "msvc", {}, env)
        self.assertEqual(cc, str(self.vc / "bin/Hostx64/x64/cl.exe"))

    def test_vswhere_discovers_nonstandard_installation_in_plain_shell(self):
        program_files = self.root / "elsewhere"
        vswhere = self.touch(program_files / "Microsoft Visual Studio/Installer/vswhere.exe")
        env = {"PATH": "", "PROGRAMFILES(X86)": str(program_files),
               "ARIAREAD_WINDOWS_KITS_ROOT": str(self.kits)}
        with patch.object(build.subprocess, "run", return_value=subprocess.CompletedProcess(
                [], 0, str(self.vs) + "\n")) as probe:
            _, (cc, _), _ = build.prepare_environment(build.parse_args([]), "msvc", {}, env)
        self.assertEqual(cc, str(self.vc / "bin/Hostx64/x64/cl.exe"))
        self.assertEqual(probe.call_args.args[0][0], str(vswhere))
        self.assertIn("Microsoft.VisualStudio.Component.VC.Tools.x86.x64", probe.call_args.args[0])

    def test_missing_msvc_and_incomplete_sdk_report_setup_instructions(self):
        for key, absent in (("ARIAREAD_VS_ROOT", self.root / "absent VS"),
                            ("ARIAREAD_WINDOWS_KITS_ROOT", self.root / "absent SDK")):
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "MSVC x64 and the Windows SDK|ARIAREAD_VS_ROOT"):
                build.prepare_environment(build.parse_args([]), "msvc", {}, dict(self.env, **{key: str(absent)}))

    def test_multidrive_msvc_sdk_fallback_survives_vswhere_failure_and_old_vs(self):
        c_drive, d_drive, e_drive, _, _ = self.drives
        selected_vs = d_drive / "worksoft/VS2026"
        selected_kits = e_drive / "Windows Kits/10"
        build.shutil.copytree(self.vs, selected_vs)
        build.shutil.copytree(self.kits, selected_kits)
        old_vs = c_drive / "Program Files/Microsoft Visual Studio/2022/BuildTools"
        build.shutil.copytree(self.vs, old_vs)
        (old_vs / "VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt").write_text("14.44.12345")
        vswhere = self.touch(c_drive / "Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe")
        environment = {"PATH": "", "PROGRAMFILES(X86)": str(c_drive / "Program Files (x86)")}
        for failure in (PermissionError("execution blocked"),
                        subprocess.CalledProcessError(1, [str(vswhere)], stderr="restricted policy")):
            diagnostic = io.StringIO()
            with self.subTest(failure=failure), redirect_stderr(diagnostic), \
                    patch.object(build.subprocess, "run", side_effect=failure):
                env, (cc, _), _ = build.prepare_environment(build.parse_args([]), "msvc", {}, environment)
            self.assertEqual(cc, str(selected_vs / "VC/Tools/MSVC" / self.vc.name / "bin/Hostx64/x64/cl.exe"))
            self.assertEqual(env["WINDOWSSDKDIR"], str(selected_kits) + "\\")
            self.assertEqual(self.which("cmake", env), str(selected_vs /
                             "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"))
            self.assertIn("warning: vswhere failed", diagnostic.getvalue())

    def test_explicit_vs_and_sdk_override_working_installations_on_other_drives(self):
        other_vs = self.drives[1] / "worksoft/VS2026"
        other_kits = self.drives[2] / "Windows Kits/10"
        build.shutil.copytree(self.vs, other_vs)
        build.shutil.copytree(self.kits, other_kits)
        env = dict(self.env, PATH=str(other_vs / "VC/Tools/MSVC" / self.vc.name / "bin/Hostx64/x64"),
                   INCLUDE="old include", LIB="old lib", WINDOWSSDKDIR=str(other_kits))
        selected, (cc, _), _ = build.prepare_environment(build.parse_args([]), "msvc", {}, env)
        self.assertEqual(cc, str(self.vc / "bin/Hostx64/x64/cl.exe"))
        self.assertEqual(selected["WINDOWSSDKDIR"], str(self.kits) + "\\")
        with self.assertRaisesRegex(ValueError, "ARIAREAD_VS_ROOT"):
            build.prepare_environment(build.parse_args([]), "msvc", {},
                                      dict(env, ARIAREAD_VS_ROOT=str(self.root / "missing explicit VS")))

    def test_multidrive_mingw_fallback_and_explicit_root_priority(self):
        destination = self.drives[3] / "worksoft/msys64/ucrt64/bin"
        build.shutil.copytree(self.mingw, destination)
        with patch.object(build.subprocess, "run", return_value=subprocess.CompletedProcess(
                [], 0, "x86_64-w64-mingw32\n")):
            _, (cc, _), _ = build.prepare_environment(build.parse_args([]), "mingw", {}, {"PATH": ""})
            self.assertEqual(cc, str(destination / "gcc.exe"))
            _, (cc, _), _ = build.prepare_environment(build.parse_args([]), "mingw", {},
                                                     {"PATH": str(destination), "MSYS2_ROOT": str(self.msys)})
            self.assertEqual(cc, str(self.mingw / "gcc.exe"))

    def test_standalone_cmake_in_program_files_is_available_without_path_entry(self):
        (self.cmake_bin / "cmake.exe").unlink()
        standalone = self.touch(self.drives[0] / "Program Files/CMake/bin/cmake.exe")
        env, _, _ = build.prepare_environment(build.parse_args([]), "msvc", {}, self.env)
        self.assertEqual(self.which("cmake", env), str(standalone))

    def test_openssl_finds_multidrive_perl_for_both_toolchains_only_when_requested(self):
        for directory in (self.drives[1] / "worksoft/msys64/ucrt64/bin",
                          self.drives[2] / "msys2/ucrt64/bin",
                          self.drives[4] / "Program Files/Strawberry/perl/bin"):
            perl = self.touch(directory / "perl.exe")
            for toolchain in ("msvc", "mingw"):
                with self.subTest(directory=directory, toolchain=toolchain), \
                        patch.object(build.subprocess, "run", return_value=subprocess.CompletedProcess(
                            [], 0, "x86_64-w64-mingw32\n")):
                    source = dict(self.env, MSYS2_ROOT=str(self.msys))
                    env, _, _ = build.prepare_environment(build.parse_args([]), toolchain, {}, source)
                    self.assertIsNone(self.which("perl", env))
                    env, _, _ = build.prepare_environment(build.parse_args(["--tls-backend", "openssl"]),
                                                          toolchain, {}, source)
                    self.assertEqual(self.which("perl", env), str(perl))
            perl.unlink()

    def test_openssl_respects_existing_perl_path_and_explicit_override(self):
        existing = self.touch(self.root / "existing perl/perl.exe")
        override = self.touch(self.root / "selected perl/perl.exe")
        self.touch(self.drives[1] / "Strawberry/perl/bin/perl.exe")
        args = build.parse_args(["--tls-backend", "openssl"])
        env, _, _ = build.prepare_environment(args, "msvc", {}, dict(self.env, PATH=str(existing.parent)))
        self.assertEqual(self.which("perl", env), str(existing))
        env, _, _ = build.prepare_environment(args, "msvc", {},
            dict(self.env, PATH=str(existing.parent), ARIAREAD_PERL_DIR=str(override.parent)))
        self.assertEqual(self.which("perl", env), str(override))

    def test_existing_developer_prompt_rejects_x86(self):
        env = dict(self.env, PATH=str(self.vc / "bin/Hostx64/x64"), INCLUDE="include", LIB="lib",
                   VSCMD_ARG_TGT_ARCH="x86")
        with self.assertRaisesRegex(ValueError, "x64 developer prompt"):
            build.prepare_environment(build.parse_args([]), "msvc", {}, env)

    def test_visual_studio_generator_works_without_ninja_and_selects_x64(self):
        (self.ninja_bin / "ninja.exe").unlink()
        self.invoke(["--generator", "Visual Studio 18 2026", "--configure-only"])
        install_environment, _ = self.dependency_install()
        _, configure_environment = self.configure_call()
        for env in (install_environment, configure_environment):
            self.assertEqual(env["CMAKE_GENERATOR"], "Visual Studio 18 2026")
            self.assertEqual(env["CMAKE_GENERATOR_PLATFORM"], "x64")
            self.assertEqual(env["CMAKE_GENERATOR_TOOLSET"], "version=14.51.36247")
        self.assertFalse(any(command[:2] == ["cmake", "--build"] for command, _, _ in self.commands))

    def test_ninja_generator_reports_missing_executable(self):
        (self.ninja_bin / "ninja.exe").unlink()
        with self.assertRaisesRegex(ValueError, "Ninja is missing"):
            build.prepare_environment(build.parse_args([]), "msvc", {}, self.env)

    def test_explicit_mingw_uses_ucrt64_compilers_and_separate_prefix(self):
        env = {"PATH": "", "MSYS2_ROOT": str(self.msys)}
        with patch.object(build.subprocess, "run", return_value=subprocess.CompletedProcess(
                [], 0, "x86_64-w64-mingw32\n")) as probe:
            _, prefix, _ = self.invoke(["--toolchain", "mingw"], env=env)
        self.assertEqual(probe.call_args.args[0], [str(self.mingw / "gcc.exe"), "-dumpmachine"])
        install_environment, options = self.dependency_install()
        self.assertEqual(Path(self.installs[0][0][2]), prefix)
        self.assertEqual(install_environment["CC"], subprocess.list2cmdline([str(self.mingw / "gcc.exe")]))
        self.assertNotIn(str(self.vs), install_environment["PATH"])

    def test_mingw_rejects_non_mingw_or_32bit_compiler_and_visual_studio(self):
        env = {"PATH": str(self.mingw)}
        for target in ("x86_64-pc-cygwin", "i686-w64-mingw32"):
            with self.subTest(target=target), patch.object(build.subprocess, "run",
                    return_value=subprocess.CompletedProcess([], 0, target)), \
                    self.assertRaisesRegex(ValueError, "Expected a MinGW x64 compiler"):
                build.prepare_environment(build.parse_args([]), "mingw", {}, env)
        with patch.object(build.subprocess, "run", return_value=subprocess.CompletedProcess(
                [], 0, "x86_64-w64-mingw32")), self.assertRaisesRegex(ValueError, "require --toolchain msvc"):
            build.prepare_environment(build.parse_args(["--generator", "Visual Studio 18 2026"]), "mingw", {}, env)

    def test_reusing_cache_with_another_generator_or_compiler_fails_before_fetch(self):
        for cache in ({"CMAKE_GENERATOR": "Visual Studio 18 2026"},
                      {"CMAKE_GENERATOR_TOOLSET": "version=14.52.10000"},
                      {"CMAKE_CXX_COMPILER": str(self.mingw / "g++.exe")}):
            with self.subTest(cache=cache), self.assertRaisesRegex(ValueError, "different (CMake generator|CMake toolset|compiler)"):
                self.invoke(["--generator", "Ninja"], cache=cache)
            self.assertEqual(self.commands, [])

    def test_runtime_build_forwards_profile_tls_offline_compilers_and_paths(self):
        folder, prefix, binary = self.invoke(["--offline", "--jobs", "3", "--tls-backend", "schannel",
                                             "--build-dir", str(self.root / "custom build"),
                                             "--deps-prefix", str(self.root / "custom deps")])
        install_environment, options = self.dependency_install()
        self.assertEqual(options["profile"], "runtime")
        self.assertEqual(options["tls_backend"], "schannel")
        self.assertEqual(Path(self.installs[0][0][2]), prefix)
        self.assertEqual(options["jobs"], 3)
        self.assertTrue(options["offline"])
        self.assertEqual(install_environment["CC"], subprocess.list2cmdline(
            [str(self.vc / "bin/Hostx64/x64/cl.exe")]))
        self.assertEqual(install_environment["CXX"], subprocess.list2cmdline(
            [str(self.vc / "bin/Hostx64/x64/cl.exe")]))
        configure, _ = self.configure_call()
        self.assertIn("-DARIAREAD_BUILD_TESTS=OFF", configure)
        self.assertIn("-DARIAREAD_BUILD_WEB_TESTS=OFF", configure)
        self.assertIn("-DARIAREAD_TLS_BACKEND=schannel", configure)
        self.assertIn("-DARIAREAD_DEPS_PREFIX=" + str(prefix), configure)
        compile_command = next(command for command, _, _ in self.commands if command[:2] == ["cmake", "--build"])
        self.assertEqual(compile_command, ["cmake", "--build", str(folder), "--config", "Release",
                                           "--parallel", "3", "--target", "ariaread_web_server"])
        self.assertEqual(self.commands[-1][0], [str(binary), "--help"])
        self.assertFalse(any(command[0] == "ctest" for command, _, _ in self.commands))

    def test_test_mode_builds_all_targets_and_runs_strict_bounded_ctest(self):
        folder, _, _ = self.invoke(["--test", "--require-web-tests", "--jobs", "2"])
        _, options = self.dependency_install()
        self.assertEqual(options["profile"], "tests")
        configure, _ = self.configure_call()
        for flag in ("BUILD_TESTS", "BUILD_WEB_TESTS", "REQUIRE_WEB_TESTS"):
            self.assertIn("-DARIAREAD_" + flag + "=ON", configure)
        compile_command = next(command for command, _, _ in self.commands if command[:2] == ["cmake", "--build"])
        self.assertNotIn("--target", compile_command)
        ctest = next(command for command, _, _ in self.commands if command[0] == "ctest")
        self.assertEqual(ctest, ["ctest", "--test-dir", str(folder), "-C", "Release", "--output-on-failure",
                                 "--no-tests=error", "--timeout", "120", "--parallel", "2"])

    def test_explicit_openssl_forwards_selection_and_can_add_native_perl(self):
        perl = self.root / "Strawberry/perl/bin"
        self.touch(perl / "perl.exe")
        self.invoke(["--tls-backend", "openssl", "--configure-only"], env=dict(self.env, ARIAREAD_PERL_DIR=str(perl)))
        install_environment, options = self.dependency_install()
        self.assertEqual(options["tls_backend"], "openssl")
        self.assertEqual(self.which("perl", install_environment), str(perl / "perl.exe"))
        self.assertIn("-DARIAREAD_TLS_BACKEND=openssl", self.configure_call()[0])

    def test_native_compiler_arguments_and_generator_are_shared_with_dependencies(self):
        cc = self.touch(self.root / "native toolchain/clang")
        cxx = self.touch(self.root / "native toolchain/clang++")
        env = {"PATH": str(self.cmake_bin), "CC": f'"{cc}" -arch arm64',
               "CXX": f'"{cxx}" -arch arm64'}
        self.invoke(["--generator", "Ninja", "--configure-only"], platform="darwin", env=env)
        install_environment, _ = self.dependency_install()
        self.assertEqual(install_environment["CMAKE_GENERATOR"], "Ninja")
        self.assertIn("-arch arm64", install_environment["CC"])
        self.assertIn("-DCMAKE_C_COMPILER_ARG1=-arch arm64", self.configure_call()[0])
        self.assertIn("-DCMAKE_CXX_COMPILER_ARG1=-arch arm64", self.configure_call()[0])

    def test_compiler_preflight_runs_first_with_same_compilers_generator_and_flags(self):
        cc = self.touch(self.root / "native toolchain/clang")
        cxx = self.touch(self.root / "native toolchain/clang++")
        environment = {"PATH": str(self.cmake_bin), "CC": f'"{cc}" -arch arm64',
                       "CXX": f'"{cxx}" -arch arm64', "CFLAGS": "-DFROM_ENV=1"}
        cache = {"CMAKE_GENERATOR": "Ninja", "CMAKE_CXX_FLAGS": "-DFROM_CACHE=1",
                 "CMAKE_CXX_FLAGS_RELEASE": "-O2", "CMAKE_OSX_ARCHITECTURES": "arm64"}
        self.invoke(["--configure-only"], platform="darwin", env=environment, cache=cache)
        preflight, preflight_env, _ = self.commands[0]
        configure, configure_env = self.configure_call()
        self.assertEqual(preflight[:2], ["cmake", "-S"])
        self.assertIn("-DARIAREAD_COMPILER_REQUIREMENTS=" +
                      str(self.root / "cmake/CompilerRequirements.cmake"), preflight)
        self.assertEqual(len(self.installs), 1)  # dependencies install after the preflight
        for option in (f"-DCMAKE_C_COMPILER={cc}", f"-DCMAKE_CXX_COMPILER={cxx}",
                       "-DCMAKE_C_COMPILER_ARG1=-arch arm64", "-DCMAKE_CXX_COMPILER_ARG1=-arch arm64",
                       "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_FLAGS=-DFROM_CACHE=1",
                       "-DCMAKE_CXX_FLAGS_RELEASE=-O2", "-DCMAKE_OSX_ARCHITECTURES=arm64"):
            self.assertIn(option, preflight)
            self.assertIn(option, configure)
        self.assertEqual(preflight_env, configure_env)
        self.assertEqual(preflight_env["CMAKE_GENERATOR"], "Ninja")
        self.assertEqual(preflight_env["CFLAGS"], "-DFROM_ENV=1")
        self.assertFalse(Path(preflight[2]).exists(), "temporary compiler probe must be cleaned up")

    def test_compiler_preflight_failure_stops_before_fetching_or_building_dependencies(self):
        def failed_preflight(command, env, **kwargs):
            self.record(command, env, **kwargs)
            self.assertEqual(str(command[0]), "cmake")
            self.assertTrue(any(str(arg).startswith("-DARIAREAD_COMPILER_REQUIREMENTS=") for arg in command))
            raise subprocess.CalledProcessError(1, command, stderr="unsupported compiler")

        with patch.object(build.sys, "platform", "win32"), patch.dict(os.environ, self.env, clear=True), \
                patch.object(build, "run", side_effect=failed_preflight), \
                patch.object(build.deps, "install", side_effect=self.record_install), self.assertRaises(subprocess.CalledProcessError):
            build.main(["--configure-only"])
        self.assertEqual(len(self.commands), 1)
        self.assertEqual(self.installs, [])
        self.assertFalse((self.root / "build/deps").exists())

    def test_native_rerun_without_cc_cxx_restores_cached_compiler_arguments(self):
        cc = self.touch(self.root / "native toolchain/clang")
        cxx = self.touch(self.root / "native toolchain/clang++")
        environment = {"PATH": str(self.cmake_bin), "CC": f'"{cc}" -arch arm64',
                       "CXX": f'"{cxx}" -arch arm64'}
        self.invoke(["--configure-only"], platform="darwin", env=environment)
        # Persist the first configure's actual arguments as CMake would.
        cache = dict(argument[2:].split("=", 1) for argument in self.configure_call()[0]
                     if argument.startswith("-DCMAKE_"))
        self.commands.clear()
        self.installs.clear()
        self.invoke(["--configure-only"], platform="darwin", env={"PATH": str(self.cmake_bin)}, cache=cache)
        install_environment, _ = self.dependency_install()
        self.assertIn("-arch arm64", install_environment["CC"])
        self.assertIn("-arch arm64", install_environment["CXX"])
        for language in ("C", "CXX"):
            self.assertIn(f"-DCMAKE_{language}_COMPILER_ARG1=-arch arm64", self.configure_call()[0])

    def test_native_rerun_with_explicit_bare_compilers_clears_cached_arguments(self):
        cc = self.touch(self.root / "native toolchain/clang")
        cxx = self.touch(self.root / "native toolchain/clang++")
        environment = {"PATH": str(self.cmake_bin), "CC": f'"{cc}" -arch arm64',
                       "CXX": f'"{cxx}" -arch arm64'}
        self.invoke(["--configure-only"], platform="darwin", env=environment)
        cache = dict(argument[2:].split("=", 1) for argument in self.configure_call()[0]
                     if argument.startswith("-DCMAKE_"))
        self.commands.clear()
        self.installs.clear()
        self.invoke(["--configure-only"], platform="darwin",
                    env=dict(environment, CC=str(cc), CXX=str(cxx)), cache=cache)
        install_environment, _ = self.dependency_install()
        self.assertNotIn("-arch", install_environment["CC"])
        self.assertNotIn("-arch", install_environment["CXX"])
        for language in ("C", "CXX"):
            self.assertIn(f"-DCMAKE_{language}_COMPILER_ARG1=", self.configure_call()[0])

    def test_clean_and_clean_only_preserve_installed_dependencies_and_downloads(self):
        args = build.parse_args([])
        folder, prefix = build.build_paths(args, "msvc")
        library = self.touch(prefix / "lib/retained.lib", "keep installed library")
        download = self.touch(self.root / "build/deps/downloads/retained.tar.gz", "keep download")
        self.invoke(["--clean-only"], cache={"CMAKE_GENERATOR": "Ninja"})
        self.assertEqual([item[0] for item in self.commands], [
            ["cmake", "--build", str(folder), "--config", "Release", "--target", "clean"]])
        self.assertEqual(library.read_text(), "keep installed library")
        self.assertEqual(download.read_text(), "keep download")
        self.commands.clear()
        self.invoke(["--clean"])
        self.assertTrue(any("--clean-first" in command for command, _, _ in self.commands))
        self.assertEqual(library.read_text(), "keep installed library")
        self.assertEqual(download.read_text(), "keep download")

    def test_clean_only_without_cache_does_not_require_a_toolchain(self):
        self.invoke(["--clean-only"], env={"PATH": ""})
        self.assertEqual(self.commands, [])

    def test_clean_only_executes_real_cmake_clean_without_deleting_nested_deps(self):
        # Exercise an actual CMake clean, with no compiler or download required.
        cmake = build.shutil.which("cmake", path=os.defpath)
        if cmake is None:
            # macOS package managers and Windows CI put CMake outside os.defpath.
            cmake = build.shutil.which("cmake", path=self.original_path)
        if cmake is None:
            self.skipTest("CMake unavailable for clean integration check")
        source = self.root / "clean fixture"
        folder = self.root / "build/clean fixture"
        self.touch(source / "CMakeLists.txt", '''cmake_minimum_required(VERSION 3.21)
project(CleanFixture NONE)
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/application-object"
    COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_BINARY_DIR}/application-object")
add_custom_target(application ALL DEPENDS "${CMAKE_BINARY_DIR}/application-object")
''')
        environment = dict(self.original_env)
        configure = [cmake, "-S", str(source), "-B", str(folder)]
        if os.name == "nt":
            # tools/build.py pins Ninja on Windows, and Ninja's clean target is
            # the one that removes custom-command outputs; the Visual Studio
            # generator leaves them, which would assert against a generator
            # the product never selects. The suite clears os.environ, so the
            # vswhere lookup must run against the captured real environment.
            ninja = build.shutil.which("ninja", path=self.original_path)
            if ninja is None:
                # The VS Installer lives at a fixed location even when the
                # surrounding environment is stripped (this suite clears
                # os.environ, and parenthesised variables can drop out of
                # inherited environments), so fall back to the standard path.
                installer_root = self.original_env.get("ProgramFiles(x86)") or \
                    r"C:\Program Files (x86)"
                vswhere = Path(installer_root) / \
                    "Microsoft Visual Studio/Installer/vswhere.exe"
                roots = []
                if vswhere.is_file():
                    listed = subprocess.run([str(vswhere), "-latest", "-property",
                                             "installationPath"], capture_output=True,
                                            text=True, env=self.original_env)
                    roots = [line.strip() for line in listed.stdout.splitlines() if line.strip()]
                for root in roots:
                    candidate = Path(root) / "Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
                    if candidate.is_file():
                        ninja = str(candidate)
                        break
            if ninja is None:
                self.skipTest("Ninja unavailable to pin the Windows generator")
            environment["PATH"] = str(Path(ninja).parent) + os.pathsep + environment.get("PATH", "")
            configure += ["-G", "Ninja"]
        subprocess.run(configure, env=environment, check=True, capture_output=True)
        subprocess.run([cmake, "--build", str(folder)], env=environment,
                       check=True, capture_output=True)
        artifact = folder / "application-object"
        self.assertTrue(artifact.exists())
        library = self.touch(folder / "deps/prefix/lib/library.a", "installed dependency")
        archive = self.touch(folder / "deps/downloads/source.tar.gz", "cached source")
        compiler = ("unused", [])

        def execute_clean(command, env, **kwargs):
            subprocess.run([cmake, *map(str, command[1:])], env=env, check=True, capture_output=True)

        with patch.object(build.sys, "platform", "darwin"), \
                patch.object(build, "prepare_environment", return_value=(environment, compiler, compiler)), \
                patch.object(build, "run", side_effect=execute_clean):
            self.assertEqual(build.main(["--clean-only", "--build-dir", str(folder)]), 0)
        self.assertFalse(artifact.exists())
        self.assertEqual(library.read_text(), "installed dependency")
        self.assertEqual(archive.read_text(), "cached source")

    def test_skip_cmake_refreshes_existing_runtime_without_build_dependencies(self):
        folder, _, binary = self.invoke(["--skip-cmake"], env={"PATH": str(self.cmake_bin)},
                                        cache={"CMAKE_CXX_COMPILER": str(self.vc / "bin/Hostx64/x64/cl.exe")})
        self.assertEqual(len(self.commands), 2)
        self.assertIn("-DARIAREAD_OUTPUT_DIR=" + str(folder / "bin"), self.commands[0][0])
        self.assertIn("-DARIAREAD_MINGW=OFF", self.commands[0][0])
        self.assertEqual(self.commands[-1][0], [str(binary), "--help"])

    def test_runtime_path_prefers_multi_config_directory_and_rejects_missing_binary(self):
        folder = self.root / "build"
        single = self.touch(folder / "bin/ariaread_web_server.exe")
        multi = self.touch(folder / "bin/Release/ariaread_web_server.exe")
        self.assertEqual(build.runtime_path(folder, "Release", True), (multi.parent, multi))
        multi.unlink()
        self.assertEqual(build.runtime_path(folder, "Release", True), (single.parent, single))
        single.unlink()
        with self.assertRaisesRegex(ValueError, "Server executable is missing"):
            build.runtime_path(folder, "Release", True)

    def test_invalid_option_combinations_and_job_counts_fail_early(self):
        for argv in (["--clean", "--skip-cmake"], ["--test", "--clean-only"],
                     ["--require-web-tests"], ["--jobs", "0"], ["--jobs", "257"], ["--jobs", "many"],
                     ["--prefix", "x"], ["--c-compiler", "x"], ["build", "--cmake-platform", "x"]):
            with self.subTest(argv=argv), self.assertRaises(SystemExit) as result:
                build.parse_args(argv)
            self.assertEqual(result.exception.code, 2)
        with patch.object(build.sys, "platform", "win32"), self.assertRaisesRegex(ValueError, "Release is required"):
            build.main(["--config", "Debug"])
        with patch.object(build.sys, "platform", "darwin"), self.assertRaisesRegex(ValueError, "only available on Windows"):
            build.main(["--tls-backend", "schannel"])
        with self.assertRaisesRegex(ValueError, "only supported on Windows"):
            build.selected_toolchain("msvc", False)

    def test_deps_update_refreshes_the_lock_through_the_library(self):
        updates = []
        with patch.object(build.deps, "update_lock",
                          side_effect=lambda file, work, **options: updates.append(
                              (str(file), str(work), options))):
            self.assertEqual(build.main(["deps-update", "--only", "aria", "--version", "aria=3.1.1"]), 0)
        file, work, options = updates[0]
        self.assertEqual(Path(file), self.root / "dependencies.json")
        self.assertEqual(Path(work), self.root / "build/deps")
        self.assertEqual(options["only"], ["aria"])
        self.assertEqual(options["versions"], {"aria": "3.1.1"})
        self.assertEqual(self.commands, [])  # no compiler discovery for a records-only command

    def test_cache_key_prints_the_machine_readable_scope(self):
        with patch.object(build.deps, "cache_key", return_value="scope-fixture"), \
                patch.object(build, "run") as shell:
            self.assertEqual(build.main(["cache-key"]), 0)
        shell.assert_not_called()  # the key is printed, not a child process's job
        self.assertEqual(self.commands, [])

    def test_deps_check_uses_cmake_compiler_bridge_without_discovery(self):
        with patch.dict(os.environ, self.env, clear=True), \
                patch.object(build, "run") as shell, \
                patch.object(build.deps, "verify", side_effect=self.record_verify):
            self.assertEqual(build.main(["deps-check", "--prefix", str(self.root / "prefix"),
                                         "--c-compiler", str(self.vc / "bin/Hostx64/x64/cl.exe"),
                                         "--cxx-compiler", str(self.vc / "bin/Hostx64/x64/cl.exe"),
                                         "--c-compiler-arg1=/DTEST", "--cmake-platform", "x64"]), 0)
        shell.assert_not_called()  # verification compiles nothing and runs no cmake
        arguments, environment, options = self.verifies[0]
        self.assertEqual(Path(arguments[2]), self.root / "prefix")
        self.assertEqual(options["c"], str(self.vc / "bin/Hostx64/x64/cl.exe"))
        self.assertEqual(options["c_arg1"], "/DTEST")
        self.assertEqual(options["cmake_platform"], "x64")
        self.assertEqual(environment["ARIAREAD_VS_ROOT"], str(self.vs))

    def test_deps_check_for_plain_users_discovers_the_toolchain_first(self):
        _, prefix, _ = self.invoke(["deps-check"], cache={"CMAKE_GENERATOR": "Ninja"})
        arguments, environment, options = self.verifies[0]
        self.assertEqual(Path(arguments[0]), self.root / "dependencies.json")
        self.assertEqual(Path(arguments[2]), prefix)
        self.assertEqual(options["profile"], "runtime")
        self.assertEqual(environment["CMAKE_GENERATOR"], "Ninja")
        self.assertEqual(environment["CC"], subprocess.list2cmdline(
            [str(self.vc / "bin/Hostx64/x64/cl.exe")]))

    def test_subprocess_failure_stops_before_runtime_success_check(self):
        def failing_run(command, env, **kwargs):
            self.record(command, env, **kwargs)
            if command[0] == "ctest":
                raise subprocess.CalledProcessError(8, command)
        with patch.object(build.sys, "platform", "win32"), patch.dict(os.environ, self.env, clear=True), \
                patch.object(build, "run", side_effect=failing_run), \
                patch.object(build.deps, "install", side_effect=self.record_install), self.assertRaises(subprocess.CalledProcessError):
            build.main(["--test"])
        self.assertEqual(self.commands[-1][0][0], "ctest")
        self.assertFalse(any("--help" in command for command, _, _ in self.commands))


if __name__ == "__main__":
    unittest.main()
