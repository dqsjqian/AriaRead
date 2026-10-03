"""Regressions for editable dependency source workspaces (deps/<name>/)."""
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import deps_sources as sources


def dependency(name='library', version='1.0', kind='archive', revision='', url='https://example.invalid/x'):
    return SimpleNamespace(name=name, version=version, url=url, sha256='a' * 64,
                           revision=revision, kind=kind)


def git(*args, cwd):
    result = subprocess.run(['git', '-C', str(cwd), *map(str, args)],
                            capture_output=True, text=True)
    if result.returncode:
        raise AssertionError(f'git {args} failed in {cwd}: {result.stderr}')
    return result.stdout.strip()


class GitFixture(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='deps-sources-fixture-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.origin = self.root / 'origin'
        git('init', '-q', '-b', 'main', cwd=self.__empty(self.origin))
        git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
            'commit', '-q', '--allow-empty', '-m', 'first', cwd=self.origin)
        self.first = git('rev-parse', 'HEAD', cwd=self.origin)
        (self.origin / 'library.txt').write_text('first\n')
        git('add', 'library.txt', cwd=self.origin)
        git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
            'commit', '-q', '-m', 'second', cwd=self.origin)
        self.second = git('rev-parse', 'HEAD', cwd=self.origin)
        (self.origin / 'library.txt').write_text('third\n')
        git('add', 'library.txt', cwd=self.origin)
        git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
            'commit', '-q', '-m', 'third', cwd=self.origin)
        self.third = git('rev-parse', 'HEAD', cwd=self.origin)
        self.deps = self.root / 'deps 目录'  # exercises non-ASCII and spaces
        self.deps.mkdir()

    def __empty(self, path):
        path.mkdir(parents=True)
        return path

    def clone_into(self, destination, revision):
        sources.clone(destination, str(self.origin), revision)
        return destination


class WorkspaceAdoptionTests(GitFixture):
    def test_missing_workspace_is_cloned_from_the_lock(self):
        dep = dependency(kind='git', revision=self.second)
        path = sources.ensure(self.deps, dep, lambda dest: self.clone_into(dest, self.second))
        self.assertEqual(git('rev-parse', 'HEAD', cwd=path), self.second)
        self.assertEqual(git('branch', '--show-current', cwd=path), 'main')
        record = json.loads((self.deps / '.ariaread-sources/library.json').read_text())
        self.assertEqual(record['requested']['revision'], self.second)
        self.assertEqual(record['pristine']['revision'], self.second)

    def test_cloned_workspace_tracks_origin_and_pulls(self):
        dep = dependency(kind='git', revision=self.second)
        path = sources.ensure(self.deps, dep, lambda dest: self.clone_into(dest, self.second))
        git('pull', '--ff-only', cwd=path)  # the user can update it themselves
        self.assertEqual(git('rev-parse', 'HEAD', cwd=path), self.third)

    def test_unrecorded_clean_clone_at_the_revision_is_adopted_pristine(self):
        dep = dependency(kind='git', revision=self.second)
        self.clone_into(self.deps / 'library', self.second)
        output = io.StringIO()
        with redirect_stdout(output):
            sources.ensure(self.deps, dep, lambda dest: self.fail('must not repopulate'))
        self.assertIn('Using existing dependency source', output.getvalue())
        record = json.loads((self.deps / '.ariaread-sources/library.json').read_text())
        self.assertIsNotNone(record['pristine'])

    def test_unrecorded_directory_is_adopted_as_user_owned(self):
        dep = dependency(kind='git', revision=self.second)
        work = self.deps / 'library'
        self.clone_into(work, self.first)
        (work / 'local edit.txt').write_text('keep me')
        sources.ensure(self.deps, dep, lambda dest: self.fail('must not repopulate'))
        self.assertEqual((work / 'local edit.txt').read_text(), 'keep me')
        record = json.loads((self.deps / '.ariaread-sources/library.json').read_text())
        self.assertIsNone(record['pristine'])

    def test_unrecorded_plain_directory_is_adopted_without_git(self):
        dep = dependency(kind='git', revision=self.second)
        work = self.deps / 'library'
        work.mkdir()
        (work / 'CMakeLists.txt').write_text('# manual tree\n')
        sources.ensure(self.deps, dep, lambda dest: self.fail('must not repopulate'))
        self.assertEqual((work / 'CMakeLists.txt').read_text(), '# manual tree\n')

    def test_same_selection_keeps_local_changes(self):
        dep = dependency(kind='git', revision=self.second)
        path = sources.ensure(self.deps, dep, lambda dest: self.clone_into(dest, self.second))
        (path / 'library.txt').write_text('local work\n')
        output = io.StringIO()
        with redirect_stdout(output):
            sources.ensure(self.deps, dep, lambda dest: self.fail('must not repopulate'))
        self.assertIn('Using local dependency source changes', output.getvalue())
        self.assertEqual((path / 'library.txt').read_text(), 'local work\n')

    def test_lock_change_replaces_only_untouched_workspaces(self):
        first = dependency(kind='git', revision=self.second)
        path = sources.ensure(self.deps, first, lambda dest: self.clone_into(dest, self.second))
        second = dependency(kind='git', revision=self.third)
        output = io.StringIO()
        with redirect_stdout(output):
            sources.ensure(self.deps, second, lambda dest: self.clone_into(dest, self.third))
        self.assertEqual(git('rev-parse', 'HEAD', cwd=path), self.third)
        backups = list((self.deps / '.ariaread-sources/backups').glob('library-*'))
        self.assertEqual(len(backups), 1)
        self.assertEqual(git('rev-parse', 'HEAD', cwd=backups[0]), self.second)
        self.assertIn('Preserved previous dependency source', output.getvalue())

    def test_lock_change_with_local_work_keeps_the_workspace_and_warns(self):
        first = dependency(kind='git', revision=self.second)
        path = sources.ensure(self.deps, first, lambda dest: self.clone_into(dest, self.second))
        (path / 'library.txt').write_text('local work\n')
        second = dependency(kind='git', revision=self.third)
        errors = io.StringIO()
        with redirect_stderr(errors), patch.object(sources, 'warn') as warned:
            sources.ensure(self.deps, second, lambda dest: self.fail('must not repopulate'))
        warned.assert_called_once()
        self.assertIn('NOT applied', warned.call_args.args[0])
        self.assertEqual((path / 'library.txt').read_text(), 'local work\n')
        record = json.loads((self.deps / '.ariaread-sources/library.json').read_text())
        self.assertEqual(record['requested']['revision'], self.second)  # still pending

    def test_explicit_update_replaces_a_clean_clone(self):
        first = dependency(kind='git', revision=self.second)
        path = sources.ensure(self.deps, first, lambda dest: self.clone_into(dest, self.second))
        second = dependency(kind='git', revision=self.third)
        sources.ensure(self.deps, second, lambda dest: self.clone_into(dest, self.third), explicit=True)
        self.assertEqual(git('rev-parse', 'HEAD', cwd=path), self.third)

    def test_explicit_update_refuses_local_work(self):
        first = dependency(kind='git', revision=self.second)
        path = sources.ensure(self.deps, first, lambda dest: self.clone_into(dest, self.second))
        (path / 'library.txt').write_text('local work\n')
        second = dependency(kind='git', revision=self.third)
        with self.assertRaisesRegex(ValueError, 'NOT applied'):
            sources.ensure(self.deps, second, lambda dest: self.fail('must not repopulate'), explicit=True)
        self.assertEqual((path / 'library.txt').read_text(), 'local work\n')

    def test_linked_worktrees_and_symlinks_are_never_relocated(self):
        dep = dependency(kind='git', revision=self.second)
        linked = self.root / 'linked'
        git('worktree', 'add', '--detach', linked, self.first, cwd=self.origin)
        (linked / 'library.txt').write_text('linked work\n')
        try:
            (self.deps / 'library').symlink_to(linked)
        except OSError:
            self.skipTest('symlink privilege unavailable')
        upgraded = dependency(kind='git', revision=self.third)
        with self.assertRaisesRegex(ValueError, 'NOT applied'):
            sources.ensure(self.deps, upgraded, lambda dest: self.fail('must not repopulate'), explicit=True)
        self.assertEqual((linked / 'library.txt').read_text(), 'linked work\n')
        self.assertTrue((self.deps / 'library').is_symlink())

    def test_source_changing_during_population_is_detected(self):
        dep = dependency(kind='git', revision=self.second)
        path = sources.ensure(self.deps, dep, lambda dest: self.clone_into(dest, self.second))

        def populate(destination):
            self.clone_into(destination, self.third)
            (path / 'library.txt').write_text('changed while preparing\n')  # concurrent edit

        upgraded = dependency(kind='git', revision=self.third)
        with self.assertRaisesRegex(ValueError, 'changed during update'):
            sources.ensure(self.deps, upgraded, populate)
        self.assertEqual((path / 'library.txt').read_text(), 'changed while preparing\n')

    def test_failed_replacement_restores_the_previous_workspace(self):
        first = dependency(kind='git', revision=self.second)
        path = sources.ensure(self.deps, first, lambda dest: self.clone_into(dest, self.second))
        original = path.stat().st_ino

        def populate(destination):
            self.clone_into(destination, self.third)
            raise OSError('simulated install failure')

        upgraded = dependency(kind='git', revision=self.third)
        with self.assertRaisesRegex(OSError, 'simulated install failure'):
            sources.ensure(self.deps, upgraded, populate)
        self.assertEqual(git('rev-parse', 'HEAD', cwd=path), self.second)
        self.assertEqual(path.stat().st_ino, original)


class IdentityTests(GitFixture):
    def setUp(self):
        super().setUp()
        self.work = self.deps / 'library'
        self.clone_into(self.work, self.second)
        (self.work / 'ignored build').mkdir()
        (self.work / 'ignored build' / 'output.txt').write_text('build product')
        (self.work / '.git' / 'info' / 'exclude').write_text('ignored build/\n')
        self.dep = dependency(kind='git', revision=self.second)

    def test_ignored_files_are_not_part_of_the_identity(self):
        before = sources.identity(self.work)
        (self.work / 'ignored build' / 'output.txt').write_text('rebuild artifact')
        self.assertEqual(sources.identity(self.work), before)

    def test_untracked_and_tracked_changes_are_part_of_the_identity(self):
        before = sources.identity(self.work)
        (self.work / 'untracked.txt').write_text('user input')
        self.assertNotEqual(sources.identity(self.work), before)
        (self.work / 'untracked.txt').unlink()
        self.assertEqual(sources.identity(self.work), before)
        (self.work / '中文 注释.txt').write_text('非 ASCII 输入')
        after = sources.identity(self.work)
        self.assertNotEqual(before, after)
        (self.work / '中文 注释.txt').unlink()
        self.assertEqual(sources.identity(self.work), before)

    def test_deleted_tracked_files_change_the_identity(self):
        before = sources.identity(self.work)
        (self.work / 'library.txt').unlink()
        self.assertNotEqual(sources.identity(self.work), before)

    def test_status_reports_the_five_workspace_states(self):
        self.assertEqual(sources.status(self.deps, self.dep), 'unrecorded')
        sources.ensure(self.deps, self.dep, lambda dest: self.fail('adopted'))
        self.assertEqual(sources.status(self.deps, self.dep), 'untouched')
        (self.work / 'library.txt').write_text('local work\n')
        self.assertEqual(sources.status(self.deps, self.dep), 'modified')
        upgraded = dependency(kind='git', revision=self.third)
        with patch.object(sources, 'warn'):
            sources.ensure(self.deps, upgraded, lambda dest: self.fail('kept'))
        self.assertEqual(sources.status(self.deps, upgraded), 'not-applied')
        self.assertEqual(sources.status(self.deps, self.dep), 'modified')
        (self.deps / 'library').rename(self.root / 'moved aside')
        self.assertEqual(sources.status(self.deps, self.dep), 'missing')

    def test_report_prints_without_raising_for_every_state(self):
        sources.ensure(self.deps, self.dep, lambda dest: self.fail('adopted'))
        output = io.StringIO()
        with redirect_stdout(output):
            state = sources.report(self.deps, self.dep)
        self.assertEqual(state, 'untouched')
        self.assertEqual(output.getvalue(), '')  # an untouched source is silent

    def test_snapshot_copies_exactly_the_current_files(self):
        (self.work / 'extra.c').write_text('user file')
        snapshot_root = self.root / 'runs snapshot'
        before = sources.snapshot(self.work, snapshot_root / 'source')
        self.assertEqual((snapshot_root / 'source' / 'extra.c').read_text(), 'user file')
        self.assertFalse((snapshot_root / 'source' / 'ignored build').exists())
        self.assertEqual(sources.identity(snapshot_root / 'source')['content'], before['content'])
        self.assertFalse((snapshot_root / 'source' / '.git').exists())  # build inputs only

    def test_snapshot_rejects_a_changed_source(self):
        expected = sources.identity(self.work)
        (self.work / 'library.txt').write_text('raced edit\n')
        with self.assertRaisesRegex(ValueError, 'changed before snapshot'):
            sources.snapshot(self.work, self.root / 'runs snapshot' / 'source', expected)

    def test_legacy_marker_files_do_not_change_workspace_semantics(self):
        (self.work / '.pinned-aria-sha').write_text('historical marker')
        before = sources.identity(self.work)
        (self.work / '.pinned-aria-sha').write_text('changed marker')
        self.assertEqual(sources.identity(self.work), before)


class CloneTests(GitFixture):
    def test_clone_rejects_a_revision_mismatch(self):
        with self.assertRaisesRegex(ValueError, 'failed'):
            sources.clone(self.root / 'workspace', str(self.origin), 'f' * 40)
        self.assertFalse((self.root / 'workspace' / 'library.txt').exists())

    def test_clone_of_a_bare_commit_id_falls_back_to_a_full_fetch(self):
        # A local path origin cannot serve `fetch <sha>` in every version.
        path = self.root / 'workspace'
        sources.clone(path, str(self.origin), self.first)
        self.assertEqual(git('rev-parse', 'HEAD', cwd=path), self.first)

    def test_copy_legacy_clone_only_accepts_a_clean_tree_at_the_revision(self):
        legacy = self.root / 'legacy'
        self.clone_into(legacy, self.second)
        destination = self.root / 'fresh'
        self.assertTrue(sources.copy_legacy_clone(legacy, destination, self.second))
        self.assertEqual((destination / 'library.txt').read_text(), 'first\n')
        self.assertTrue(legacy.exists())  # the legacy tree is preserved
        # A clean tree at another revision is not this selection.
        other = self.root / 'other'
        self.assertFalse(sources.copy_legacy_clone(legacy, other, self.third))
        self.assertFalse(other.exists())
        # A dirty tree is only copied when changes are explicitly allowed.
        (legacy / 'uncommitted.txt').write_text('unfinished work')
        self.assertFalse(sources.copy_legacy_clone(legacy, other, self.second))
        self.assertTrue(sources.copy_legacy_clone(legacy, other, self.second, allow_changes=True))
        self.assertEqual((other / 'uncommitted.txt').read_text(), 'unfinished work')


if __name__ == '__main__':
    unittest.main(verbosity=2)
