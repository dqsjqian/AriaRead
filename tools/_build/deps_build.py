"""One dependency pipeline for every library AriaRead consumes.

Internal library of tools/build.py — never executed directly. dependencies.json
declares every dependency exactly the same way: aria, Mira, curl, zlib,
doctest ... each is one lock entry and one row in RECIPES below. No dependency
has a private script, a private directory naming scheme or a private code path.

Layout contract
---------------
deps/<name>/             editable source workspace, one flat directory each
deps/.ariaread-sources/  workspace records plus replacement backups
build/deps/cache/        SHA256-verified download archives
build/deps/runs/         disposable snapshots; patches are applied here only
build/deps/prefix/       installed libraries (windows-msvc|windows-mingw on Win)

Behaviour contract
------------------
* The lock only decides what a MISSING workspace starts from. Afterwards the
  directory's actual contents are the build input; edits, pulls and branch
  switches are the user's responsibility, tracked by content identity.
* Workspaces are never compiled in place or reset. Snapshots under runs/ are
  compiled instead, so patches never touch user files.
* A component is reused only with identical source contents, recipe, patches,
  toolchain and installed files; any change also rebuilds every static
  consumer of that component. Old binaries are never relabelled as new sources.
* Replacements preserve the previous prefix/workspace and roll back on failure.
* verify() is read-only: CMake calls it at configure time; it never downloads.
"""

from __future__ import annotations

import contextlib
import csv
import hashlib
import inspect
import json
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import uuid
import zipfile
from dataclasses import asdict, dataclass, replace
from pathlib import Path, PurePosixPath

import deps_sources as sources

REPO = Path(__file__).resolve().parents[2]
PATCHES = Path(__file__).resolve().parent / "patches"


# ─────────────────────────────────────────────────────────────────────────────
# Lock file: resolve and verify dependencies.json
# ─────────────────────────────────────────────────────────────────────────────


class DependencyError(ValueError):
    """A dependency request cannot be resolved or verified."""


def load_json(path: Path, *, optional: bool = False) -> dict:
    if optional and not path.exists():
        return {"schema": 2, "dependencies": {}}
    with path.open(encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, dict) or value.get("schema") != 2:
        raise DependencyError(f"Unsupported dependency schema: {path}")
    if not isinstance(value.get("dependencies"), dict):
        raise DependencyError(f"Missing dependency dictionary: {path}")
    return value


def dependency_spec(entry: dict) -> dict:
    if not isinstance(entry, dict):
        raise DependencyError("Dependency entries must be objects")
    return {key: value for key, value in entry.items() if key != "resolved"}


def request_hash(spec: dict) -> str:
    """Portable length-prefixed UTF-8 encoding, also implemented by CMake."""
    encoded = bytearray(b"aria-dependency-request-v1\n")
    for key, value in sorted(spec.items()):
        if not re.fullmatch(r"[a-z][a-z0-9_]*", key) or not isinstance(value, str):
            raise DependencyError("Dependency declarations require simple string fields")
        for part in (key, value):
            data = part.encode("utf-8")
            encoded.extend(str(len(data)).encode("ascii") + b":" + data)
    return hashlib.sha256(encoded).hexdigest()


def _entry_record(entry: dict, *, allow_missing: bool = False) -> dict | None:
    spec = dependency_spec(entry)
    fingerprint = request_hash(spec)
    resolved = entry.get("resolved")
    if resolved is None and allow_missing:
        return None
    if not isinstance(resolved, dict):
        raise DependencyError("Missing or invalid resolved dependency result")
    if resolved.get("request_hash") != fingerprint:
        if allow_missing:
            return None
        raise DependencyError("Dependency declaration changed; resolve it before building")
    record = {**resolved, "source": spec}
    validate_record(record)
    return record


def read_resolved(file: Path, only: list | None = None) -> dict:
    """Read/verify selected results without discovery or changing the file."""
    entries = load_json(file)["dependencies"]
    selected = set(only or entries)
    if selected - entries.keys():
        raise DependencyError("Selections must name a declared dependency")
    return {"schema": 1, "dependencies": {
        name: _entry_record(entries[name]) for name in sorted(selected)
    }}


def atomic_json(path: Path, value: dict) -> None:
    content = json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
    if path.is_file() and path.read_text(encoding="utf-8") == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent,
                                         prefix=f".{path.name}.", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()


@contextlib.contextmanager
def lock_output(path: Path):
    # Keep coordination files outside source roots, including when the lock
    # file itself is a version-controlled build input.
    directory = Path(tempfile.gettempdir()) / "aria-dependency-locks"
    directory.mkdir(exist_ok=True)
    key = hashlib.sha256(str(path.resolve()).encode()).hexdigest()
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


def https_url(url: str) -> str:
    parts = urllib.parse.urlsplit(url)
    if parts.scheme != "https" or not parts.hostname or parts.username or parts.password:
        raise DependencyError("Dependency downloads require an HTTPS URL without credentials")
    return url


class Context:
    def __init__(self, cache_dir: Path, offline: bool = False):
        self.cache_dir = cache_dir
        self.offline = offline

    def get_bytes(self, url: str) -> bytes:
        if self.offline:
            raise DependencyError("Offline resolution requires a matching lock record")
        request = urllib.request.Request(https_url(url), headers={"User-Agent": "aria-dependencies"})
        with urllib.request.urlopen(request, timeout=120) as response:
            https_url(response.url)
            return response.read()

    def get_text(self, url: str) -> str:
        return self.get_bytes(url).decode("utf-8")

    def github_json(self, path: str):
        if self.offline:
            raise DependencyError("Offline resolution requires a matching lock record")
        if not path.startswith("repos/") or ".." in path.split("/"):
            raise DependencyError("Invalid GitHub API path")
        # gh uses its existing credential store without exposing a token to
        # this process or to release-asset download hosts.
        if shutil.which("gh"):
            result = subprocess.run(["gh", "api", "--hostname", "github.com", path],
                                    capture_output=True, text=True, encoding="utf-8",
                                    errors="replace", check=False)
            if result.returncode == 0:
                return json.loads(result.stdout)
            if "404" in result.stderr:
                return None
        try:
            return json.loads(self.get_text("https://api.github.com/" + path))
        except urllib.error.HTTPError as error:
            if error.code == 404:
                return None
            raise DependencyError(f"GitHub API request failed with HTTP {error.code}") from error

    def download_digest(self, url: str, expected_sha256: str | None = None) -> str:
        if self.offline:
            raise DependencyError("Offline resolution cannot discover a new archive checksum")
        https_url(url)
        self.cache_dir.mkdir(parents=True, exist_ok=True)
        digest = hashlib.sha256()
        # Discovery always downloads afresh when no official digest exists.
        # A mutable upstream URL must not silently reuse an obsolete archive
        # during an explicit update. Normal locked builds never call here.
        with tempfile.TemporaryDirectory(prefix="resolve-", dir=self.cache_dir) as temporary:
            archive = Path(temporary) / "download"
            request = urllib.request.Request(url, headers={"User-Agent": "aria-dependencies"})
            with urllib.request.urlopen(request, timeout=120) as response, archive.open("wb") as output:
                https_url(response.url)
                while chunk := response.read(1024 * 1024):
                    digest.update(chunk)
                    output.write(chunk)
            actual = digest.hexdigest()
            if expected_sha256 and actual != expected_sha256.lower():
                raise DependencyError("Downloaded dependency does not match its published SHA256")
            destination = self.cache_dir / actual
            if destination.is_symlink():
                raise DependencyError("Resolver cache archive must not be a symbolic link")
            if destination.exists():
                if not destination.is_file() or hashlib.sha256(destination.read_bytes()).hexdigest() != actual:
                    raise DependencyError("Resolver cache archive was modified")
            else:
                os.replace(archive, destination)
            return actual


def tag_version(tag: str, spec: dict) -> str | None:
    prefix = spec.get("tag_prefix", "v")
    if not tag.startswith(prefix):
        return None
    version = tag[len(prefix):].replace(spec.get("tag_separator", "."), ".")
    # Never turn a prerelease, nightly, branch name or floating tag into the
    # default stable release. Explicit versions still need a real Git tag.
    if re.fullmatch(r"\d+(?:\.\d+){0,3}(?:\+[0-9A-Za-z.-]+)?", version):
        return version
    return None


def version_key(version: str) -> tuple:
    return tuple(int(part) for part in version.split("+")[0].split("."))


def github_pages(context: Context, path: str):
    for page in range(1, 101):
        values = context.github_json(f"{path}?per_page=100&page={page}")
        if values is None:
            return
        if not isinstance(values, list):
            raise DependencyError("Unexpected GitHub release listing")
        yield from values
        if len(values) < 100:
            return
    raise DependencyError("GitHub release listing exceeded the supported pagination limit")


def tag_revision(context: Context, repo: str, tag: str) -> str:
    reference = context.github_json(f"repos/{repo}/git/ref/tags/{urllib.parse.quote(tag, safe='')}")
    if not reference or "object" not in reference:
        raise DependencyError(f"Release tag was not found: {repo} {tag}")
    obj = reference["object"]
    visited = set()
    while obj.get("type") == "tag":
        sha = obj.get("sha", "")
        if sha in visited or len(visited) >= 10:
            raise DependencyError("Invalid annotated tag chain")
        visited.add(sha)
        annotation = context.github_json(f"repos/{repo}/git/tags/{sha}")
        if not annotation:
            raise DependencyError("Annotated tag object was not found")
        obj = annotation["object"]
    if obj.get("type") != "commit" or not re.fullmatch(r"[0-9a-f]{40}", obj.get("sha", "")):
        raise DependencyError("Dependency tag must resolve to a full Git commit")
    return obj["sha"]


def resolve_github(spec: dict, requested: str, context: Context) -> dict:
    repo = spec.get("repo", "")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repo):
        raise DependencyError("GitHub dependencies require an owner/repository")
    artifact = spec.get("artifact", "archive")
    if artifact not in {"archive", "file", "release-asset", "git"}:
        raise DependencyError("Unknown GitHub artifact type")
    release = None
    if requested == "latest":
        candidates = []
        for candidate in github_pages(context, f"repos/{repo}/releases"):
            version = tag_version(candidate.get("tag_name", ""), spec)
            if version and not candidate.get("draft") and not candidate.get("prerelease"):
                candidates.append((version_key(version), version, candidate))
        if candidates:
            _, version, release = max(candidates, key=lambda item: item[0])
            tag = release["tag_name"]
        elif artifact != "release-asset":
            tags = [(tag_version(item.get("name", ""), spec), item.get("name", ""))
                    for item in github_pages(context, f"repos/{repo}/tags")]
            tags = [(version, tag) for version, tag in tags if version]
            if not tags:
                raise DependencyError(f"No stable release tag was found for {repo}")
            version, tag = max(tags, key=lambda item: version_key(item[0]))
        else:
            raise DependencyError(f"No stable release with published assets was found for {repo}")
    else:
        prefix = spec.get("tag_prefix", "v")
        version = requested[len(prefix):] if prefix and requested.startswith(prefix) else requested
        version = version.replace(spec.get("tag_separator", "."), ".")
        separator = spec.get("tag_separator", ".")
        tag = spec.get("tag_template", "{prefix}{separated_version}").format(
            prefix=prefix, version=version, version_underscore=version.replace(".", "_"),
            separated_version=version.replace(".", separator))
        if requested.strip() != requested or tag_version(tag, spec) != version:
            raise DependencyError("Explicit dependency versions must be stable release versions")
        release = context.github_json(f"repos/{repo}/releases/tags/{urllib.parse.quote(tag, safe='')}")
        if release and (release.get("draft") or release.get("prerelease")):
            raise DependencyError("Dependency versions must select stable, published releases")
    revision = tag_revision(context, repo, tag)
    record = {"version": version, "tag": tag, "revision": revision}
    if artifact == "git":
        record.update(url=f"https://github.com/{repo}.git", sha256="")
    elif artifact == "archive":
        record["url"] = f"https://codeload.github.com/{repo}/tar.gz/{revision}"
        record["sha256"] = context.download_digest(record["url"])
        record["checksum_source"] = "downloaded-over-https"
    elif artifact == "file":
        path = spec.get("path", "")
        if not path or path.startswith("/") or ".." in path.split("/"):
            raise DependencyError("Invalid repository file path")
        record["url"] = f"https://raw.githubusercontent.com/{repo}/{revision}/{path}"
        record["sha256"] = context.download_digest(record["url"])
        record["checksum_source"] = "downloaded-over-https"
    else:
        if not release:
            raise DependencyError(f"The requested release has no asset metadata: {repo} {tag}")
        asset_name = spec["asset"].format(version=version, tag=tag,
                                          version_underscore=version.replace(".", "_"))
        assets = [asset for asset in release.get("assets", []) if asset.get("name") == asset_name]
        if len(assets) != 1:
            raise DependencyError(f"Expected exactly one release asset named {asset_name}")
        asset = assets[0]
        record["url"] = https_url(asset["browser_download_url"])
        official = asset.get("digest") or ""
        if re.fullmatch(r"sha256:[0-9a-fA-F]{64}", official):
            record["sha256"] = official[7:].lower()
            record["checksum_source"] = "github-release-asset"
        else:
            record["sha256"] = context.download_digest(record["url"])
            record["checksum_source"] = "downloaded-over-https"
    return record


def validate_record(record: dict) -> None:
    if not isinstance(record, dict) or not isinstance(record.get("source"), dict):
        raise DependencyError("Invalid locked dependency record")
    if not isinstance(record.get("version"), str) or not record["version"]:
        raise DependencyError("Locked dependency has no resolved version")
    if not isinstance(record.get("requested"), str) or not record["requested"]:
        raise DependencyError("Locked dependency has no version request")
    https_url(record.get("url", ""))
    if record["source"].get("provider") == "github":
        if not re.fullmatch(r"[0-9a-f]{40}", record.get("revision", "")):
            raise DependencyError("Locked GitHub dependency must contain a full commit")
    if record["source"].get("artifact") != "git" and not re.fullmatch(r"[0-9a-f]{64}", record.get("sha256", "")):
        raise DependencyError("Locked download must contain SHA256")


def resolve_record(spec: dict, requested: str, context: Context) -> dict:
    if spec.get("provider") == "github":
        record = resolve_github(spec, requested, context)
    elif spec.get("provider") in {"sqlite", "quickjs"}:
        provider = resolve_sqlite if spec["provider"] == "sqlite" else resolve_quickjs
        record = provider(spec, requested, context)
    else:
        raise DependencyError(f"Unsupported dependency provider: {spec.get('provider')}")
    record.update(requested=requested, source=spec)
    validate_record(record)
    return record


def resolve(file: Path, effective_file: Path | None = None, *,
            versions: dict | None = None, only: list | None = None,
            update: bool = False, context: Context | None = None) -> dict:
    """Resolve selected entries atomically in one file, or an isolated output.

    An effective output binds the complete input-file hash, so editing the
    checked-in resolution cannot resurrect an older build-directory selection.
    Returns a verified in-memory view for the selected names only.
    """
    output = effective_file or file
    versions = versions or {}
    context = context or Context(file.parent / "build/deps/resolver")
    with lock_output(output):
        document = load_json(file)
        entries = document["dependencies"]
        selected = set(only or entries)
        if (set(versions) | selected) - entries.keys():
            raise DependencyError("Version overrides and selections must name a declared dependency")
        if set(versions) - selected:
            raise DependencyError("Every version override must also be selected by --only")
        separate = output.resolve() != file.resolve()
        source_hash = hashlib.sha256(file.read_bytes()).hexdigest()
        previous = load_json(output, optional=True) if separate else document
        effective_entries = (previous["dependencies"]
                             if separate and previous.get("_base_sha256") == source_hash else {})
        # Copy declaration/result objects so no partial mutation is exposed on
        # disk if a later selected dependency fails to resolve.
        result = {"schema": 2, "dependencies": json.loads(json.dumps(entries))}
        if separate:
            result["_base_sha256"] = source_hash
            for name, entry in effective_entries.items():
                if name in entries and dependency_spec(entry) == dependency_spec(entries[name]):
                    old = _entry_record(entry, allow_missing=True)
                    if old:
                        result["dependencies"][name]["resolved"] = dict(entry["resolved"])
        for name in sorted(selected):
            spec = dependency_spec(entries[name])
            fingerprint = request_hash(spec)
            requested = versions.get(name, spec.get("version") or "latest")
            if not isinstance(requested, str) or not requested.strip():
                raise DependencyError(f"Invalid version selector: {name}")
            explicit = name in versions or spec.get("version") not in (None, "", "latest")
            record = None
            if not update:
                candidates = [entries[name]]
                if name in effective_entries and dependency_spec(effective_entries[name]) == spec:
                    candidates.append(effective_entries[name])
                for candidate in candidates:
                    old = _entry_record(candidate, allow_missing=True)
                    if old and (not explicit or requested == old.get("requested")
                                or requested in {old.get("version"), old.get("tag")}):
                        record = dict(old)
                        if explicit:
                            record["requested"] = requested
                        break
            if record is None:
                if context.offline:
                    raise DependencyError(f"No matching resolved result for {name} ({requested}); resolve online first")
                record = resolve_record(spec, requested, context)
            result["dependencies"][name]["resolved"] = {
                **{key: value for key, value in record.items() if key != "source"},
                "request_hash": fingerprint,
            }
        atomic_json(output, result)
        return read_resolved(output, only=sorted(selected))


# ─────────────────────────────────────────────────────────────────────────────
# Official providers beyond GitHub (sqlite.org, bellard.org)
# ─────────────────────────────────────────────────────────────────────────────


def resolve_sqlite(spec, requested, context):
    page_url = 'https://www.sqlite.org/download.html'
    rows = csv.reader(context.get_text(page_url).splitlines())
    products = [row for row in rows if len(row) >= 5 and row[0] == 'PRODUCT'
                and re.fullmatch(r'\d{4}/sqlite-amalgamation-\d+\.zip', row[2])
                and re.fullmatch(r'\d+\.\d+\.\d+(?:\.\d+)?', row[1])]
    chosen = next((row for row in products if row[1] == requested), None)
    if requested == 'latest':
        if not products:
            raise ValueError('SQLite download page contains no stable amalgamation')
        chosen = max(products, key=lambda row: tuple(map(int, row[1].split('.'))))
    if chosen:
        version, relative, expected_sha3 = chosen[1], chosen[2], chosen[4]
        url = urllib.parse.urljoin(page_url, relative)
        data = context.get_bytes(url)
        if hashlib.sha3_256(data).hexdigest() != expected_sha3:
            raise ValueError('SQLite official SHA3-256 mismatch')
        digest = context.download_digest(url, hashlib.sha256(data).hexdigest())
        checksum_source = 'sqlite.org download product CSV; SHA3-256 verified'
    else:
        if not re.fullmatch(r'3\.\d{1,2}\.\d{1,2}(?:\.\d{1,2})?', requested):
            raise ValueError('SQLite version must be a released 3.x.y[.z] version')
        version = requested
        release_url = 'https://www.sqlite.org/releaselog/' + version.replace('.', '_') + '.html'
        release = context.get_text(release_url)
        date = re.search(r'SQLite Release\s+' + re.escape(version) + r'\s+On\s+(\d{4})-\d{2}-\d{2}', release)
        if not date:
            raise ValueError('Cannot determine official SQLite release year: ' + version)
        parts = [int(part) for part in version.split('.')]
        parts += [0] * (4 - len(parts))
        number = str(parts[0]) + ''.join(f'{part:02}' for part in parts[1:])
        url = f'https://www.sqlite.org/{date.group(1)}/sqlite-amalgamation-{number}.zip'
        digest = context.download_digest(url)
        checksum_source = 'SHA256 computed from official sqlite.org release archive'
    return {'version': version, 'tag': version, 'revision': '', 'url': url,
            'sha256': digest, 'checksum_source': checksum_source}


def resolve_quickjs(spec, requested, context):
    base = 'https://bellard.org/quickjs/'
    if requested == 'latest':
        versions = re.findall(r'href=[\"\']quickjs-(\d{4}-\d{2}-\d{2})\.tar\.xz[\"\']', context.get_text(base))
        if not versions:
            raise ValueError('QuickJS page contains no stable source release')
        version = max(versions)
    else:
        if not re.fullmatch(r'\d{4}-\d{2}-\d{2}', requested):
            raise ValueError('QuickJS version must be an official YYYY-MM-DD release')
        version = requested
    url = f'{base}quickjs-{version}.tar.xz'
    return {'version': version, 'tag': version, 'revision': '', 'url': url,
            'sha256': context.download_digest(url),
            'checksum_source': 'SHA256 computed from official bellard.org release archive'}


# ─────────────────────────────────────────────────────────────────────────────
# Verified, transactional installation cache (the prefix)
# ─────────────────────────────────────────────────────────────────────────────


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
    # MSVC /Bv includes the compiler-pass paths in its output. Invoke the
    # canonical executable so PATH lookup and CMake's absolute spelling produce
    # the same banner without dropping any compiler-version information.
    executable = str(Path(executable).resolve())
    argument = '/Bv' if Path(executable).stem.lower() == 'cl' else '--version'
    # Children are pinned to UTF-8 (python children reconfigure their streams,
    # and CL=/utf-8 makes cl emit UTF-8), so decode explicitly. The locale
    # codec on Chinese Windows (GBK) would crash on the very output this
    # function exists to capture.
    result = subprocess.run([executable, *parts[1:], argument], text=True,
                            encoding='utf-8', errors='replace',
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


def verify_location(context, prefix):
    # Native Windows and MSYS Python can spell the same directory and CPU
    # differently; these are host identities, not the target compiler flags.
    aliases = {'amd64': 'x86_64', 'x64': 'x86_64', 'aarch64': 'arm64'}

    def architecture(machine):
        return aliases.get(machine.lower(), machine.lower())

    actual = {'prefix': str(prefix.resolve()), 'platform': platform.system(),
              'machine': platform.machine()}
    if (Path(context['prefix']).resolve() != prefix.resolve()
            or context['platform'].lower() != actual['platform'].lower()
            or architecture(context['machine']) != architecture(actual['machine'])):
        recorded = {name: context[name] for name in actual}
        raise ValueError(f'Dependency prefix location/platform changed; rebuild required; '
                         f'recorded={recorded!r}, actual={actual!r}')


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


@contextlib.contextmanager
def prefix_lock(prefix):
    prefix.parent.mkdir(parents=True, exist_ok=True)
    path = prefix.parent / (prefix.name + '.install-lock')
    try:
        path.mkdir()
    except FileExistsError as error:
        raise ValueError(f'Dependency install already active, or interrupted lock needs inspection: {path}') from error
    try:
        (path / 'owner.json').write_text(json.dumps({'pid': os.getpid(), 'host': platform_node()}))
        yield
    finally:
        (path / 'owner.json').unlink(missing_ok=True)
        path.rmdir()


def platform_node():
    import platform
    return platform.node()


@contextlib.contextmanager
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


# ─────────────────────────────────────────────────────────────────────────────
# Recipes: every dependency, declared exactly once
# ─────────────────────────────────────────────────────────────────────────────


@dataclass(frozen=True)
class Dependency:
    """One locked dependency source plus the recipe used to install it."""

    name: str
    version: str
    url: str
    sha256: str
    license: str
    #: License files inside the archive, copied to share/licenses/<name>/
    license_files: tuple[str, ...]
    #: Top-level directory inside the archive; empty locates the single root
    root: str
    #: cmake | openssl | generated | git
    kind: str
    #: Extra arguments passed to CMake
    options: tuple[str, ...] = ()
    #: CMakeLists.txt written into the source root when kind == "generated"
    cmake_lists: str = ""
    #: Files that must exist after installation (prefix-relative), self-check
    artifacts: tuple[str, ...] = ()
    #: Patch applied to the snapshot before building (tools/_build/patches/)
    patch: str = ""
    #: Whether the project honours CMAKE_INSTALL_LIBDIR (GNUInstallDirs)
    uses_libdir: bool = False
    #: Extra files copied into the source root (tools/_build/patches/)
    extra_files: tuple[str, ...] = ()
    #: Provenance of the recorded hash, written into the manifest
    hash_note: str = "SHA256 verified against dependencies.json resolved metadata"
    revision: str = ""
    #: Libraries consumed by this recipe; changes invalidate its static consumers
    requires: tuple[str, ...] = ()

    @property
    def archive_name(self) -> str:
        return self.url.rsplit("/", 1)[-1]


# Generated CMakeLists for upstreams without a CMake build (gumbo, sqlite,
# quickjs). They live here — not in the repository sources — because they
# describe how an upstream is compiled into an installed library, which is a
# property of this pipeline, not of AriaRead's own build.

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
        license="blessing (Public Domain)",
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
        validate_record(entry)
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
    """Fingerprint the build pipeline, independent of CLI storage syntax.

    How a workspace was first populated (download, extract, clone) is not part
    of the recipe: the workspace's actual content identity covers its result.
    What a snapshot includes, and everything done to the snapshot, is.
    """
    patches = {name: sha256(PATCHES / name) for dep in dependencies
               for name in (dep.patch, *dep.extra_files) if name}
    functions = (run, apply_patch, prepare_source, copy_licenses, windows_toolchain,
                 artifact_present, build_environment, cmake_arguments, install_component,
                 dependency_selection, compiler, build_context,
                 sources.files, sources.inventory, sources.snapshot)
    functions += (build_openssl, build_openssl_mingw, msys2_tool) if any(dep.kind == 'openssl' for dep in dependencies) else ()
    functions += (build_cmake,) if any(dep.kind != 'openssl' for dep in dependencies) else ()
    functions += (normalize_zlib_static,) if any(dep.name == 'zlib' for dep in dependencies) else ()
    return fingerprint({'recipes': [asdict(dep) for dep in dependencies],
                        'patches': patches,
                        'pipeline': {fn.__name__: inspect.getsource(fn) for fn in functions}})


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


def component_identities(dependencies, context, sources_by_name=None):
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
        # Content only: committing an edit without changing files needs no rebuild.
        identities[dep.name] = fingerprint({
            'recipe': recipe_digest([dep]), 'context': context,
            'source': ((sources_by_name or {}).get(dep.name) or {}).get('content'),
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
                  c_arg1='', cxx_arg1='', source_dir=None):
    """Read-only check that the prefix was built from the current sources."""
    rebuild = 'run python tools/build.py deps (add --profile tests for test builds)'
    state = read_state(prefix)
    if not state:
        raise ValueError(f'Unverified/legacy prefix {prefix}: {rebuild}')
    if not state.get('source_dir') or not state.get('components'):
        raise ValueError(f'Legacy prefix has no editable source receipt: {rebuild}')
    source_dir = Path(source_dir or state['source_dir'])
    identities = sources.identities(source_dir, dependencies)
    if not selected <= identities.keys():
        raise ValueError(f'Dependency source missing in {source_dir}: {sorted(selected - identities.keys())}; {rebuild}')
    for dep in dependencies:
        if dep.name in selected:
            sources.report(source_dir, dep, identities[dep.name])
    component_identities_now = component_identities(dependencies, state['context'], identities)
    stale = selected - reusable_components(state, component_identities_now)
    if stale:
        raise ValueError(f'Dependency sources, selection or recipe changed since the last dependency build '
                         f'({", ".join(sorted(stale))}); {rebuild}')
    if not selected <= set(state.get('completed', [])):
        raise ValueError(f'Dependency prefix is incomplete: {rebuild}')
    context = state['context']
    verify_environment(context, toolchain)
    verify_location(context, prefix)
    for name, command, arguments in [('c', c, c_arg1), ('cxx', cxx, cxx_arg1)]:
        if command:
            if arguments:
                # CMake exposes the executable and its pre-command arguments
                # separately. Preserve the executable as one token, then let
                # compiler() parse the argument string with host quoting rules.
                quote = subprocess.list2cmdline if os.name == 'nt' else shlex.join
                command = quote([command]) + ' ' + arguments
            actual = compiler(command)
            if same_compiler(context[name], actual):
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
    context = build_context(prefix, c, cxx)
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


def prepare_workspace(work, source_dir, dep, offline, explicit=False):
    """deps/<name>: a Git clone the user can pull, or the unpacked release archive."""
    def populate(destination):
        if dep.kind == 'git':
            fetch_git(work, destination, dep, offline)
        else:
            archive = download(work / 'cache', dep, offline)
            source = extract(archive, destination.parent / 'extracted', '')
            source.rename(destination)
    return sources.ensure(source_dir, dep, populate, explicit=explicit)


def install_component(dep, available, run_root, prefix, jobs, common, expected_source=None):
    holder = run_root / dep.name
    holder.mkdir()
    source = holder / 'source'
    actual_source = sources.snapshot(available, source, expected_source)
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
            'actual_source': actual_source,
            'license': dep.license, 'license_files': licenses}


# ─────────────────────────────────────────────────────────────────────────────
# Public entry points (used by tools/build.py only)
# ─────────────────────────────────────────────────────────────────────────────


def resolve_selection(file, work, recipes, *, offline=False, versions=(), update=False):
    """Resolve the lock in place for every recipe declared by this selection."""
    names = {dep.name for dep in recipes}
    for name in versions:
        if name not in names:
            raise ValueError(f'Unknown dependency version override: {name}')
    return resolve(file, versions=dict(versions), only=sorted(names), update=update,
                   context=Context(work / 'cache', offline=offline))


def _layout(work, prefix, source_dir):
    """Validate the three output locations and their separations."""
    if Path(prefix).is_symlink():
        raise ValueError(f'Refusing symlink installation prefix: {prefix}')
    work = output_path(work)
    prefix = output_path(prefix)
    source_dir = output_path(source_dir)
    if prefix == source_dir or prefix in source_dir.parents or source_dir in prefix.parents:
        raise ValueError('the source directory overlaps the installed prefix')
    for directory in ('cache', 'git', 'runs'):
        other = work / directory
        if prefix == other or prefix in other.parents or other in prefix.parents:
            raise ValueError('the installed prefix overlaps the source/build cache')
        if source_dir == other or source_dir in other.parents or other in source_dir.parents:
            raise ValueError('the source directory overlaps the source/build cache')
    return work, prefix, source_dir


def install(file, work, prefix, source_dir, *, profile='tests', tls_backend='auto',
            only='', offline=False, versions=(), update=False, jobs=None,
            c=None, cxx=None, toolchain=None):
    """Resolve, prepare workspaces and install every selected dependency."""
    recipes = configured_recipes(profile, tls_backend)
    selected = dependency_selection(only, recipes)
    work, prefix, source_dir = _layout(work, prefix, source_dir)
    resolve_selection(file, work, recipes, offline=offline, versions=versions, update=update)
    resolution = read_resolved(file, [dep.name for dep in recipes])
    dependencies = locked_recipes(resolution, recipes)
    context = build_environment(prefix, c, cxx, toolchain)
    jobs = jobs or min(os.cpu_count() or 1, 8)

    (work / 'cache').mkdir(parents=True, exist_ok=True)
    with prefix_lock(prefix):
        # Sources are user workspaces, never patched or reset during builds.
        # Initial download/migration finishes before replacing an old prefix.
        available = {dep.name: prepare_workspace(work, source_dir, dep, offline,
                                                 update or dep.name in versions)
                     for dep in dependencies if dep.name in selected}
        identities = sources.identities(source_dir, dependencies)
        metadata = {'resolution': fingerprint(resolution), 'recipe': recipe_digest(dependencies),
                    'context': context, 'generator': 'tools/build.py',
                    'source_dir': str(source_dir), 'sources': identities}
        component_ids = component_identities(dependencies, context, identities)
        # Transaction identity must follow component identity rules as well.
        identity = fingerprint({'metadata': metadata, 'components': component_ids})
        previous = read_state(prefix) if prefix.exists() else None
        completed = reusable_components(previous, component_ids)
        if selected <= completed:
            print(f'Reusing verified dependency prefix: {prefix}')
            return prefix
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
        with installation(prefix, identity, selected, metadata) as (done, state):
            if done is None:
                return prefix
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
                before = inventory(prefix)
                records.append(install_component(dep, available[dep.name], run_root, prefix,
                                                 jobs, common, identities[dep.name]))
                after = inventory(prefix)
                changed = [name for name, content in before.items() if after.get(name) != content]
                if changed:
                    raise ValueError(f'{dep.name} overwrote another component: {changed}')
                components[dep.name] = {'identity': component_ids[dep.name],
                                        'files': sorted(after.keys() - before.keys())}
                done.add(dep.name)
            state.update(completed=sorted(done), dependencies=records, components=components)
        if staged.exists():
            shutil.rmtree(staged)
        print(f'Installed dependency prefix from current editable sources: {prefix}')
        return prefix


def verify(file, work, prefix, source_dir, *, profile='tests', tls_backend='auto', only='',
           c=None, cxx=None, c_arg1='', cxx_arg1='', toolchain=None, cmake_platform=''):
    """Read-only verification that the prefix matches the current sources."""
    recipes = configured_recipes(profile, tls_backend)
    selected = dependency_selection(only, recipes)
    work, prefix, source_dir = _layout(work, prefix, source_dir)
    resolution = read_resolved(file, [dep.name for dep in recipes])
    dependencies = locked_recipes(resolution, recipes)
    verify_prefix(prefix, resolution, dependencies, selected, c, cxx, toolchain,
                   c_arg1, cxx_arg1, source_dir)
    if sys.platform == 'win32' and cmake_platform.lower() not in ('', 'x64'):
        raise ValueError('The Windows dependency prefix targets x64; configure the project for x64')
    print(f'Verified dependency prefix and current source contents: {prefix}')
    return prefix


def update_lock(file, work, *, only=(), versions=(), offline=False):
    """Refresh dependency selections; the next build updates the sources."""
    return resolve(file, versions=dict(versions), only=list(only) or None, update=True,
                   context=Context(work / 'cache', offline=offline))


def cache_key(work):
    """A GitHub Actions cache scope for the actual host compiler and SDK.

    Dependency versions/recipes are appended by the workflow. A restore key
    can therefore reuse unchanged components across dependency updates. The
    builder still verifies every restored file and never trusts this key.
    """
    context = build_context(Path(work) / 'prefix')
    context['cmake'] = subprocess.check_output(['cmake', '--version'], text=True)
    context['runner_image'] = os.environ.get('ImageVersion', '')
    return fingerprint(context)
