"""Verified, transactional installation cache for AriaRead dependencies."""
from __future__ import annotations

import hashlib
import json
import os
import platform
import re
import shlex
import shutil
import subprocess
import tempfile
import uuid
from contextlib import contextmanager
from pathlib import Path

STATE = Path('share/ariaread-deps/manifest.json')


def fingerprint(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def inventory(root):
    result = {}
    for path in sorted(root.rglob('*')):
        relative = path.relative_to(root).as_posix()
        if relative == STATE.as_posix():
            continue
        if path.is_symlink():
            target = path.resolve()
            if root.resolve() not in target.parents or not target.exists():
                raise ValueError(f'Dependency prefix contains a dangling/external symlink: {path}')
            result[relative] = {'link': os.readlink(path)}
        elif path.is_file():
            digest = hashlib.sha256()
            with path.open('rb') as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                    digest.update(chunk)
            result[relative] = {'sha256': digest.hexdigest(), 'executable': bool(path.stat().st_mode & 0o111)}
        elif not path.is_dir():
            raise ValueError(f'Unsupported file in dependency prefix: {path}')
    return result


def compiler(command):
    parts = [command] if Path(command).is_file() else shlex.split(command, posix=os.name != 'nt')
    if os.name == 'nt':
        parts = [part.strip(chr(34)) for part in parts]
    executable = shutil.which(parts[0]) if parts else None
    if not executable:
        raise ValueError(f'Compiler is unavailable: {command}')
    argument = '/Bv' if Path(executable).stem.lower() == 'cl' else '--version'
    result = subprocess.run([executable, *parts[1:], argument], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return {'path': str(Path(executable).resolve()),
            'binary_sha256': hashlib.sha256(Path(executable).read_bytes()).hexdigest(),
            'arguments': parts[1:],
            'version': result.stdout.strip()}


def same_compiler(recorded, actual):
    # GCC aliases cc/gcc and c++/g++ can be identical binaries with different
    # argv[0] banners. Keep the version details while normalizing that one token.
    def identity(record):
        value = {key: item for key, item in record.items() if key != 'path'}
        banner = value['version'].split(' ', 1)
        value['version'] = banner[1] if len(banner) == 2 else banner[0]
        return value
    return identity(recorded) == identity(actual)


def build_context(prefix, c=None, cxx=None):
    default_c = 'cl' if shutil.which('cl') else ('cc' if shutil.which('cc') else 'gcc')
    default_cxx = 'cl' if shutil.which('cl') else ('c++' if shutil.which('c++') else 'g++')
    environment = {key: os.environ.get(key, '') for key in (
        'CC', 'CXX', 'CL', '_CL_', 'CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS',
        'CMAKE_GENERATOR', 'CMAKE_GENERATOR_PLATFORM', 'CMAKE_GENERATOR_TOOLSET',
        'CMAKE_TOOLCHAIN_FILE', 'SDKROOT', 'MACOSX_DEPLOYMENT_TARGET',
        'INCLUDE', 'LIB', 'LIBPATH', 'WindowsSDKVersion', 'VCToolsVersion')}
    toolchain = environment['CMAKE_TOOLCHAIN_FILE']
    if toolchain:
        environment['toolchain_sha256'] = hashlib.sha256(Path(toolchain).read_bytes()).hexdigest()
    return {'prefix': str(prefix), 'platform': platform.system(),
            'machine': platform.machine(), 'configuration': 'Release',
            'c': compiler(c or os.environ.get('CC') or default_c),
            'cxx': compiler(cxx or os.environ.get('CXX') or default_cxx),
            'environment': environment}


def verify_environment(context, toolchain=None):
    recorded = context['environment']
    # CC/CXX are compared through the selected compiler, and a generator may
    # change without changing the ABI (for example VS -> Ninja with the same cl).
    for key in ('CL', '_CL_', 'CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS',
                'SDKROOT', 'MACOSX_DEPLOYMENT_TARGET', 'INCLUDE', 'LIB', 'LIBPATH',
                'WindowsSDKVersion', 'VCToolsVersion'):
        old, current = recorded.get(key, ''), os.environ.get(key, '')
        if key == 'CL':
            old = re.sub(r'(?<!\S)/utf-8(?!\S)', '', old).strip()
            current = re.sub(r'(?<!\S)/utf-8(?!\S)', '', current).strip()
        if old != current:
            raise ValueError(f'Dependency ABI environment changed ({key}); rebuild required')
    selected = toolchain if toolchain is not None else os.environ.get('CMAKE_TOOLCHAIN_FILE', '')
    previous = recorded.get('CMAKE_TOOLCHAIN_FILE', '')
    if bool(selected) != bool(previous):
        raise ValueError('Dependency CMake toolchain differs; rebuild required')
    if selected:
        if Path(selected).resolve() != Path(previous).resolve() or hashlib.sha256(Path(selected).read_bytes()).hexdigest() != recorded.get('toolchain_sha256'):
            raise ValueError('Dependency CMake toolchain changed; rebuild required')


def read_state(prefix):
    path = prefix / STATE
    if not path.exists():
        return None
    if path.is_symlink():
        raise ValueError(f'Dependency state must not be a symlink: {path}')
    state = json.loads(path.read_text(encoding='utf-8'))
    if state.get('schema') != 2:
        return None  # Legacy prefixes are preserved as backups, never trusted for reuse.
    if state.get('files') != inventory(prefix):
        raise ValueError(f'Dependency prefix has local modifications or missing files; preserved: {prefix}')
    return state


def write_state(prefix, state):
    state = {**state, 'schema': 2, 'files': inventory(prefix)}
    path = prefix / STATE
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix='.manifest-', dir=path.parent)
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as stream:
            json.dump(state, stream, ensure_ascii=False, indent=2)
            stream.write('\n')
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


@contextmanager
def prefix_lock(prefix):
    prefix.parent.mkdir(parents=True, exist_ok=True)
    path = prefix.parent / (prefix.name + '.install-lock')
    try:
        path.mkdir()
    except FileExistsError as error:
        raise ValueError(f'Dependency install already active, or interrupted lock needs inspection: {path}') from error
    try:
        (path / 'owner.json').write_text(json.dumps({'pid': os.getpid(), 'host': platform.node()}))
        yield
    finally:
        (path / 'owner.json').unlink(missing_ok=True)
        path.rmdir()


@contextmanager
def installation(prefix, identity, expected, metadata):
    """Build at the final absolute prefix; preserve and restore old installs on failure."""
    if prefix.is_symlink():
        raise ValueError(f'Refusing symlink installation prefix: {prefix}')
    previous = read_state(prefix) if prefix.exists() else None
    completed = set(previous.get('completed', [])) if previous and previous.get('identity') == identity else set()
    if expected <= completed:
        yield None, previous
        return
    backup = None
    if prefix.exists():
        backup = prefix.with_name(prefix.name + '-backup-' + uuid.uuid4().hex[:12])
        prefix.rename(backup)
        print(f'Preserved previous dependency prefix: {backup}', flush=True)
    try:
        if completed:
            shutil.copytree(backup, prefix, symlinks=True)
        else:
            prefix.mkdir(parents=True)
        state = {**metadata, 'identity': identity, 'completed': sorted(completed)}
        yield completed, state
        write_state(prefix, state)
    except BaseException:
        if prefix.exists():
            failed = prefix.with_name(prefix.name + '-failed-' + uuid.uuid4().hex[:12])
            prefix.rename(failed)
            print(f'Preserved incomplete dependency installation: {failed}', flush=True)
        if backup:
            backup.rename(prefix)
        raise
