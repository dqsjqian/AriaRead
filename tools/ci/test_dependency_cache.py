#!/usr/bin/env python3
"""Offline regressions for dependency identity, rollback and source protection."""
import io
import json
import os
import sys
import tarfile
import subprocess
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path
from unittest.mock import patch

import build_ariaread_deps as builder
import dependency_cache as cache


class InstallationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.prefix = self.root / 'prefix'

    def install(self, identity='one', content='1.0', selected=None):
        selected = selected or {'library'}
        with cache.installation(self.prefix, identity, selected, {'lock': identity}) as (done, state):
            if done is not None:
                (self.prefix / 'library.a').write_text(content)
                done.update(selected)
                state['completed'] = sorted(done)
                state['version'] = content

    def test_exact_identity_reuses_verified_installation(self):
        self.install()
        with cache.installation(self.prefix, 'one', {'library'}, {}) as (done, state):
            self.assertIsNone(done)
            self.assertEqual(state['version'], '1.0')
        self.assertFalse(list(self.root.glob('prefix-backup-*')))

    def test_changed_version_rebuilds_in_empty_prefix_and_preserves_backup(self):
        self.install()
        with cache.installation(self.prefix, 'two', {'library'}, {'lock': 'two'}) as (done, state):
            self.assertFalse((self.prefix / 'library.a').exists())
            (self.prefix / 'library.a').write_text('2.0')
            state['completed'] = ['library']
        backup, = self.root.glob('prefix-backup-*')
        self.assertEqual((backup / 'library.a').read_text(), '1.0')
        self.assertEqual((self.prefix / 'library.a').read_text(), '2.0')
        self.assertEqual(cache.read_state(self.prefix)['lock'], 'two')

    def test_failed_upgrade_restores_original_prefix_and_preserves_failure(self):
        self.install()
        before = (self.prefix / cache.STATE).read_bytes()
        with self.assertRaisesRegex(RuntimeError, 'build failed'):
            with cache.installation(self.prefix, 'two', {'library'}, {}) as (done, state):
                (self.prefix / 'partial').write_text('preserve me')
                raise RuntimeError('build failed')
        self.assertEqual((self.prefix / cache.STATE).read_bytes(), before)
        self.assertEqual((self.prefix / 'library.a').read_text(), '1.0')
        failed, = self.root.glob('prefix-failed-*')
        self.assertTrue((failed / 'partial').exists())
        cache.read_state(self.prefix)

    def test_partial_install_completion_preserves_verified_existing_components(self):
        self.install()
        with cache.installation(self.prefix, 'one', {'library', 'headers'}, {}) as (done, state):
            self.assertEqual(done, {'library'})
            self.assertEqual((self.prefix / 'library.a').read_text(), '1.0')
            (self.prefix / 'header.h').write_text('header')
            state['completed'] = ['library', 'headers']
        self.assertEqual(set(cache.read_state(self.prefix)['completed']), {'library', 'headers'})

    def test_modified_installed_file_is_never_overwritten(self):
        self.install()
        (self.prefix / 'library.a').write_text('local edit')
        with self.assertRaisesRegex(ValueError, 'local modifications'):
            self.install('two', '2.0')
        self.assertEqual((self.prefix / 'library.a').read_text(), 'local edit')
        self.assertFalse(list(self.root.glob('prefix-backup-*')))

    def test_missing_installed_file_is_not_blessed_as_new_version(self):
        self.install()
        (self.prefix / 'library.a').unlink()
        with self.assertRaisesRegex(ValueError, 'missing files'):
            self.install('two', '2.0')
        self.assertEqual(json.loads((self.prefix / cache.STATE).read_text())['version'], '1.0')

    def test_added_local_file_is_preserved(self):
        self.install()
        (self.prefix / 'notes.txt').write_text('local notes')
        with self.assertRaises(ValueError):
            self.install('two')
        self.assertEqual((self.prefix / 'notes.txt').read_text(), 'local notes')

    def test_legacy_artifacts_are_backed_up_and_never_relabelled(self):
        self.prefix.mkdir()
        (self.prefix / 'library.a').write_text('unknown old binary')
        self.install('new', 'new binary')
        backup, = self.root.glob('prefix-backup-*')
        self.assertEqual((backup / 'library.a').read_text(), 'unknown old binary')
        self.assertEqual((self.prefix / 'library.a').read_text(), 'new binary')

    def test_stale_lock_is_rejected_before_cmake_can_use_prefix(self):
        self.install()
        with patch.object(builder, 'recipe_digest', return_value='recipe'):
            with self.assertRaisesRegex(ValueError, 'resolution or recipe changed'):
                builder.verify_prefix(self.prefix, {'new': 'lock'}, [], {'library'})

    def test_manifest_write_failure_also_restores_old_installation(self):
        self.install()
        original = (self.prefix / cache.STATE).read_bytes()
        with patch.object(cache, 'write_state', side_effect=OSError('disk full')):
            with self.assertRaisesRegex(OSError, 'disk full'):
                self.install('two', '2.0')
        self.assertEqual((self.prefix / cache.STATE).read_bytes(), original)
        self.assertEqual((self.prefix / 'library.a').read_text(), '1.0')

    def test_concurrent_install_is_rejected(self):
        with cache.prefix_lock(self.prefix):
            with self.assertRaisesRegex(ValueError, 'already active'):
                with cache.prefix_lock(self.prefix):
                    self.fail('second writer entered')
        self.assertFalse((self.root / 'prefix.install-lock').exists())

    def test_prefix_symlink_is_rejected(self):
        destination = self.root / 'real'
        destination.mkdir()
        try:
            self.prefix.symlink_to(destination, target_is_directory=True)
        except OSError:
            self.skipTest('symlink privilege unavailable')
        with self.assertRaisesRegex(ValueError, 'symlink'):
            self.install()


class SourceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def dep(self, data, url='https://example.invalid/source.tar.gz'):
        return replace(builder.RECIPES[0], version='1.0', url=url,
                       sha256=cache.hashlib.sha256(data).hexdigest())

    def test_exact_offline_content_cache_is_reused(self):
        data = b'locked archive'
        dep = self.dep(data)
        (self.root / dep.sha256).write_bytes(data)
        # Imported helpers may use a Windows redirected console, unlike main()
        # which explicitly configures UTF-8. Progress must work in that case.
        output = io.BytesIO()
        with io.TextIOWrapper(output, encoding='cp1252') as console, patch.object(builder.sys, 'stdout', console):
            archive = builder.download(self.root, dep, True)
            console.flush()
            self.assertIn(b'Reusing verified archive', output.getvalue())
        self.assertEqual(archive.read_bytes(), data)

    def test_changed_hash_cannot_reuse_same_archive_basename(self):
        old = self.dep(b'old')
        (self.root / old.archive_name).write_bytes(b'old')
        with self.assertRaisesRegex(ValueError, '离线缓存缺失'):
            builder.download(self.root, self.dep(b'new'), True)
        self.assertEqual((self.root / old.archive_name).read_bytes(), b'old')

    def test_broken_archive_symlink_never_writes_outside_cache(self):
        data = b'archive'
        dep = self.dep(data)
        (self.root / dep.sha256).write_bytes(data)
        outside = self.root / 'outside'
        archive = self.root / (dep.sha256 + '-' + dep.archive_name)
        try:
            archive.symlink_to(outside)
        except OSError:
            self.skipTest('symlink privilege unavailable')
        with self.assertRaisesRegex(ValueError, 'symbolic-link'):
            builder.download(self.root, dep, True)
        self.assertFalse(outside.exists())
        self.assertTrue(archive.is_symlink())

    def test_unrelated_or_shared_library_does_not_satisfy_static_artifact(self):
        lib = self.root / 'lib'
        lib.mkdir()
        for name in ('libzstd.a', 'libz.dylib', 'zlib.dll'):
            (lib / name).write_text('wrong binary')
        self.assertFalse(builder.artifact_present(self.root, 'lib/libz.a'))
        (lib / 'zlibstatic.lib').write_text('correct static spelling')
        self.assertTrue(builder.artifact_present(self.root, 'lib/libz.a'))

    def test_windows_zlib_static_names_preserve_exports_and_support_findzlib(self):
        for original, compatible in [('zs.lib', 'zlibstatic.lib'), ('libzs.a', 'libz.a')]:
            with self.subTest(original=original):
                prefix = self.root / original
                (prefix / 'lib').mkdir(parents=True)
                (prefix / 'lib' / original).write_bytes(b'compiled static library')
                builder.normalize_zlib_static(prefix)
                self.assertEqual((prefix / 'lib' / original).read_bytes(), b'compiled static library')
                self.assertEqual((prefix / 'lib' / compatible).read_bytes(), b'compiled static library')
                self.assertTrue(builder.artifact_present(prefix, 'lib/libz.a'))
                if cache.shutil.which('cmake'):
                    (prefix / 'include').mkdir()
                    (prefix / 'include/zlib.h').write_text('#define ZLIB_VERSION "1.3.2"\n')
                    script = prefix / 'CMakeLists.txt'
                    script.write_text('cmake_minimum_required(VERSION 3.20)\n'
                                      'set(CMAKE_SYSTEM_NAME Windows)\nproject(FindZlibFixture NONE)\n'
                                      'set(CMAKE_FIND_LIBRARY_PREFIXES "" "lib")\n'
                                      'set(CMAKE_FIND_LIBRARY_SUFFIXES ".a" ".lib")\n'
                                      f'set(ZLIB_ROOT "{prefix.as_posix()}")\n'
                                      'set(ZLIB_USE_STATIC_LIBS ON)\nfind_package(ZLIB REQUIRED)\n'
                                      f'if(NOT ZLIB_LIBRARY_RELEASE STREQUAL "{(prefix / "lib" / compatible).as_posix()}")\n'
                                      'message(FATAL_ERROR "FindZLIB selected another library: ${ZLIB_LIBRARY_RELEASE}")\nendif()\n')
                    found = subprocess.run(['cmake', '-S', str(prefix), '-B', str(prefix / 'build')], encoding='utf-8', errors='replace',
                                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                    self.assertEqual(found.returncode, 0, found.stdout)
                builder.normalize_zlib_static(prefix)
                (prefix / 'lib' / compatible).write_bytes(b'local modification')
                with self.assertRaisesRegex(ValueError, 'Conflicting zlib'):
                    builder.normalize_zlib_static(prefix)
                self.assertEqual((prefix / 'lib' / compatible).read_bytes(), b'local modification')

    def test_corrupt_content_cache_is_rejected_and_preserved(self):
        dep = self.dep(b'original')
        path = self.root / dep.sha256
        path.write_bytes(b'local edit')
        with self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
            builder.download(self.root, dep, True)
        self.assertEqual(path.read_bytes(), b'local edit')

    def test_dirty_git_cache_is_rejected(self):
        source = self.root / 'source'
        subprocess.run(['git', 'init', '-q', str(source)], check=True)
        (source / 'file').write_text('original')
        subprocess.run(['git', '-C', str(source), 'add', 'file'], check=True)
        subprocess.run(['git', '-C', str(source), '-c', 'user.name=Fixture', '-c',
                        'user.email=fixture@example.invalid', 'commit', '-qm', 'fixture'], check=True)
        revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
        target = self.root / 'git/Mira' / revision
        target.parent.mkdir(parents=True)
        source.rename(target)
        dep = replace(builder.RECIPES[-1], version='1', revision=revision)
        self.assertEqual(builder.fetch_git(self.root, dep, True), target)
        (target / 'untracked').write_text('keep')
        with self.assertRaisesRegex(ValueError, 'local changes'):
            builder.fetch_git(self.root, dep, True)
        self.assertEqual((target / 'untracked').read_text(), 'keep')

    def test_new_version_never_receives_old_patch(self):
        records = {dep.name: {'version': next(iter(builder.PATCH_VERSIONS.get(dep.name, {'1.0'}))),
                   'requested': 'latest', 'url': 'https://example.invalid/a', 'sha256': 'a' * 64, 'revision': 'b' * 40,
                   'source': {'artifact': 'git' if dep.kind == 'git' else 'archive'}}
                   for dep in builder.RECIPES}
        records['quickjs']['version'] = '2099-01-01'
        with self.assertRaisesRegex(ValueError, 'explicitly reviewed'):
            builder.locked_recipes({'dependencies': records})

    def test_compiler_alias_banner_is_accepted_but_version_change_is_not(self):
        cc = {'path': '/usr/bin/cc', 'arguments': [], 'binary_sha256': 'same', 'version': 'cc (GCC) 16.0\nmore'}
        gcc = {**cc, 'path': '/usr/bin/gcc', 'version': 'gcc (GCC) 16.0\nmore'}
        self.assertTrue(cache.same_compiler(cc, gcc))
        self.assertFalse(cache.same_compiler(cc, {**gcc, 'version': 'gcc (GCC) 17.0\nmore'}))

    def test_abi_flags_and_toolchain_changes_are_rejected(self):
        context = {'environment': {'CXXFLAGS': '-DOLD_ABI'}}
        with patch.dict(cache.os.environ, {}, clear=True):
            with self.assertRaisesRegex(ValueError, 'CXXFLAGS'):
                cache.verify_environment(context)
            cache.verify_environment({'environment': {'CL': '/utf-8'}})
            toolchain = self.root / 'toolchain.cmake'
            toolchain.write_text('old')
            context = {'environment': {'CMAKE_TOOLCHAIN_FILE': str(toolchain),
                       'toolchain_sha256': cache.hashlib.sha256(b'old').hexdigest()}}
            cache.verify_environment(context, str(toolchain))
            toolchain.write_text('new')
            with self.assertRaisesRegex(ValueError, 'toolchain changed'):
                cache.verify_environment(context, str(toolchain))

    def test_msvc_generators_keep_platform_and_configuration_separate(self):
        for generator in ('Visual Studio 18 2026', 'Ninja', 'Ninja Multi-Config', 'NMake Makefiles'):
            with self.subTest(generator=generator), patch.dict(builder.os.environ, {'CMAKE_GENERATOR': generator}, clear=True), \
                    patch.object(builder.sys, 'platform', 'win32'), \
                    patch.object(builder, 'windows_toolchain', return_value='msvc'), \
                    patch.object(builder, 'run') as run:
                builder.build_cmake(self.root, self.root / 'build', self.root / 'prefix', 1, builder.RECIPES[0], [])
                commands = [call.args[0] for call in run.call_args_list]
                self.assertEqual('-A' in commands[0], generator.startswith('Visual Studio'))
                self.assertEqual(commands[1][-2:], ['--config', 'Release'])
                self.assertEqual(commands[2][-2:], ['--config', 'Release'])

    def test_selected_gcc_is_not_overridden_by_msvc_on_path(self):
        with patch.dict(builder.os.environ, {'CC': '/toolchain/gcc'}, clear=True), \
                patch.object(builder.shutil, 'which', return_value='/unrelated/cl'):
            self.assertEqual(builder.windows_toolchain(), 'mingw')
        with patch.dict(builder.os.environ, {'CC': '"/toolchain with spaces/cl.exe"'}, clear=True):
            self.assertEqual(builder.windows_toolchain(), 'msvc')

    def test_compiler_path_with_spaces_is_one_executable(self):
        executable = self.root / 'compiler with spaces'
        executable.write_text('fixture')
        result = subprocess.CompletedProcess([], 0, stdout='compiler 1.0')
        with patch.object(cache.shutil, 'which', return_value=str(executable)), \
                patch.object(cache.subprocess, 'run', return_value=result) as run:
            record = cache.compiler(str(executable))
        self.assertEqual(run.call_args.args[0], [str(executable), '--version'])
        self.assertEqual(record['path'], str(executable.resolve()))

    def test_compiler_and_patch_changes_alter_identity(self):
        baseline = {'lock': 'same', 'compiler': 'A', 'patch': 'one'}
        self.assertNotEqual(cache.fingerprint(baseline), cache.fingerprint({**baseline, 'compiler': 'B'}))
        dep = replace(next(dep for dep in builder.RECIPES if dep.name == 'gumbo'), version='1', url='url', sha256='a' * 64)
        with patch.object(builder, 'sha256', return_value='one'):
            old = builder.recipe_digest([dep])
        with patch.object(builder, 'sha256', return_value='two'):
            new = builder.recipe_digest([dep])
        self.assertNotEqual(old, new)


    def test_recipe_identity_tracks_build_helpers_but_not_cli_storage(self):
        dependency = builder.RECIPES[0]
        expected = builder.recipe_digest([dependency])
        with patch.object(builder, 'main', lambda: None):
            self.assertEqual(builder.recipe_digest([dependency]), expected)
        original = builder.inspect.getsource
        for helper in (builder.cmake_arguments, builder.build_environment, builder.prepare_source,
                       builder.windows_toolchain, builder.install_component, cache.build_context):
            with self.subTest(helper=helper.__name__), patch.object(builder.inspect, 'getsource',
                    side_effect=lambda function: original(function) + ('\n# changed' if function is helper else '')):
                self.assertNotEqual(builder.recipe_digest([dependency]), expected)


class BuilderIntegrationTests(unittest.TestCase):
    def test_cli_and_resolver_emit_utf8_with_legacy_parent_encoding(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            missing = root / '\u4f9d\u8d56-missing.json'
            result = subprocess.run([sys.executable, str(Path(builder.__file__)),
                '--path', str(root / 'work'), '--file', str(missing), '--offline'],
                env={**os.environ, 'PYTHONIOENCODING': 'cp1252'}, encoding='utf-8',
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(missing.name, result.stdout)
            self.assertNotIn('UnicodeEncodeError', result.stdout)
            self.assertNotIn('UnicodeDecodeError', result.stdout)

    def test_real_offline_install_upgrade_and_failed_upgrade(self):
        if not cache.shutil.which('cmake'):
            self.skipTest('CMake unavailable')
        with tempfile.TemporaryDirectory(prefix='ariaread-deps-fixture-') as temporary:
            root = Path(temporary)
            work = root / 'work'
            (work / 'cache').mkdir(parents=True)
            specs = {'schema': 2, 'dependencies': {dep.name: {'provider': 'github',
                     'repo': 'example/fixture', 'artifact': 'git' if dep.kind == 'git' else 'archive'}
                     for dep in builder.RECIPES}}
            records = {dep.name: {'version': next(iter(builder.PATCH_VERSIONS.get(dep.name, {'1.0'}))),
                       'requested': 'latest', 'url': 'https://example.invalid/archive.tar.gz',
                       'sha256': 'a' * 64, 'revision': 'b' * 40, 'source': specs['dependencies'][dep.name]}
                       for dep in builder.RECIPES}
            file = root / 'dependencies.json'

            def archive(version, fail=False):
                contents = {'LICENSE.MIT': 'Fixture license', 'json.hpp': version,
                            'CMakeLists.txt': ('cmake_minimum_required(VERSION 3.20)\n'
                              'project(Fixture C CXX)\n' + ('message(FATAL_ERROR "fixture build failure")\n' if fail else
                              'install(FILES json.hpp DESTINATION include/nlohmann)\n'))}
                data = io.BytesIO()
                with tarfile.open(fileobj=data, mode='w:gz') as package:
                    for name, text in contents.items():
                        encoded = text.encode()
                        info = tarfile.TarInfo('source/' + name)
                        info.size = len(encoded)
                        package.addfile(info, io.BytesIO(encoded))
                digest = cache.hashlib.sha256(data.getvalue()).hexdigest()
                (work / 'cache' / digest).write_bytes(data.getvalue())
                records['json'].update(version=version, sha256=digest)
                entries = {}
                for name, record in records.items():
                    spec = specs['dependencies'][name]
                    resolved = {key: value for key, value in record.items() if key != 'source'}
                    resolved['request_hash'] = builder.dependency_resolver.request_hash(spec)
                    entries[name] = {**spec, 'resolved': resolved}
                file.write_text(json.dumps({'schema': 2, 'dependencies': entries}))

            def invoke(*extra):
                return subprocess.run([sys.executable, str(Path(builder.__file__)),
                    '--path', str(work), '--file', str(file),
                    '--only', 'json', '--offline', '--jobs', '1', *extra], text=True, encoding='utf-8', errors='replace',
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

            archive('1.0')
            first = invoke()
            self.assertEqual(first.returncode, 0, first.stdout)
            header = work / 'prefix/include/nlohmann/json.hpp'
            self.assertEqual(header.read_text(), '1.0')
            repeated = invoke()
            self.assertEqual(repeated.returncode, 0, repeated.stdout)
            self.assertIn('Reusing verified dependency prefix', repeated.stdout)
            specs['dependencies']['json']['version'] = '1.0'
            records['json']['requested'] = '2.0'
            archive('2.0')
            upgraded = invoke('--version', 'json=2.0')
            self.assertEqual(upgraded.returncode, 0, upgraded.stdout)
            self.assertEqual(header.read_text(), '2.0')
            backup, = work.glob('prefix-backup-*')
            self.assertEqual((backup / 'include/nlohmann/json.hpp').read_text(), '1.0')
            snapshot = file.read_bytes()
            verified = invoke('--verify-prefix')
            self.assertEqual(verified.returncode, 0, verified.stdout)
            self.assertEqual(file.read_bytes(), snapshot)
            modified = json.loads(snapshot)
            modified['dependencies']['json']['version'] = '9.0'
            file.write_text(json.dumps(modified))
            stale = invoke('--verify-prefix')
            self.assertNotEqual(stale.returncode, 0)
            self.assertIn('declaration changed', stale.stdout)
            self.assertEqual(header.read_text(), '2.0')
            file.write_bytes(snapshot)
            records['json']['requested'] = '3.0'
            archive('3.0', fail=True)
            failed = invoke('--version', 'json=3.0')
            self.assertNotEqual(failed.returncode, 0)
            self.assertIn('fixture build failure', failed.stdout)
            self.assertEqual(header.read_text(), '2.0')
            self.assertEqual(cache.read_state(work / 'prefix')['dependencies'][0]['version'], '2.0')


if __name__ == '__main__':
    unittest.main()
