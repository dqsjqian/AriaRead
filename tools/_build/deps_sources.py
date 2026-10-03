"""Editable dependency sources, independent from disposable build snapshots.

Every dependency lives in a flat, user-owned directory such as deps/curl or
deps/aria. The lock file only chooses what a missing directory starts from.
Afterwards the directory's actual contents are the build input: users may
edit files, git pull or switch branches and take responsibility for that.

deps/.ariaread-sources/<name>.json remembers which locked selection created a
directory and its untouched identity. A later lock change replaces only an
untouched directory (the previous tree is a rollback target and is deleted
once the swap succeeds). Changed directories are kept and reported; the new
selection is NOT applied.
"""
from __future__ import annotations

import hashlib
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import uuid
from contextlib import contextmanager
from pathlib import Path


def fingerprint(value):
    """A stable JSON fingerprint; deps.py shares the exact same encoding."""
    return hashlib.sha256(json.dumps(value, sort_keys=True,
                                     separators=(',', ':')).encode()).hexdigest()


@contextmanager
def lock_output(path):
    """OS file lock; identical semantics to the lock used for dependencies.json."""
    directory = Path(tempfile.gettempdir()) / "aria-dependency-locks"
    directory.mkdir(exist_ok=True)
    key = hashlib.sha256(str(Path(path).resolve()).encode()).hexdigest()
    with (directory / key).open("a+b") as stream:
        if os.name == "nt":
            import msvcrt
            stream.seek(0, os.SEEK_END)
            if stream.tell() == 0:
                stream.write(b"\0")
                stream.flush()
            stream.seek(0)
            msvcrt.locking(stream.fileno(), msvcrt.LK_LOCK, 1)
            try:
                yield
            finally:
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl
            fcntl.flock(stream, fcntl.LOCK_EX)
            try:
                yield
            finally:
                fcntl.flock(stream, fcntl.LOCK_UN)


STATE_DIRECTORY = '.ariaread-sources'
# Written by the former Aria fetcher; it records provenance, not source input.
LEGACY_MARKERS = ('.pinned-aria-sha',)


def requested(dependency):
    """The locked selection that a workspace was created from."""
    return {name: getattr(dependency, name) for name in ('version', 'url', 'sha256', 'revision', 'kind')}


def git_output(source, *arguments, raw=False):
    # --no-optional-locks keeps inspection read-only: status must not refresh
    # the user's index while a check, an IDE or another build is running.
    command = ['git', '--no-optional-locks', '-C', str(source), *map(str, arguments)]
    result = subprocess.run(command, capture_output=True)
    if result.returncode:
        message = result.stderr.decode('utf-8', 'replace').strip()
        raise ValueError(f'git {" ".join(map(str, arguments))} failed in {source}: {message}')
    return result.stdout if raw else result.stdout.decode('utf-8', 'strict').strip()


def is_git(source):
    return (source / '.git').exists()


def files(source):
    """Track user inputs, including untracked files, but not Git-ignored output."""
    if is_git(source):
        names = git_output(source, 'ls-files', '-z', '--cached', '--others', '--exclude-standard', raw=True)
        paths = {source / os.fsdecode(name) for name in names.split(b'\0') if name}
    else:
        paths = set(source.iterdir())
    result = set()
    pending = list(paths)
    while pending:
        path = pending.pop()
        if path.name == '.git' or path.relative_to(source).as_posix() in LEGACY_MARKERS:
            continue
        if path.is_symlink() or path.is_file():
            result.add(path)
        elif path.is_dir():
            pending.extend(path.iterdir())
        elif path.exists():
            raise ValueError(f'Unsupported dependency source entry: {path}')
        # Deleted tracked files are absent from the tree fingerprint.
    return sorted(result)


def inventory(source):
    if not source.is_dir():
        raise ValueError(f'Dependency source missing: {source}; run python tools/build.py deps')
    root = source.resolve()
    entries = {}
    for path in files(source):
        relative = path.relative_to(source).as_posix()
        if path.is_symlink():
            target = path.resolve()
            if root not in target.parents or not target.exists():
                raise ValueError(f'Dangling/external dependency source symlink: {path}')
            entries[relative] = {'link': os.readlink(path)}
        else:
            digest = hashlib.sha256()
            with path.open('rb') as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                    digest.update(chunk)
            entries[relative] = {'sha256': digest.hexdigest(),
                                 'executable': bool(path.stat().st_mode & 0o111)}
    return entries


def identity(source):
    """content drives build caching; revision is informational for reports."""
    return {'content': fingerprint(inventory(source)),
            'revision': git_output(source, 'rev-parse', 'HEAD') if is_git(source) else ''}


def identities(source_root, dependencies):
    return {dep.name: identity(source_root / dep.name)
            for dep in dependencies if (source_root / dep.name).exists()}


def git_changes(source):
    """Tracked edits and untracked inputs; ignored build output is not a change."""
    return git_output(source, 'status', '--porcelain', '--untracked-files=all', '--', '.',
                      *(f':(exclude){marker}' for marker in LEGACY_MARKERS))


def linked(source):
    """Symlinks, linked worktrees and submodules point at Git data elsewhere."""
    return source.is_symlink() or (source / '.git').is_file()


def clean_git(source):
    return is_git(source) and not linked(source) and not git_changes(source)


def at_revision(source, revision):
    return bool(revision) and clean_git(source) and git_output(source, 'rev-parse', 'HEAD') == revision


def record_path(source_root, name):
    return source_root / STATE_DIRECTORY / (name + '.json')


def read_record(source_root, name):
    path = record_path(source_root, name)
    if path.is_symlink():
        raise ValueError(f'Refusing symlink source workspace metadata: {path}')
    if not path.is_file():
        return None
    value = json.loads(path.read_text(encoding='utf-8'))
    if not isinstance(value, dict) or value.get('schema') != 1:
        return None  # Unknown metadata never authorizes replacing a directory.
    return value


def write_record(source_root, name, request, pristine):
    path = record_path(source_root, name)
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix='.source-', dir=path.parent)
    try:
        with os.fdopen(descriptor, 'w', encoding='utf-8') as stream:
            json.dump({'schema': 1, 'requested': request, 'pristine': pristine},
                      stream, indent=2, ensure_ascii=False, sort_keys=True)
            stream.write('\n')
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def describe(request):
    return request.get('version') or request.get('revision') or 'the locked source'


def status(source_root, dependency, actual=None):
    """Read-only: missing, unrecorded, untouched, modified or not-applied."""
    source = source_root / dependency.name
    if not (source.exists() or source.is_symlink()):
        return 'missing'
    record = read_record(source_root, dependency.name)
    if record is None:
        return 'unrecorded'
    if record['requested'] != requested(dependency):
        return 'not-applied'
    return 'untouched' if record.get('pristine') == (actual or identity(source)) else 'modified'


def report(source_root, dependency, actual=None):
    """Print a deps-check line for any state other than an untouched source."""
    state = status(source_root, dependency, actual)
    source = source_root / dependency.name
    if state == 'modified':
        print(f'{dependency.name}: building local source changes in {source}', flush=True)
    elif state == 'not-applied':
        warn(f'{dependency.name}: the selected {describe(requested(dependency))} is NOT applied; '
             f'{source} keeps local changes')
    elif state == 'unrecorded':
        print(f'{dependency.name}: using an existing source directory {source}', flush=True)
    return state


def warn(message):
    print('warning: ' + message, file=sys.stderr, flush=True)


def head(value):
    return value['revision'] or 'archive contents'


def replace_directory(source, candidate, state_root, scratch):
    """Swap a fully prepared candidate in; restore the previous tree on failure.

    The previous tree is a rollback target only: a successful swap deletes it,
    so workspaces never accumulate backups.
    """
    backup = None
    if source.exists() or source.is_symlink():
        backups = state_root / 'backups'
        backups.mkdir(exist_ok=True)
        backup = backups / (source.name + '-' + uuid.uuid4().hex[:12])
        source.rename(backup)
    try:
        candidate.rename(source)
    except BaseException:
        if source.exists():
            source.rename(scratch / 'failed-source')
        if backup:
            backup.rename(source)
        raise
    if backup:
        shutil.rmtree(backup, ignore_errors=True)


def ensure(source_root, dependency, populate, *, explicit=False):
    """Prepare one editable source and return its path.

    populate(destination) creates the locked selection in a new directory.
    Ordinary builds never fail or discard work because of local changes: they
    build the actual contents and warn when a new selection was not applied.
    explicit=True is an update requested now; it may replace a clean ordinary
    Git clone (keeping a backup) and fails instead of skipping local work.
    """
    source = source_root / dependency.name
    state_root = source_root / STATE_DIRECTORY
    state_root.mkdir(parents=True, exist_ok=True)
    request = requested(dependency)
    with lock_output(source):
        previous = read_record(source_root, dependency.name)
        present = source.exists() or source.is_symlink()
        actual = None
        if present:
            if not source.is_dir():
                raise ValueError(f'Dependency source is not a directory; preserved: {source}')
            actual = identity(source)
            if previous and previous['requested'] == request:
                if previous.get('pristine') != actual:
                    print(f'Using local dependency source changes: {source} (HEAD {head(actual)})', flush=True)
                return source
            if at_revision(source, dependency.revision):
                # Already the selected commit, e.g. after a manual checkout.
                write_record(source_root, dependency.name, request, actual)
                print(f'Using existing dependency source: {source} (already at {dependency.revision})', flush=True)
                return source
            untouched = bool(previous) and previous.get('pristine') == actual and not linked(source)
            if previous is None and not explicit:
                # Adopt directories created elsewhere: an older layout, a manual
                # clone or a restored cache. Unknown contents stay user-owned.
                write_record(source_root, dependency.name, request, None)
                print(f'Using existing dependency source: {source} (HEAD {head(actual)})', flush=True)
                return source
            if explicit and not (untouched or clean_git(source)):
                raise ValueError(f'Requested {dependency.name} update to {describe(request)} was NOT applied: '
                                 f'{source} has local changes, unknown archive contents or is a linked '
                                 f'workspace (HEAD {head(actual)}). Sources were preserved; commit or move '
                                 'them aside, or update that directory manually.')
            if not explicit and not untouched:
                warn(f'{dependency.name} selection changed to {describe(request)}, but {source} has local '
                     f'changes or is a linked workspace; the new selection was NOT applied. Building the '
                     f'current contents (HEAD {head(actual)}). Move the directory aside to fetch it.')
                return source
        # Populate fully before moving any existing sources out of the way.
        with tempfile.TemporaryDirectory(prefix=dependency.name + '-prepare-', dir=state_root) as temporary:
            scratch = Path(temporary)
            candidate = scratch / 'source'
            populate(candidate)
            pristine = identity(candidate)
            if is_git(candidate) and not at_revision(candidate, dependency.revision):
                pristine = None  # A migrated legacy tree with edits stays user-owned.
            if present and identity(source) != actual:
                raise ValueError(f'Dependency source changed during update; preserved: {source}')
            replace_directory(source, candidate, state_root, scratch)
            write_record(source_root, dependency.name, request, pristine)
        return source


def clone(destination, url, revision, *, branch='main'):
    """Clone one revision on a normal tracking branch, so git pull works later."""
    def git(*arguments):
        command = ['git', *map(str, arguments)]
        print('+ git ' + ' '.join(map(str, arguments)), flush=True)
        result = subprocess.run(command, capture_output=True)
        if result.returncode:
            message = result.stderr.decode('utf-8', 'replace').strip()
            raise ValueError(f'git {arguments[2] if arguments[0] == "-C" else arguments[0]} failed: {message}')
    git('init', '-q', destination)
    git('-C', destination, 'remote', 'add', 'origin', url)
    try:
        git('-C', destination, 'fetch', '-q', '--depth', '1', 'origin', revision)
    except ValueError:
        # Some servers and local repositories refuse fetching a bare commit ID.
        git('-C', destination, 'fetch', '-q', '--tags', 'origin')
    git('-C', destination, 'checkout', '-q', '-B', branch, revision)
    git('-C', destination, 'config', f'branch.{branch}.remote', 'origin')
    git('-C', destination, 'config', f'branch.{branch}.merge', f'refs/heads/{branch}')
    actual = git_output(destination, 'rev-parse', 'HEAD')
    if actual != revision:
        raise ValueError(f'Git checkout revision mismatch: expected {revision}, got {actual}')


def copy_legacy_clone(legacy, destination, revision, *, allow_changes=False):
    """Copy an old checkout without moving it: older build trees may still use it.

    A clean tree at the selected revision is copied. allow_changes also copies
    a tree with edits, so that unfinished work continues; it stays user-owned.
    """
    if legacy.is_symlink() or not (legacy / '.git').is_dir():
        return False
    changes = git_changes(legacy)
    if not (changes and allow_changes) and (changes or git_output(legacy, 'rev-parse', 'HEAD') != revision):
        return False
    shutil.copytree(legacy, destination, symlinks=True)
    for marker in LEGACY_MARKERS:
        (destination / marker).unlink(missing_ok=True)
    # The copy has new file stat data; refresh only this new private index.
    subprocess.run(['git', '-C', str(destination), 'update-index', '-q', '--refresh'], capture_output=True)
    print(f'Copied legacy dependency source {legacy} -> {destination}; original retained', flush=True)
    return True


def snapshot(source, destination, expected=None):
    """Copy the exact identity's files, leaving editable sources unpatched."""
    before = identity(source)
    if expected is not None and before != expected:
        raise ValueError(f'Dependency source changed before snapshot; retry build: {source}')
    entries = inventory(source)
    destination.mkdir(parents=True)
    for relative, entry in entries.items():
        origin, target = source / relative, destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if 'link' in entry:
            target.symlink_to(entry['link'], target_is_directory=origin.is_dir())
        else:
            shutil.copy2(origin, target)
    if (fingerprint(inventory(destination)) != before['content']
            or identity(source) != before):
        raise ValueError(f'Dependency source changed during snapshot; retry build: {source}')
    return before
