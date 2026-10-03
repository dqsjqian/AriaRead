#!/usr/bin/env python3
"""Offline regressions for the unified dependency library (no network access)."""
import io
import json
import os
import shlex
import sys
import tarfile
import subprocess
import tempfile
import unittest
from contextlib import redirect_stdout
from dataclasses import replace
from pathlib import Path
from unittest.mock import patch

import deps_build as deps

DOCTEST_FIXTURE = Path(__file__).with_name('fixtures') / 'doctest-2.5.3'
SHA = "a" * 40
DIGEST = "b" * 64
SOURCE = {"provider": "github", "repo": "example/library", "artifact": "git", "tag_prefix": "v"}


class FakeContext:
    offline = False

    def __init__(self):
        self.calls = []
        self.releases = [
            {"tag_name": "v1.9.0"}, {"tag_name": "v2.0.0-rc1", "prerelease": True},
            {"tag_name": "v1.10.0"}, {"tag_name": "v99.0.0", "draft": True},
        ]
        self.tags = []
        self.annotation = None
        self.fail = False

    def github_json(self, path):
        self.calls.append(path)
        if self.offline or self.fail:
            raise AssertionError("Unexpected network lookup")
        if "/releases?" in path:
            return self.releases
        if "/tags?" in path:
            return self.tags
        if "/releases/tags/" in path:
            tag = path.rsplit("/", 1)[1]
            return next((x for x in self.releases if x["tag_name"] == tag), None)
        if "/git/ref/tags/" in path:
            return {"object": self.annotation or {"type": "commit", "sha": SHA}}
        if "/git/tags/" in path:
            return {"object": {"type": "commit", "sha": SHA}}
        raise AssertionError(path)

    def download_digest(self, url, expected_sha256=None):
        if self.offline:
            raise AssertionError("Unexpected download")
        self.calls.append(url)
        return DIGEST


class DependencyTests(unittest.TestCase):
    """Lock resolution: selection, pinning and failure semantics."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ariaread-dependencies-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.manifest = self.root / "dependencies.json"
        self.lock = self.manifest
        self.context = FakeContext()
        self.write_manifest({"library": SOURCE})

    def write_manifest(self, dependencies):
        previous = json.loads(self.manifest.read_text())["dependencies"] if self.manifest.exists() else {}
        entries = {}
        for name, spec in dependencies.items():
            entries[name] = dict(spec)
            if name in previous and "resolved" in previous[name]:
                entries[name]["resolved"] = previous[name]["resolved"]
        self.manifest.write_text(json.dumps({"schema": 2, "dependencies": entries}))

    def resolve(self, **kwargs):
        return deps.resolve(self.manifest, context=self.context, **kwargs)

    def test_latest_means_highest_stable_release(self):
        record = self.resolve()["dependencies"]["library"]
        self.assertEqual(record["version"], "1.10.0")
        self.assertEqual(record["revision"], SHA)
        self.assertEqual(record["requested"], "latest")

    def test_locked_resolution_is_offline_and_does_not_rewrite(self):
        self.resolve()
        content = self.lock.read_bytes()
        timestamp = self.lock.stat().st_mtime_ns
        self.context.offline = True
        self.context.calls.clear()
        self.resolve()
        self.assertEqual(self.context.calls, [])
        self.assertEqual(self.lock.read_bytes(), content)
        self.assertEqual(self.lock.stat().st_mtime_ns, timestamp)

    def test_update_is_explicit_and_refreshes_latest(self):
        self.resolve()
        self.context.releases.append({"tag_name": "v2.0.0"})
        self.assertEqual(self.resolve()["dependencies"]["library"]["version"], "1.10.0")
        self.assertEqual(self.resolve(update=True)["dependencies"]["library"]["version"], "2.0.0")

    def test_explicit_version_wins_and_remains_locked(self):
        self.write_manifest({"library": {**SOURCE, "version": "1.9.0"}})
        result = self.resolve(versions={"library": "1.10.0"})
        self.assertEqual(result["dependencies"]["library"]["version"], "1.10.0")
        # Removing a command-line override restores an explicit manifest request.
        self.assertEqual(self.resolve()["dependencies"]["library"]["version"], "1.9.0")
        self.write_manifest({"library": SOURCE})
        self.resolve(versions={"library": "1.9.0"})
        self.context.offline = True
        self.assertEqual(self.resolve()["dependencies"]["library"]["version"], "1.9.0")

    def test_mixed_persistent_pins_and_latest(self):
        self.write_manifest({"fixed_one": {**SOURCE, "version": "1.9.0"},
                             "fixed_two": {**SOURCE, "version": "1.10.0"},
                             "rolling": SOURCE})
        self.context.releases.append({"tag_name": "v2.0.0"})
        selected = self.resolve(update=True)["dependencies"]
        self.assertEqual({name: record["version"] for name, record in selected.items()},
                         {"fixed_one": "1.9.0", "fixed_two": "1.10.0", "rolling": "2.0.0"})
        self.context.releases.append({"tag_name": "v3.0.0"})
        self.resolve(update=True, only=["rolling"])
        next_selection = deps.read_resolved(self.manifest)["dependencies"]
        self.assertEqual(next_selection["rolling"]["version"], "3.0.0")
        for name in ["fixed_one", "fixed_two"]:
            self.assertEqual(next_selection[name], selected[name])

    def test_selecting_the_already_locked_version_needs_no_network(self):
        self.resolve()
        self.context.offline = True
        record = self.resolve(versions={"library": "1.10.0"})["dependencies"]["library"]
        self.assertEqual(record["requested"], "1.10.0")

    def test_explicit_latest_policy_still_locks_normal_builds(self):
        self.write_manifest({"library": {**SOURCE, "version": "latest"}})
        self.resolve(versions={"library": "1.9.0"})
        self.context.offline = True
        self.assertEqual(self.resolve()["dependencies"]["library"]["version"], "1.9.0")

    def test_unknown_version_override_is_rejected(self):
        with self.assertRaisesRegex(deps.DependencyError, "declared dependency"):
            self.resolve(versions={"typo": "1.0"})
        self.assertNotIn("resolved", json.loads(self.manifest.read_text())["dependencies"]["library"])

    def test_explicit_branch_or_prerelease_is_rejected_before_network(self):
        for version in ["main", "2.0.0-rc1", "v2.0.0-beta", "../bad"]:
            with self.subTest(version=version), self.assertRaises(deps.DependencyError):
                self.resolve(versions={"library": version})
        self.assertEqual(self.context.calls, [])

    def test_override_outside_selection_is_not_silently_ignored(self):
        self.write_manifest({"library": SOURCE, "other": SOURCE})
        with self.assertRaisesRegex(deps.DependencyError, "selected by --only"):
            self.resolve(only=["library"], versions={"other": "1.9.0"})
        self.assertNotIn("resolved", json.loads(self.manifest.read_text())["dependencies"]["library"])

    def test_malformed_locked_record_is_reported_without_mutation(self):
        self.lock.write_text(json.dumps({"schema": 2, "dependencies": {"library": {**SOURCE, "resolved": []}}}))
        before = self.lock.read_bytes()
        with self.assertRaisesRegex(deps.DependencyError, "invalid resolved"):
            self.resolve()
        self.assertEqual(self.lock.read_bytes(), before)

    def test_new_selection_offline_does_not_mutate_lock(self):
        self.resolve()
        old = self.lock.read_bytes()
        self.context.offline = True
        with self.assertRaisesRegex(deps.DependencyError, "matching resolved"):
            self.resolve(versions={"library": "1.9.0"})
        self.assertEqual(self.lock.read_bytes(), old)

    def test_resolution_failure_is_transactional(self):
        self.resolve()
        self.write_manifest({"library": SOURCE, "broken": {"provider": "unsupported"}})
        old = self.lock.read_bytes()
        with self.assertRaisesRegex(deps.DependencyError, "Unsupported"):
            self.resolve(update=True)
        self.assertEqual(self.lock.read_bytes(), old)

    def test_effective_lock_preserves_source_and_unselected_records(self):
        self.resolve()
        original = self.lock.read_bytes()
        effective = self.root / "build" / "effective.json"
        result = deps.resolve(self.manifest, effective,
                              versions={"library": "1.9.0"}, context=self.context)
        self.assertEqual(self.lock.read_bytes(), original)
        self.assertEqual(result["dependencies"]["library"]["version"], "1.9.0")

    def test_lock_hash_revision_and_url_are_validated(self):
        record = self.resolve()["dependencies"]["library"]
        for changed in [{"revision": "main"}, {"url": "file:///tmp/private"},
                        {"source": {**SOURCE, "artifact": "archive"}, "sha256": "bad"}]:
            with self.subTest(changed=changed), self.assertRaises(deps.DependencyError):
                deps.validate_record({**record, **changed})

    def test_tag_only_projects_and_annotated_tags(self):
        self.context.releases = []
        self.context.tags = [{"name": "main"}, {"name": "v2.2.0"}, {"name": "v3.0.0-beta"}]
        self.context.annotation = {"type": "tag", "sha": "c" * 40}
        record = self.resolve()["dependencies"]["library"]
        self.assertEqual(record["tag"], "v2.2.0")
        self.assertEqual(record["revision"], SHA)

    def test_tag_separator_and_asset_templates(self):
        source = {**SOURCE, "tag_prefix": "curl-", "tag_separator": "_",
                  "artifact": "release-asset", "asset": "curl-{version}.tar.xz"}
        self.write_manifest({"library": source})
        self.context.releases = [{"tag_name": "curl-8_22_0", "assets": [
            {"name": "curl-8.22.0.tar.xz", "browser_download_url": "https://example.invalid/curl.tar.xz",
             "digest": "sha256:" + DIGEST}]}]
        record = self.resolve(versions={"library": "8.22.0"})["dependencies"]["library"]
        self.assertEqual(record["version"], "8.22.0")
        self.assertEqual(record["tag"], "curl-8_22_0")
        self.assertEqual(record["sha256"], DIGEST)
        self.assertEqual(record["checksum_source"], "github-release-asset")

    def test_request_fingerprint_is_portable_and_detects_edits(self):
        self.assertEqual(deps.request_hash(SOURCE),
                         "1044731845c62e4a50b0251f6039d78e6ce1233cdcac02a2b086809fb6526f54")
        self.resolve()
        value = json.loads(self.manifest.read_text())
        self.assertNotIn("source", value["dependencies"]["library"]["resolved"])
        value["dependencies"]["library"]["repo"] = "example/another"
        self.manifest.write_text(json.dumps(value))
        with self.assertRaisesRegex(deps.DependencyError, "declaration changed"):
            deps.read_resolved(self.manifest)
        self.context.offline = True
        with self.assertRaisesRegex(deps.DependencyError, "matching resolved"):
            self.resolve()

    def test_effective_output_tracks_base_changes_and_keeps_other_overrides(self):
        self.write_manifest({"library": SOURCE, "other": SOURCE})
        self.resolve()
        effective = self.root / "build" / "effective.json"
        deps.resolve(self.manifest, effective, versions={"library": "1.9.0"},
                     only=["library"], context=self.context)
        deps.resolve(self.manifest, effective, versions={"other": "1.9.0"},
                     only=["other"], context=self.context)
        self.assertEqual(deps.read_resolved(effective)["dependencies"]["library"]["version"], "1.9.0")
        before = self.manifest.read_bytes()
        self.context.offline = True
        deps.resolve(self.manifest, effective, versions={"library": "1.9.0"},
                     only=["library"], context=self.context)
        self.assertEqual(self.manifest.read_bytes(), before)
        # Deleting a persisted result invalidates the isolated output too.
        value = json.loads(self.manifest.read_text())
        del value["dependencies"]["library"]["resolved"]
        self.manifest.write_text(json.dumps(value))
        with self.assertRaisesRegex(deps.DependencyError, "matching resolved"):
            deps.resolve(self.manifest, effective, only=["library"], context=self.context)
        self.context.offline = False
        self.context.releases.append({"tag_name": "v3.0.0"})
        selected = deps.resolve(self.manifest, effective, only=["library"], context=self.context)
        self.assertEqual(selected["dependencies"]["library"]["version"], "3.0.0")

    def test_read_only_selection_allows_an_unresolved_other_entry(self):
        self.write_manifest({"library": SOURCE, "other": SOURCE})
        self.resolve(only=["library"])
        record = deps.read_resolved(self.manifest, only=["library"])["dependencies"]["library"]
        self.assertEqual(record["version"], "1.10.0")
        with self.assertRaisesRegex(deps.DependencyError, "Missing or invalid"):
            deps.read_resolved(self.manifest)


class InstallationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.prefix = self.root / 'prefix'

    def test_license_collection_keeps_embedded_notices_and_sqlite_dedication(self):
        source = self.root / 'source'
        headers = source / 'include/nlohmann/detail'
        headers.mkdir(parents=True)
        (source / 'LICENSE.MIT').write_text('selected MIT license')
        (source / 'NOTICE').write_text('selected upstream attribution')
        (headers / 'one.hpp').write_text('// SPDX-FileCopyrightText: First Author\n// SPDX-License-Identifier: MIT\n')
        (headers / 'two.hpp').write_text('// SPDX-FileCopyrightText: Second Author\n// SPDX-License-Identifier: MIT\n')
        dep = next(dep for dep in deps.RECIPES if dep.name == 'json')
        copied = deps.copy_licenses(self.prefix, source, dep)
        out = self.prefix / 'share/licenses/json'
        self.assertEqual((out / 'LICENSE.MIT').read_text(), 'selected MIT license')
        self.assertEqual((out / 'NOTICE').read_text(), 'selected upstream attribution')
        self.assertIn('ATTRIBUTIONS.txt', copied)
        self.assertIn('First Author', (out / 'ATTRIBUTIONS.txt').read_text())
        self.assertIn('Second Author', (out / 'ATTRIBUTIONS.txt').read_text())
        self.assertEqual((out / 'ATTRIBUTIONS.txt').read_text().count('SPDX-License-Identifier'), 1)
        dedication = '/*\n** The author disclaims copyright to this source code.\n** Blessing.\n'
        (source / 'sqlite3.h').write_text(dedication + '*' * 73 + '\nprivate implementation')
        sqlite = next(dep for dep in deps.RECIPES if dep.name == 'sqlite3')
        deps.copy_licenses(self.prefix, source, sqlite)
        self.assertEqual((self.prefix / 'share/licenses/sqlite3/PUBLIC-DOMAIN.txt').read_text(), dedication)

    def test_runtime_license_staging_uses_selected_prefix_without_private_receipt(self):
        source = self.root / 'project'
        source.mkdir()
        (source / 'LICENSE').write_text('own MIT')
        (source / 'THIRD_PARTY_NOTICES.md').write_text('own notices')
        license_dir = self.prefix / 'share/licenses'
        # Every dependency — aria included — stages its license through the
        # prefix manifest; no source-tree path participates in staging.
        (license_dir / 'aria').mkdir(parents=True)
        (license_dir / 'aria' / 'LICENSE').write_text('installed aria MIT')
        (license_dir / 'library').mkdir(parents=True)
        (license_dir / 'library' / 'LICENSE').write_text('selected dependency license')
        receipt = self.prefix / 'share/ariaread-deps/manifest.json'
        receipt.parent.mkdir(parents=True)
        receipt.write_text(json.dumps({'context': {'prefix': 'private-build-path'},
            'dependencies': [{'name': 'aria', 'version': '3.1.1', 'license_files': ['LICENSE']},
                             {'name': 'library', 'version': '2.0', 'license_files': ['LICENSE']}]}))
        output = self.root / 'runtime licenses'
        script = Path(__file__).resolve().parents[2] / 'cmake/StageLicenses.cmake'
        command = ['cmake', '-DARIAREAD_SOURCE_DIR=' + str(source),
                   '-DARIAREAD_DEPS_PREFIX=' + str(self.prefix),
                   '-DARIAREAD_LICENSE_OUTPUT=' + str(output), '-P', str(script)]
        subprocess.run(command, check=True, capture_output=True, text=True)
        self.assertEqual((output / 'dependencies/library/LICENSE').read_text(), 'selected dependency license')
        self.assertEqual((output / 'dependencies/aria/LICENSE').read_text(), 'installed aria MIT')
        self.assertFalse((output / 'aria').exists())
        exported = (output / 'components.json').read_text()
        self.assertNotIn('private-build-path', exported)
        self.assertEqual(json.loads(exported)[0]['version'], '3.1.1')
        (license_dir / 'library' / 'NOTICE').write_text('old optional notice')
        subprocess.run(command, check=True, capture_output=True, text=True)
        self.assertTrue((output / 'dependencies/library/NOTICE').exists())
        (license_dir / 'library' / 'NOTICE').unlink()
        subprocess.run(command, check=True, capture_output=True, text=True)
        self.assertFalse((output / 'dependencies/library/NOTICE').exists())
        (license_dir / 'library' / 'LICENSE').unlink()
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Missing or invalid license file', result.stderr)

    def install(self, identity='one', content='1.0', selected=None):
        selected = selected or {'library'}
        with deps.installation(self.prefix, identity, selected, {'lock': identity}) as (done, state):
            if done is not None:
                (self.prefix / 'library.a').write_text(content)
                done.update(selected)
                state['completed'] = sorted(done)
                state['version'] = content

    def test_exact_identity_reuses_verified_installation(self):
        self.install()
        with deps.installation(self.prefix, 'one', {'library'}, {}) as (done, state):
            self.assertIsNone(done)
            self.assertEqual(state['version'], '1.0')
        self.assertFalse(list(self.root.glob('prefix-backup-*')))

    def test_changed_version_rebuilds_and_leaves_no_backup(self):
        self.install()
        with deps.installation(self.prefix, 'two', {'library'}, {'lock': 'two'}) as (done, state):
            self.assertFalse((self.prefix / 'library.a').exists())
            (self.prefix / 'library.a').write_text('2.0')
            state['completed'] = ['library']
        self.assertEqual((self.prefix / 'library.a').read_text(), '2.0')
        self.assertEqual(deps.read_state(self.prefix)['lock'], 'two')
        # A committed transaction deletes its rollback target.
        self.assertEqual(list(self.root.glob('prefix*')), [self.prefix])

    def test_failed_upgrade_restores_original_prefix_and_preserves_failure(self):
        self.install()
        before = (self.prefix / deps.STATE).read_bytes()
        with self.assertRaisesRegex(RuntimeError, 'build failed'):
            with deps.installation(self.prefix, 'two', {'library'}, {}) as (done, state):
                (self.prefix / 'partial').write_text('preserve me')
                raise RuntimeError('build failed')
        self.assertEqual((self.prefix / deps.STATE).read_bytes(), before)
        self.assertEqual((self.prefix / 'library.a').read_text(), '1.0')
        failed, = self.root.glob('prefix-failed-*')
        self.assertTrue((failed / 'partial').exists())
        deps.read_state(self.prefix)

    def test_partial_install_completion_preserves_verified_existing_components(self):
        self.install()
        with deps.installation(self.prefix, 'one', {'library', 'headers'}, {}) as (done, state):
            self.assertEqual(done, {'library'})
            self.assertEqual((self.prefix / 'library.a').read_text(), '1.0')
            (self.prefix / 'header.h').write_text('header')
            state['completed'] = ['library', 'headers']
        self.assertEqual(set(deps.read_state(self.prefix)['completed']), {'library', 'headers'})

    def test_modified_installed_file_is_never_overwritten(self):
        self.install()
        (self.prefix / 'library.a').write_text('local edit')
        with self.assertRaisesRegex(ValueError, 'local modifications'):
            self.install('two', '2.0')
        self.assertEqual((self.prefix / 'library.a').read_text(), 'local edit')
        self.assertEqual(list(self.root.glob('prefix*')), [self.prefix])

    def test_missing_installed_file_is_not_blessed_as_new_version(self):
        self.install()
        (self.prefix / 'library.a').unlink()
        with self.assertRaisesRegex(ValueError, 'missing files'):
            self.install('two', '2.0')
        self.assertEqual(json.loads((self.prefix / deps.STATE).read_text())['version'], '1.0')

    def test_added_local_file_is_preserved(self):
        self.install()
        (self.prefix / 'notes.txt').write_text('local notes')
        with self.assertRaises(ValueError):
            self.install('two')
        self.assertEqual((self.prefix / 'notes.txt').read_text(), 'local notes')

    def test_legacy_artifacts_are_replaced_not_relabelled(self):
        self.prefix.mkdir()
        (self.prefix / 'library.a').write_text('unknown old binary')
        self.install('new', 'new binary')
        self.assertEqual((self.prefix / 'library.a').read_text(), 'new binary')
        self.assertEqual(list(self.root.glob('prefix*')), [self.prefix])  # no leftovers

    def test_stale_sources_are_rejected_before_cmake_can_use_prefix(self):
        self.install()
        state = deps.read_state(self.prefix)
        state['source_dir'] = str(self.root / 'sources')
        state['context'] = {'environment': {'CC': '', 'CXX': ''}}
        state['components'] = {'library': {'identity': 'stale', 'files': ['library.a']}}
        state['files'] = {'library.a': {}}
        deps.write_state(self.prefix, state)
        with patch.object(deps, 'recipe_digest', return_value='recipe'), \
                patch.object(deps.sources, 'identities', return_value={'library': {}}):
            with self.assertRaisesRegex(ValueError, 'changed since the last dependency build'):
                deps.verify_prefix(self.prefix, {'new': 'lock'}, [], {'library'})

    def test_manifest_write_failure_also_restores_old_installation(self):
        self.install()
        original = (self.prefix / deps.STATE).read_bytes()
        with patch.object(deps, 'write_state', side_effect=OSError('disk full')):
            with self.assertRaisesRegex(OSError, 'disk full'):
                self.install('two', '2.0')
        self.assertEqual((self.prefix / deps.STATE).read_bytes(), original)
        self.assertEqual((self.prefix / 'library.a').read_text(), '1.0')

    def test_concurrent_install_is_rejected(self):
        with deps.prefix_lock(self.prefix):
            with self.assertRaisesRegex(ValueError, 'already active'):
                with deps.prefix_lock(self.prefix):
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


if __name__ == '__main__':
    unittest.main()


class SourceTests(unittest.TestCase):
    """Archives, git workspaces, patches and recipe identity."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def dep(self, data, url='https://example.invalid/source.tar.gz'):
        return replace(deps.RECIPES[0], version='1.0', url=url,
                       sha256=deps.hashlib.sha256(data).hexdigest())

    def test_exact_offline_content_cache_is_reused(self):
        data = b'locked archive'
        dep = self.dep(data)
        (self.root / dep.sha256).write_bytes(data)
        # Imported helpers may use a Windows redirected console, unlike the
        # entry point, which explicitly configures UTF-8. Progress must work.
        output = io.BytesIO()
        with io.TextIOWrapper(output, encoding='cp1252') as console, patch.object(deps.sys, 'stdout', console):
            archive = deps.download(self.root, dep, True)
            console.flush()
            self.assertIn(b'Reusing verified archive', output.getvalue())
        self.assertEqual(archive.read_bytes(), data)

    def test_changed_hash_cannot_reuse_same_archive_basename(self):
        old = self.dep(b'old')
        (self.root / old.archive_name).write_bytes(b'old')
        with self.assertRaisesRegex(ValueError, 'Offline cache missing'):
            deps.download(self.root, self.dep(b'new'), True)
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
            deps.download(self.root, dep, True)
        self.assertFalse(outside.exists())
        self.assertTrue(archive.is_symlink())

    def test_unrelated_or_shared_library_does_not_satisfy_static_artifact(self):
        lib = self.root / 'lib'
        lib.mkdir()
        for name in ('libzstd.a', 'libz.dylib', 'zlib.dll'):
            (lib / name).write_text('wrong binary')
        self.assertFalse(deps.artifact_present(self.root, 'lib/libz.a'))
        (lib / 'zlibstatic.lib').write_text('correct static spelling')
        self.assertTrue(deps.artifact_present(self.root, 'lib/libz.a'))

    def test_windows_zlib_static_names_preserve_exports_and_support_findzlib(self):
        for original, compatible in [('zs.lib', 'zlibstatic.lib'), ('libzs.a', 'libz.a')]:
            with self.subTest(original=original):
                prefix = self.root / original
                (prefix / 'lib').mkdir(parents=True)
                (prefix / 'lib' / original).write_bytes(b'compiled static library')
                deps.normalize_zlib_static(prefix)
                self.assertEqual((prefix / 'lib' / original).read_bytes(), b'compiled static library')
                self.assertEqual((prefix / 'lib' / compatible).read_bytes(), b'compiled static library')
                self.assertTrue(deps.artifact_present(prefix, 'lib/libz.a'))
                if deps.shutil.which('cmake'):
                    (prefix / 'include').mkdir()
                    (prefix / 'include/zlib.h').write_text('#define ZLIB_VERSION "1.3.2"\n')
                    script = prefix / 'CMakeLists.txt'
                    script.write_text('cmake_minimum_required(VERSION 3.20)\n'
                                      'set(CMAKE_SYSTEM_NAME Windows)\nproject(FindZlibFixture NONE)\n'
                                      'set(CMAKE_FIND_LIBRARY_PREFIXES "" "lib")\n'
                                      'set(CMAKE_FIND_LIBRARY_SUFFIXES ".a" ".lib")\n'
                                      f'set(ZLIB_ROOT "{prefix.as_posix()}")\n'
                                      'set(ZLIB_USE_STATIC_LIBS ON)\nfind_package(ZLIB REQUIRED)\n'
                                      # New FindZLIB versions know the upstream
                                      # name; older versions require our alias.
                                      f'if(NOT ZLIB_LIBRARY_RELEASE STREQUAL "{(prefix / "lib" / compatible).as_posix()}"\n'
                                      f'   AND NOT ZLIB_LIBRARY_RELEASE STREQUAL "{(prefix / "lib" / original).as_posix()}")\n'
                                      'message(FATAL_ERROR "FindZLIB selected another library: ${ZLIB_LIBRARY_RELEASE}")\nendif()\n')
                    found = subprocess.run(['cmake', '-S', str(prefix), '-B', str(prefix / 'build')], encoding='utf-8', errors='replace',
                                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                    self.assertEqual(found.returncode, 0, found.stdout)
                deps.normalize_zlib_static(prefix)
                (prefix / 'lib' / compatible).write_bytes(b'local modification')
                with self.assertRaisesRegex(ValueError, 'Conflicting zlib'):
                    deps.normalize_zlib_static(prefix)
                self.assertEqual((prefix / 'lib' / compatible).read_bytes(), b'local modification')

    def test_corrupt_content_cache_is_rejected_and_preserved(self):
        dep = self.dep(b'original')
        path = self.root / dep.sha256
        path.write_bytes(b'local edit')
        with self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
            deps.download(self.root, dep, True)
        self.assertEqual(path.read_bytes(), b'local edit')

    def git_fixture(self, name='Mira'):
        source = self.root / (name + '-origin')
        subprocess.run(['git', 'init', '-q', '-b', 'main', str(source)], check=True)
        (source / 'file').write_text('original')
        subprocess.run(['git', '-C', str(source), 'add', 'file'], check=True)
        subprocess.run(['git', '-C', str(source), '-c', 'user.name=Fixture', '-c',
                        'user.email=fixture@example.invalid', 'commit', '-qm', 'fixture'], check=True)
        return source, subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'],
                                                text=True).strip()

    def test_legacy_git_cache_is_copied_only_when_clean_at_the_revision(self):
        source, revision = self.git_fixture()
        legacy = self.root / 'git' / 'Mira' / revision
        legacy.parent.mkdir(parents=True)
        deps.shutil.copytree(source, legacy)
        dep = replace(deps.RECIPES[-1], version='1', revision=revision)
        destination = self.root / 'deps' / 'Mira'
        destination.parent.mkdir(parents=True)
        deps.fetch_git(self.root, destination, dep, True)
        self.assertTrue((destination / 'file').is_file())
        self.assertTrue(legacy.is_dir())  # the legacy cache is never moved
        # A dirty legacy cache is not migrated; offline then fails clearly.
        (legacy / 'local edit').write_text('keep')
        with self.assertRaisesRegex(ValueError, 'Offline source missing'):
            deps.fetch_git(self.root, self.root / 'deps' / 'another', dep, True)

    def test_new_version_never_receives_old_patch(self):
        records = {dep.name: {'version': next(iter(deps.PATCH_VERSIONS.get(dep.name, {'1.0'}))),
                   'requested': 'latest', 'url': 'https://example.invalid/a', 'sha256': 'a' * 64, 'revision': 'b' * 40,
                   'source': {'artifact': 'git' if dep.kind == 'git' else 'archive'}}
                   for dep in deps.RECIPES}
        records['quickjs']['version'] = '2099-01-01'
        with self.assertRaisesRegex(ValueError, 'explicitly reviewed'):
            deps.locked_recipes({'dependencies': records})

    def test_compiler_alias_banner_is_accepted_but_version_change_is_not(self):
        cc = {'path': '/usr/bin/cc', 'arguments': [], 'binary_sha256': 'same', 'version': 'cc (GCC) 16.0\nmore'}
        gcc = {**cc, 'path': '/usr/bin/gcc', 'version': 'gcc (GCC) 16.0\nmore'}
        self.assertTrue(deps.same_compiler(cc, gcc))
        self.assertFalse(deps.same_compiler(cc, {**gcc, 'version': 'gcc (GCC) 17.0\nmore'}))

    def test_abi_flags_and_toolchain_changes_are_rejected(self):
        context = {'environment': {'CXXFLAGS': '-DOLD_ABI'}}
        with patch.dict(deps.os.environ, {}, clear=True):
            with self.assertRaisesRegex(ValueError, 'CXXFLAGS'):
                deps.verify_environment(context)
            deps.verify_environment({'environment': {'CL': '/utf-8'}})
            toolchain = self.root / 'toolchain.cmake'
            toolchain.write_text('old')
            context = {'environment': {'CMAKE_TOOLCHAIN_FILE': str(toolchain),
                       'toolchain_sha256': deps.hashlib.sha256(b'old').hexdigest()}}
            deps.verify_environment(context, str(toolchain))
            toolchain.write_text('new')
            with self.assertRaisesRegex(ValueError, 'toolchain changed'):
                deps.verify_environment(context, str(toolchain))

    def test_msvc_generators_keep_platform_and_configuration_separate(self):
        for generator in ('Visual Studio 18 2026', 'Ninja', 'Ninja Multi-Config', 'NMake Makefiles'):
            with self.subTest(generator=generator), patch.dict(deps.os.environ, {'CMAKE_GENERATOR': generator}, clear=True), \
                    patch.object(deps.sys, 'platform', 'win32'), \
                    patch.object(deps, 'windows_toolchain', return_value='msvc'), \
                    patch.object(deps, 'run') as run:
                deps.build_cmake(self.root, self.root / 'build', self.root / 'prefix', 1, deps.RECIPES[0], [])
                commands = [call.args[0] for call in run.call_args_list]
                self.assertEqual('-A' in commands[0], generator.startswith('Visual Studio'))
                self.assertEqual(commands[1][-2:], ['--config', 'Release'])
                self.assertEqual(commands[2][-2:], ['--config', 'Release'])

    def test_selected_gcc_is_not_overridden_by_msvc_on_path(self):
        with patch.dict(deps.os.environ, {'CC': '/toolchain/gcc'}, clear=True), \
                patch.object(deps.shutil, 'which', return_value='/unrelated/cl'):
            self.assertEqual(deps.windows_toolchain(), 'mingw')
        with patch.dict(deps.os.environ, {'CC': '"/toolchain with spaces/cl.exe"'}, clear=True):
            self.assertEqual(deps.windows_toolchain(), 'msvc')

    def test_openssl_msvc_compiler_path_is_quoted_once_by_upstream(self):
        selected = {'path': r'C:\Program Files\Compiler\cl.exe', 'arguments': ['/DSELECTED=1']}
        original = '"' + selected['path'] + '" /DSELECTED=1'
        with patch.dict(deps.os.environ, {'CC': original, 'CFLAGS': '/O2'}, clear=True), \
                patch.object(deps.sys, 'platform', 'win32'), \
                patch.object(deps, 'windows_toolchain', return_value='msvc'), \
                patch.object(deps.shutil, 'which', return_value='available'), \
                patch.object(deps, 'compiler', return_value=selected) as compiler, \
                patch.object(deps, 'run') as run:
            deps.build_openssl(self.root, self.root / 'prefix', 1)
            compiler.assert_called_once_with(original)
            configure = run.call_args_list[0].args[0]
            self.assertIn('CC=' + selected['path'], configure)
            self.assertIn('CFLAGS=/DSELECTED=1 /O2', configure)
            self.assertEqual(deps.os.environ['CC'], original)
            self.assertEqual(deps.os.environ['CFLAGS'], '/O2')

    def test_compiler_path_with_spaces_is_one_executable(self):
        executable = self.root / 'compiler with spaces'
        executable.write_text('fixture')
        result = subprocess.CompletedProcess([], 0, stdout='compiler 1.0')
        with patch.object(deps.shutil, 'which', return_value=str(executable)), \
                patch.object(deps.subprocess, 'run', return_value=result) as run:
            record = deps.compiler(str(executable))
        self.assertEqual(run.call_args.args[0], [str(executable.resolve()), '--version'])
        self.assertEqual(record['path'], str(executable.resolve()))

    def test_verify_prefix_preserves_separate_compiler_arguments(self):
        executable = self.root / 'compiler with spaces'
        executable.write_text('fixture compiler')
        result = subprocess.CompletedProcess([], 0, stdout='compiler 1.0')
        quote = subprocess.list2cmdline if os.name == 'nt' else shlex.join
        arguments = ['/DTEST=1', 'argument with spaces'] if os.name == 'nt' else ['-arch', 'arm64', '-I/include with spaces']
        with patch.object(deps.shutil, 'which', return_value=str(executable)), \
                patch.object(deps.subprocess, 'run', return_value=result):
            recorded = deps.compiler(quote([str(executable), *arguments]))
            state = {'resolution': deps.fingerprint({}), 'recipe': 'recipe', 'completed': ['library'],
                     'context': {'c': recorded, 'cxx': recorded}, 'source_dir': str(self.root / 'sources'),
                     'components': {'library': {'identity': 'stale', 'files': []}}}
            with patch.object(deps, 'read_state', return_value=state), \
                    patch.object(deps.sources, 'identities', return_value={'library': {}}), \
                    patch.object(deps, 'verify_environment'), patch.object(deps, 'verify_location'), \
                    patch.object(deps, 'recipe_digest', return_value='recipe'), \
                    patch.object(deps, 'reusable_components', return_value={'library'}):
                deps.verify_prefix(self.root, {}, [], {'library'}, str(executable), str(executable),
                                   c_arg1=quote(arguments), cxx_arg1=quote(arguments))
                with self.assertRaisesRegex(ValueError, 'compiler differs'):
                    deps.verify_prefix(self.root, {}, [], {'library'}, str(executable),
                                       c_arg1=quote([*arguments, '-different-abi']))

    def test_native_windows_and_msys_host_spellings_are_equivalent(self):
        context = {'prefix': self.root.as_posix(), 'platform': 'Windows', 'machine': 'AMD64'}
        with patch.object(deps.platform, 'system', return_value='Windows'), \
                patch.object(deps.platform, 'machine', return_value='x86_64'):
            deps.verify_location(context, self.root)
            with self.assertRaisesRegex(ValueError, 'location/platform changed'):
                deps.verify_location(context, self.root / 'another-prefix')
            with self.assertRaisesRegex(ValueError, 'location/platform changed'):
                deps.verify_location({**context, 'machine': 'x86'}, self.root)

    def test_native_msvc_path_alias_and_utf8_flag_preserve_identity(self):
        if os.name != 'nt' or not deps.shutil.which('cl'):
            self.skipTest('Requires native Windows MSVC')
        executable = Path(deps.shutil.which('cl')).resolve()
        with patch.dict(os.environ, {'CL': (os.environ.get('CL', '') + ' /utf-8').strip()}):
            recorded = deps.compiler('cl')
        actual = deps.compiler(executable.as_posix())
        self.assertTrue(deps.same_compiler(recorded, actual),
                        f'recorded={recorded!r}, actual={actual!r}')

    def test_compiler_and_patch_changes_alter_identity(self):
        baseline = {'lock': 'same', 'compiler': 'A', 'patch': 'one'}
        self.assertNotEqual(deps.fingerprint(baseline), deps.fingerprint({**baseline, 'compiler': 'B'}))
        dep = replace(next(dep for dep in deps.RECIPES if dep.name == 'gumbo'), version='1', url='url', sha256='a' * 64)
        with patch.object(deps, 'sha256', return_value='one'):
            old = deps.recipe_digest([dep])
        with patch.object(deps, 'sha256', return_value='two'):
            new = deps.recipe_digest([dep])
        self.assertNotEqual(old, new)

    def test_recipe_identity_tracks_build_helpers_only(self):
        dependency = deps.RECIPES[0]
        expected = deps.recipe_digest([dependency])
        original = deps.inspect.getsource
        for helper in (deps.cmake_arguments, deps.build_environment, deps.prepare_source,
                       deps.windows_toolchain, deps.install_component, deps.build_context):
            with self.subTest(helper=helper.__name__), patch.object(deps.inspect, 'getsource',
                    side_effect=lambda function: original(function) + ('\n# changed' if function is helper else '')):
                self.assertNotEqual(deps.recipe_digest([dependency]), expected)

    def test_source_population_helpers_do_not_rebuild_unchanged_components(self):
        """How a workspace was created is not part of the recipe."""
        dependency = replace(deps.RECIPES[-1], version='1', url='https://example.invalid/m.git', revision='a' * 40)
        expected = deps.recipe_digest([dependency])
        original = deps.inspect.getsource
        for helper in (deps.download, deps.extract, deps.fetch_git):
            with self.subTest(helper=helper.__name__), patch.object(deps.inspect, 'getsource',
                    side_effect=lambda function: original(function) + ('\n# changed' if function is helper else '')):
                self.assertEqual(deps.recipe_digest([dependency]), expected)


class BuilderIntegrationTests(unittest.TestCase):
    """Recipe selection, component identity and the real install transaction."""

    def test_profiles_select_native_windows_tls_without_openssl_or_test_sources(self):
        with patch.object(deps.sys, 'platform', 'win32'):
            recipes = deps.configured_recipes('runtime')
            names = {dep.name for dep in recipes}
            self.assertNotIn('openssl', names)
            self.assertNotIn('doctest', names)
            self.assertIn('aria', names)  # aria is installed like every other dependency
            curl = next(dep for dep in recipes if dep.name == 'curl')
            self.assertIn('-DCURL_USE_SCHANNEL=ON', curl.options)
            self.assertIn('-DCURL_USE_OPENSSL=OFF', curl.options)
            self.assertIn('-DBUILD_LIBCURL_DOCS=OFF', curl.options)
            self.assertEqual(deps.dependency_selection('curl', recipes), {'curl', 'zlib'})
            self.assertIn('openssl', {dep.name for dep in deps.configured_recipes('runtime', 'openssl')})
        with patch.object(deps.sys, 'platform', 'darwin'):
            recipes = deps.configured_recipes('tests')
            self.assertIn('doctest', {dep.name for dep in recipes})
            curl = next(dep for dep in recipes if dep.name == 'curl')
            self.assertIn('-DUSE_APPLE_SECTRUST=ON', curl.options)
            self.assertEqual(deps.dependency_selection('curl', recipes), {'curl', 'zlib', 'openssl'})
            with self.assertRaisesRegex(ValueError, 'requires Windows'):
                deps.configured_recipes('runtime', 'schannel')

    def test_aria_is_an_ordinary_recipe(self):
        aria = next(dep for dep in deps.RECIPES if dep.name == 'aria')
        self.assertEqual(aria.kind, 'git')  # cloned like Mira, installed like curl
        self.assertIn('-DARIA_BUILD_SHARED=ON', aria.options)
        self.assertIn('lib/cmake/aria/ariaConfig.cmake', aria.artifacts)
        records = {dep.name: {'version': next(iter(deps.PATCH_VERSIONS.get(dep.name, {'1.0'}))),
                              'requested': 'latest', 'url': 'https://example.invalid/a',
                              'sha256': 'a' * 64, 'revision': 'b' * 40,
                              'source': {'artifact': 'git' if dep.kind == 'git' else 'archive'}}
                   for dep in deps.RECIPES}
        locked = {dep.name: dep for dep in deps.locked_recipes({'dependencies': records})}
        self.assertEqual(locked['aria'].version, '1.0')

    def test_component_changes_invalidate_transitive_consumers_only(self):
        recipes = [replace(deps.RECIPES[0], name='base', requires=()),
                   replace(deps.RECIPES[0], name='consumer', requires=('base',)),
                   replace(deps.RECIPES[0], name='indirect', requires=('consumer',)),
                   replace(deps.RECIPES[0], name='other', requires=())]
        original = deps.component_identities(recipes, {'compiler': 'one'})
        upgraded = deps.component_identities([replace(recipes[0], version='2'), *recipes[1:]], {'compiler': 'one'})
        self.assertNotEqual(original['base'], upgraded['base'])
        self.assertNotEqual(original['consumer'], upgraded['consumer'])
        self.assertNotEqual(original['indirect'], upgraded['indirect'])
        self.assertEqual(original['other'], upgraded['other'])
        self.assertEqual(deps.dependency_selection('indirect', recipes), {'base', 'consumer', 'indirect'})
        switched = deps.component_identities(recipes, {'compiler': 'two'})
        self.assertTrue(all(original[name] != switched[name] for name in original))

    def test_partial_ownership_receipts_are_never_reused(self):
        previous = {'completed': ['one', 'two'], 'files': {'one.h': {}, 'two.h': {}},
                    'components': {'one': {'identity': '1', 'files': ['one.h']},
                                   'two': {'identity': '2', 'files': ['two.h']}}}
        self.assertEqual(deps.reusable_components(previous, {'one': '1', 'two': '2'}), {'one', 'two'})
        self.assertEqual(deps.reusable_components(previous, {'one': 'new', 'two': '2'}), {'two'})
        previous['components']['one']['files'].append('two.h')
        self.assertEqual(deps.reusable_components(previous, {'one': '1', 'two': '2'}), set())

    def test_component_identity_ignores_compiler_spelling_and_generator_only(self):
        compiler = {'path': '/tools/cc', 'arguments': [], 'binary_sha256': 'binary',
                    'version': 'compiler 1.0'}
        context = {'c': compiler, 'cxx': compiler, 'environment': {
            'CC': '', 'CXX': '', 'CMAKE_GENERATOR': 'Unix Makefiles',
            'CMAKE_GENERATOR_PLATFORM': '', 'CMAKE_GENERATOR_TOOLSET': '',
            'CFLAGS': '', 'CXXFLAGS': '', 'CMAKE_TOOLCHAIN_FILE': ''}}
        recipes = [deps.RECIPES[0]]
        original = deps.component_identities(recipes, context)
        canonical = {**context, 'environment': {**context['environment'],
                     'CC': '/tools/cc', 'CXX': '/tools/cc', 'CMAKE_GENERATOR': 'Ninja'}}
        self.assertEqual(deps.component_identities(recipes, canonical), original)
        self.assertEqual(context['environment']['CC'], '')
        for key in ('CMAKE_GENERATOR_PLATFORM', 'CMAKE_GENERATOR_TOOLSET',
                    'CFLAGS', 'CXXFLAGS', 'CMAKE_TOOLCHAIN_FILE'):
            changed = {**canonical, 'environment': {**canonical['environment'], key: 'different'}}
            with self.subTest(environment=key):
                self.assertNotEqual(deps.component_identities(recipes, changed), original)
        for key, value in (('path', '/other/cc'), ('binary_sha256', 'different'),
                           ('version', 'compiler 2.0'), ('arguments', ['-m32'])):
            changed = {**canonical, 'c': {**compiler, key: value}}
            with self.subTest(compiler=key):
                self.assertNotEqual(deps.component_identities(recipes, changed), original)

    def test_old_component_identity_cannot_short_circuit_transaction_rebuild(self):
        with tempfile.TemporaryDirectory(prefix='ariaread-receipt-migration-') as temporary:
            work = Path(temporary).resolve()
            prefix = work / 'prefix'
            dependency = next(dep for dep in deps.RECIPES if dep.name == 'json')
            compiler = {'path': '/tools/cc', 'arguments': []}
            context = {'c': compiler, 'cxx': compiler,
                       'environment': {'CC': '', 'CXX': '', 'CMAKE_TOOLCHAIN_FILE': ''}}
            resolution = {'fixture': 'unchanged'}
            source_dir = work / 'sources'
            source = source_dir / dependency.name
            source.mkdir(parents=True)
            (source / 'header.h').write_text('fixture')
            metadata = {'resolution': deps.fingerprint(resolution), 'recipe': 'same-recipe',
                        'context': context, 'generator': 'tools/build.py'}
            # Emulate an installation produced before compiler spelling was
            # normalized, using the old whole-prefix transaction identity.
            old_component = deps.fingerprint({'recipe': 'same-recipe',
                                             'context': context, 'requires': {}})
            record = {'name': 'json', 'version': 'fixture'}
            with deps.installation(prefix, deps.fingerprint(metadata), {'json'}, metadata) as (done, state):
                (prefix / 'library.a').write_text('old build')
                state.update(completed=['json'], dependencies=[record], components={
                    'json': {'identity': old_component, 'files': ['library.a']}})

            def install(*args, **kwargs):
                (prefix / 'library.a').write_text('rebuilt')
                return record

            with patch.object(deps, 'resolve'), \
                    patch.object(deps, 'configured_recipes', return_value=[dependency]), \
                    patch.object(deps, 'read_resolved', return_value=resolution), \
                    patch.object(deps, 'locked_recipes', return_value=[dependency]), \
                    patch.object(deps, 'build_environment', return_value=context), \
                    patch.object(deps, 'recipe_digest', return_value='same-recipe'), \
                    patch.object(deps, 'prepare_workspace', return_value=source), \
                    patch.object(deps, 'install_component', side_effect=install) as rebuilt:
                deps.install(work / 'dependencies.json', work, prefix, source_dir,
                             only='json', jobs=1)
                rebuilt.assert_called_once()
                current = deps.component_identities([dependency], context,
                                                    deps.sources.identities(source_dir, [dependency]))
                self.assertEqual(deps.reusable_components(deps.read_state(prefix), current), {'json'})
            self.assertEqual((prefix / 'library.a').read_text(), 'rebuilt')
            self.assertEqual(list(work.glob('prefix*')), [prefix])  # committed: no backups

    def test_windows_compilers_cannot_mix_msvc_and_mingw(self):
        with tempfile.TemporaryDirectory() as temporary, \
                patch.dict(deps.os.environ, {}, clear=True), \
                patch.object(deps.sys, 'platform', 'win32'), \
                patch.object(deps.shutil, 'which', return_value='available'), \
                patch.object(deps, 'build_context', return_value={
                    'c': {'path': '/tools/cl.exe'}, 'cxx': {'path': '/tools/g++.exe'}}):
            with self.assertRaisesRegex(ValueError, 'same Windows toolchain'):
                deps.build_environment(Path(temporary))

    def test_entry_point_emits_utf8_with_legacy_parent_encoding(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            missing = root / '\u4f9d\u8d56-missing.json'
            entry = Path(__file__).resolve().parents[1] / 'build.py'
            result = subprocess.run([sys.executable, str(entry), 'deps-update',
                '--file', str(missing), '--offline'],
                env={**os.environ, 'PYTHONIOENCODING': 'cp1252'}, encoding='utf-8',
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(missing.name, result.stdout)
            self.assertNotIn('UnicodeEncodeError', result.stdout)
            self.assertNotIn('UnicodeDecodeError', result.stdout)


class OfflineTransactionTests(unittest.TestCase):
    """A real offline install/upgrade/rollback, including the doctest UTF-8 fix."""

    def setUp(self):
        if not deps.shutil.which('cmake'):
            self.skipTest('CMake unavailable')
        temporary = tempfile.TemporaryDirectory(prefix='ariaread-deps-fixture-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.work = self.root / 'work'
        self.prefix = self.work / 'prefix'
        (self.work / 'cache').mkdir(parents=True)
        self.sources = self.root / 'sources'
        self.specs = {'schema': 2, 'dependencies': {dep.name: {'provider': 'github',
                     'repo': 'example/fixture', 'artifact': 'git' if dep.kind == 'git' else 'archive'}
                     for dep in deps.RECIPES}}
        self.records = {dep.name: {'version': next(iter(deps.PATCH_VERSIONS.get(dep.name, {'1.0'}))),
                                   'requested': 'latest', 'url': 'https://example.invalid/archive.tar.gz',
                                   'sha256': 'a' * 64, 'revision': 'b' * 40,
                                   'source': self.specs['dependencies'][dep.name]}
                        for dep in deps.RECIPES}
        self.file = self.root / 'dependencies.json'

        # The doctest archive embeds the real upstream discovery module, so the
        # production UTF-8 patch applies and Chinese test names survive CTest.
        data = io.BytesIO()
        with tarfile.open(fileobj=data, mode='w:gz') as package:
            for name, content in {
                    'LICENSE.txt': (DOCTEST_FIXTURE / 'LICENSE.txt').read_text(encoding='utf-8'),
                    'doctest.h': 'untouched test header',
                    'scripts/cmake/doctestAddTests.cmake':
                        (DOCTEST_FIXTURE / 'scripts/cmake/doctestAddTests.cmake').read_text(encoding='utf-8'),
                    'CMakeLists.txt': 'cmake_minimum_required(VERSION 3.20)\n'
                                      'project(DoctestFixture NONE)\n'
                                      'install(FILES doctest.h DESTINATION include/doctest)\n'
                                      'install(FILES scripts/cmake/doctestAddTests.cmake '
                                      'DESTINATION lib/cmake/doctest)\n'}.items():
                encoded = content.encode()
                info = tarfile.TarInfo('source/' + name)
                info.size = len(encoded)
                package.addfile(info, io.BytesIO(encoded))
        digest = deps.hashlib.sha256(data.getvalue()).hexdigest()
        (self.work / 'cache' / digest).write_bytes(data.getvalue())
        self.records['doctest']['sha256'] = digest
        self.archive('1.0')

    def archive(self, version, fail=False):
        contents = {'LICENSE.MIT': 'Fixture license', 'json.hpp': version,
                    'include/nlohmann/notice.hpp':
                        '// SPDX-FileCopyrightText: Fixture Author\n// SPDX-License-Identifier: MIT\n',
                    'CMakeLists.txt': ('cmake_minimum_required(VERSION 3.20)\n'
                                       'project(Fixture C CXX)\n' +
                                       ('message(FATAL_ERROR "fixture build failure")\n' if fail else
                                        'install(FILES json.hpp DESTINATION include/nlohmann)\n' +
                                        ('install(FILES obsolete.hpp DESTINATION include/nlohmann)\n'
                                         if version == '1.0' else '')))}
        if version == '1.0':
            contents['obsolete.hpp'] = 'removed by the upgrade'
        data = io.BytesIO()
        with tarfile.open(fileobj=data, mode='w:gz') as package:
            for name, text in contents.items():
                encoded = text.encode()
                info = tarfile.TarInfo('source/' + name)
                info.size = len(encoded)
                package.addfile(info, io.BytesIO(encoded))
        digest = deps.hashlib.sha256(data.getvalue()).hexdigest()
        (self.work / 'cache' / digest).write_bytes(data.getvalue())
        self.records['json'].update(version=version, sha256=digest)
        entries = {}
        for name, record in self.records.items():
            spec = self.specs['dependencies'][name]
            resolved = {key: value for key, value in record.items() if key != 'source'}
            resolved['request_hash'] = deps.request_hash(spec)
            entries[name] = {**spec, 'resolved': resolved}
        self.file.write_text(json.dumps({'schema': 2, 'dependencies': entries}))

    def invoke(self, *, versions=(), profile='tests', only='json,doctest', verify=False, environment=None):
        output = io.StringIO()
        env = dict(os.environ)
        if environment:
            env.update(environment)
        with patch.dict(os.environ, env, clear=False), redirect_stdout(output):
            if verify:
                deps.verify(self.file, self.work, self.prefix, self.sources,
                            profile=profile, tls_backend='openssl', only=only)
            else:
                deps.install(self.file, self.work, self.prefix, self.sources,
                             profile=profile, tls_backend='openssl', only=only,
                             offline=True, versions=versions, jobs=1)
        return output.getvalue()

    def test_offline_install_upgrade_and_failed_upgrade(self):
        # A runtime profile installs only json; doctest stays absent.
        first = self.invoke(only='json', profile='runtime')
        header = self.prefix / 'include/nlohmann/json.hpp'
        self.assertEqual(header.read_text(), '1.0')
        other = self.prefix / 'include/doctest/doctest.h'
        self.assertFalse(other.exists())
        header_timestamp = header.stat().st_mtime_ns

        # Expanding to the tests profile reuses json byte-for-byte.
        expanded = self.invoke()
        self.assertIn('Reusing verified component: json', expanded)
        self.assertNotIn('json.hpp', expanded)
        self.assertEqual(header.stat().st_mtime_ns, header_timestamp)
        self.assert_doctest_utf8_discovery(self.prefix / 'lib/cmake/doctest/doctestAddTests.cmake')

        # A repeated full install reuses the whole prefix.
        other_timestamp = other.stat().st_mtime_ns
        self.assertTrue((header.parent / 'obsolete.hpp').is_file())
        repeated = self.invoke()
        self.assertIn('Reusing verified dependency prefix', repeated)
        receipt = self.prefix / deps.STATE
        receipt_bytes = receipt.read_bytes()
        self.assertEqual(list(self.work.glob('prefix*')), [self.prefix])  # committed: no backups

        # A canonical CC/CXX spelling is not a rebuild.
        recorded = deps.read_state(self.prefix)['context']
        environment = {'CMAKE_GENERATOR': 'Ninja'}
        for variable, key in [('CC', 'c'), ('CXX', 'cxx')]:
            command = [recorded[key]['path'], *recorded[key]['arguments']]
            environment[variable] = subprocess.list2cmdline(command) if os.name == 'nt' else shlex.join(command)
        canonical = self.invoke(environment=environment)
        self.assertIn('Reusing verified dependency prefix', canonical)
        self.assertEqual(receipt.read_bytes(), receipt_bytes)
        self.assertEqual(list(self.work.glob('prefix*')), [self.prefix])

        # An explicit version replaces only that component.
        self.specs['dependencies']['json']['version'] = '1.0'
        self.records['json']['requested'] = '2.0'
        self.archive('2.0')
        upgraded = self.invoke(versions={'json': '2.0'})
        self.assertIn('Reusing verified component: doctest', upgraded)
        self.assertNotIn('doctest.h', upgraded)
        self.assertEqual(other.read_text(), 'untouched test header')
        self.assertEqual(other.stat().st_mtime_ns, other_timestamp)
        self.assertFalse((header.parent / 'obsolete.hpp').exists())
        self.assertEqual(header.read_text(), '2.0')
        # Component snapshots live in one flat directory per dependency.
        self.assertTrue((self.work / 'runs' / 'json').is_dir())
        self.assertFalse(list((self.work / 'runs').glob('*-*')))
        self.assertEqual(list(self.work.glob('prefix*')), [self.prefix])  # the old prefix was deleted

        # Verification is read-only and does not touch the lock.
        snapshot = self.file.read_bytes()
        self.assertIn('Verified dependency prefix', self.invoke(verify=True))
        self.assertEqual(self.file.read_bytes(), snapshot)
        self.assertIn('Verified dependency prefix',
                      self.invoke(only='json', profile='runtime', verify=True))
        self.assertFalse((self.work / 'runs' / '.staged').exists())

        # A tampered lock is rejected before CMake can use the prefix.
        modified = json.loads(snapshot)
        modified['dependencies']['json']['version'] = '9.0'
        self.file.write_text(json.dumps(modified))
        with self.assertRaisesRegex(deps.DependencyError, 'declaration changed'):
            self.invoke(verify=True)
        self.assertEqual(header.read_text(), '2.0')
        self.file.write_bytes(snapshot)

        # A failed upgrade rolls the prefix back to the last good version.
        self.records['json']['requested'] = '3.0'
        self.archive('3.0', fail=True)
        with self.assertRaisesRegex(subprocess.CalledProcessError, 'returned non-zero exit status'):
            self.invoke(versions={'json': '3.0'})
        self.assertEqual(header.read_text(), '2.0')
        restored = deps.read_state(self.prefix)
        self.assertEqual(next(record['version'] for record in restored['dependencies']
                              if record['name'] == 'json'), '2.0')
        self.assertEqual(other.read_text(), 'untouched test header')

    def assert_doctest_utf8_discovery(self, discovery_script):
        """Run the installed upstream discovery module and its generated tests."""
        with tempfile.TemporaryDirectory(prefix='ariaread-doctest-discovery-') as temporary:
            root = Path(temporary)
            executable = root / 'doctest fixture.py'
            cases = ['中文检索,精确过滤', 'Unicode suite selection']
            label = '书源解析套件'
            executable.write_text(
                'import sys\n'
                f'cases = {cases!r}\n'
                f'label = {label!r}\n'
                'if "--list-test-cases" in sys.argv:\n'
                '    lines = ["[doctest] listing cases", *cases]\n'
                'elif "--list-test-suites" in sys.argv:\n'
                '    assert sys.argv[1] in ["--test-case=" + case for case in cases], sys.argv\n'
                '    lines = ["[doctest] listing suites", label]\n'
                'else:\n'
                '    assert sys.argv[1] in ["--test-case=" + case.replace(",", "\\\\,") for case in cases], sys.argv\n'
                '    lines = []\n'
                'sys.stdout.buffer.write(("\\n".join(lines) + "\\n").encode("utf-8"))\n',
                encoding='utf-8')
            ctest_file = root / 'CTestTestfile.cmake'
            command = ['cmake', '--trace-expand', '--trace-format=json-v1',
                       '--trace-source=' + str(discovery_script),
                       '-DTEST_EXECUTOR=' + sys.executable, '-DTEST_EXECUTABLE=' + str(executable),
                       '-DTEST_WORKING_DIR=' + str(root), '-DTEST_ADD_LABELS=TRUE',
                       '-DTEST_LIST=discovered_tests', '-DCTEST_FILE=' + str(ctest_file),
                       '-P', str(discovery_script)]
            environment = {**os.environ, 'PYTHONIOENCODING': 'cp1252'}
            discovered = subprocess.run(command, env=environment, encoding='utf-8', capture_output=True)
            self.assertEqual(discovered.returncode, 0, discovered.stdout + discovered.stderr)
            # CMake only applies ENCODING on Windows. Check the executed calls
            # on every host as well as round-tripping real UTF-8 child output.
            # The call count itself varies with CMake's internal strategy.
            calls = [event for line in discovered.stderr.splitlines() if line.startswith('{')
                     for event in [json.loads(line)] if event.get('cmd') == 'execute_process']
            self.assertGreaterEqual(len(calls), 1)
            for call in calls:
                arguments = call['args']
                self.assertIn('ENCODING', arguments)
                self.assertEqual(arguments[arguments.index('ENCODING') + 1], 'UTF-8')
            listed = subprocess.run(['ctest', '--show-only=json-v1'], cwd=root,
                                    encoding='utf-8', capture_output=True)
            self.assertEqual(listed.returncode, 0, listed.stdout + listed.stderr)
            tests = json.loads(listed.stdout)['tests']
            self.assertEqual([test['name'] for test in tests], cases)
            for test in tests:
                properties = {entry['name']: entry['value'] for entry in test['properties']}
                self.assertEqual(properties['LABELS'], [label])
                self.assertEqual(test['command'][-1], '--test-case=' + test['name'].replace(',', '\\,'))
            executed = subprocess.run(['ctest', '--output-on-failure'], cwd=root, env=environment,
                                      encoding='utf-8', capture_output=True)
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)
