# Updating dependencies

Run these commands from the **AriaRead repository root** with Python 3.10+, CMake and the toolchain listed in the [README](../README.en.md). Substitute `python3` when needed. No pip packages are required. Initial discovery and deliberate updates need network access; GitHub requests can use an authenticated `gh` CLI.

Every command goes through the single entry point `python tools/build.py`. Internal modules (`tools/_build/deps_build.py`, `deps_sources.py`) are plain libraries; there is no per-dependency script to run.

## One dependency file

`dependencies.json` contains both requirements and resolved results — **aria included, exactly like every other dependency**. Each dependency has source fields such as `provider`, `repo` and `artifact`, an optional top-level `version` for a persistent version requirement, and a generated `resolved` object containing the actual version, full commit, URL, checksum and request fingerprint. Source declarations are not duplicated inside `resolved`. Do not edit generated checksums or fingerprints.

Normal builds reuse valid results. Missing results and deliberate updates select the latest stable release unless explicitly constrained; prereleases and development branches are excluded. Commit this single dependency file when selections change, not downloaded sources or build caches.

Case-sensitive names: `Mira`, `aria`, `curl`, `doctest`, `gumbo`, `json`, `openssl`, `quickjs`, `sqlite3`, `sqlite_modern_cpp`, `zlib`.

## Source workspaces

Every dependency's sources live flat under `deps/<name>/` (`deps/aria`, `deps/curl`, ...) with no version-suffixed directory names. The lock only decides what a **missing** directory starts from; afterwards the directory's actual contents are the build input — `git pull`, branch switches and direct edits are yours, and any change rebuilds that component and its consumers on the next build.

After a lock change, an untouched directory is replaced automatically (the previous one is preserved under `deps/.ariaread-sources/backups/`), while a locally modified directory is kept with a warning that the new selection was NOT applied. An explicit update (`deps-update` or `--version`) fails loudly instead of skipping local work.

## Select and update

```bash
python tools/build.py deps-update --help

# Update curl only, preserving other selections including Mira
python tools/build.py deps-update --only curl

# Update selected libraries with explicit versions for this invocation
python tools/build.py deps-update --only json --only openssl --version json=3.12.0 --version openssl=4.0.3

# Deliberately update every dependency according to its requirements
python tools/build.py deps-update
```

Repeat `--only` to select several libraries and `--version` for different overrides. When using both, select every overridden name. Unknown names, repeated overrides and overrides outside the selection fail explicitly.

To keep two libraries fixed while updating a third, add `"version": "3.12.0"` to the existing JSON entry and `"version": "4.0.3"` to the OpenSSL entry, preserving their source fields. Leave curl without a top-level `version`, then run:

```bash
python tools/build.py deps-update --only json --only openssl --only curl
```

Only the unpinned curl re-selects the latest stable release. Remove a top-level `version` and update again to unpin; no script edits and no `resolved` deletions are needed.

Precedence: CLI overrides for this invocation, then the file's explicit `version`, then valid existing results. The CLI never rewrites the persistent requirement — later normal builds keep reusing a CLI-selected result unless the file explicitly demands a different version.

## Build and test after updating

The updater stores results atomically, compiles nothing and cannot promise API compatibility. After a successful update run:

```bash
python tools/build.py --test
```

That single command prepares the source workspaces, installs the prefix, configures, builds and runs CTest. CI adds `--require-web-tests` so missing Node.js prerequisites fail loudly; local builds keep optional web tests.

Review `git diff -- dependencies.json` and commit the file once the build and tests pass. System compilers, SDKs and platform tools such as Qt remain separate installs the updater never touches.

## Offline, partial builds and read-only verification

```bash
# One-shot offline build: existing workspaces or exact source caches, loud failure when missing
python tools/build.py --offline

# Build only selected libraries plus their prerequisites
python tools/build.py deps --only zlib,json

# Read-only verification that sources match installed components (CMake uses this too)
python tools/build.py deps-check
```

Offline metadata resolution does not imply archives are downloaded. Offline rebuilds need exact archives or existing source workspaces; `deps-update --offline` cannot discover new versions. Partial installs cannot configure the full application.

## The installation prefix and experiments

Third-party libraries (aria included) are built by the single entry point. Select versions through the file's top-level `version` or `--version NAME=VERSION`.

CMake validates results and the prefix through the entry point's `deps-check` at configure time; it never goes online or re-implements version selection. Results produced by legitimate CLI overrides are therefore consumable by CMake, and the next build's selection still follows the precedence above.

The prefix binds each component to its resolved selection, recipe and patches, compiler, ABI environment and installed files. A local change rebuilds only that component and its static consumers; a compiler/ABI change invalidates the whole prefix. Updates preserve `prefix-backup-*`; a failed build restores the previous prefix and keeps `prefix-failed-*`. Legacy-format caches rebuild once during migration. Modified installed files are never overwritten. Patches for QuickJS, Gumbo, sqlite_modern_cpp and doctest apply only to reviewed versions; a new version needs an adapted recipe first. Windows recipes target x64 Release with Visual Studio, Ninja, Ninja Multi-Config or NMake generator arguments.

Experiments use `--file` with a separate complete dependency file:

```bash
python tools/build.py deps --file build/deps/experiment.json --only json
```

An experiment file never changes which root file the application's CMake uses. Review and port the selections to the root file before a production build. Custom install locations use `--deps-prefix <path>` plus `-DARIAREAD_DEPS_PREFIX=<path>` for CMake.

## Failures and rollback

A failed resolution or checksum verification never commits partial results; fix the version, the network or API rate limiting first. Never hand-edit SHA256 values to accept different bytes. If a new version is incompatible, keep your current changes, restore **the single file `dependencies.json`** from a verified Git commit, then run `deps` and `--test`. Restoring metadata does not restore binaries; the preserved old prefix and failed directories remain available for diagnosis.

## One entry point and platform profiles

`python tools/build.py` is the recommended way to build: dependencies, configure, compile and runtime directory in one command. `--test` adds test dependencies and runs CTest. `--profile` selects the dependency set (`--test` implies `tests`, otherwise `runtime`), and CMake receives the matching `-DARIAREAD_BUILD_TESTS`.

Windows defaults `--tls-backend auto` to Schannel without building OpenSSL; pass `--tls-backend openssl` explicitly to build it, and CMake receives `-DARIAREAD_TLS_BACKEND=openssl`. macOS/Linux keep OpenSSL with Apple SecTrust on macOS. MSVC and MinGW prefixes stay separate while download caches are shared. See [build architecture](build-architecture.md).
