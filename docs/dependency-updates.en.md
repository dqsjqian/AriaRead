# Updating dependencies

Run these commands from the **AriaRead repository root** with Python 3.10+, CMake and the toolchain listed in the [README](../README.en.md). Substitute `python3` when needed. No pip packages are required. Initial discovery and deliberate updates need network access; GitHub requests can use an authenticated `gh` CLI.

## One dependency file

`dependencies.json` contains both requirements and resolved results. Each dependency has source fields such as `provider`, `repo` and `artifact`, an optional top-level `version` for a persistent version requirement, and a generated `resolved` object containing the actual version, full commit, URL, checksum and request fingerprint. Source declarations are not duplicated inside `resolved`. Do not edit generated checksums or fingerprints.

Normal builds reuse valid results. Missing results and deliberate updates select the latest stable release unless explicitly constrained; prereleases and development branches are excluded. Commit this single dependency file when selections change, not downloaded sources or build caches.

Case-sensitive names: `Mira`, `aria`, `curl`, `doctest`, `gumbo`, `json`, `openssl`, `quickjs`, `sqlite3`, `sqlite_modern_cpp`, `zlib`.

## Select and update

```bash
python tools/ci/update_dependencies.py --help

# Update curl only, preserving other selections including Mira
python tools/ci/update_dependencies.py --only curl

# Update selected libraries with explicit versions for this invocation
python tools/ci/update_dependencies.py --only json --only openssl --version json=3.12.0 --version openssl=4.0.3

# Deliberately update every dependency according to its requirements
python tools/ci/update_dependencies.py
```

Repeat `--only` to select several libraries and `--version` for different overrides. When using both, select every overridden name. Unknown names, repeated overrides and overrides outside the selection fail explicitly.

To keep two libraries fixed while updating a third, add `"version": "3.12.0"` to the existing JSON entry and `"version": "4.0.3"` to the OpenSSL entry, preserving their source fields. Leave curl without a top-level `version`, then run:

```bash
python tools/ci/update_dependencies.py --only json --only openssl --only curl
```

The two requirements remain fixed while curl selects its latest stable release. Remove a top-level `version` and update to unpin a library; neither script edits nor deletion of `resolved` are needed.

Precedence is the current CLI override, an explicit file requirement, then valid existing results. CLI overrides do not rewrite persistent requirements. Later normal resolution keeps a CLI selection unless the file explicitly requests a different version; a builder invocation without the override then restores that explicit requirement.

## Build and verify

The updater atomically saves metadata; it does not compile the application or establish API compatibility. After updating:

```bash
python tools/ci/fetch_aria.py
python tools/ci/build_ariaread_deps.py --jobs 3
cmake -S . -B build/flavors/dependency-check -DARIAREAD_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/flavors/dependency-check --config Release --parallel 3
ctest --test-dir build/flavors/dependency-check -C Release --output-on-failure --no-tests=error
```

Use the same build/test configuration and a fresh build directory when changing platforms or compilers. CI uses `-DARIAREAD_REQUIRE_WEB_TESTS=ON` to fail if any Web regression cannot be registered; local builds retain optional behavior. Review `git diff -- dependencies.json` and commit the file after successful tests. Compilers, SDKs and other platform tools are installed separately.

## Offline operation and local sources

```bash
python tools/ci/dependencies.py resolve --file dependencies.json
python tools/ci/dependencies.py resolve --file dependencies.json --offline
python tools/ci/build_ariaread_deps.py --offline
python tools/ci/build_ariaread_deps.py --only zlib,json
```

`resolve` preserves valid results and fills missing/mismatched selections. Offline resolution requires matching metadata; rebuilding also needs the exact locked archives or Git checkout cached locally. An identical verified installation can be reused directly. Partial installations include prerequisites but must be completed before configuring the full application. `update --offline` cannot discover new releases.

`fetch_aria.py` supports `--file`, `--version`, `--update`, `--offline` and `--source`. A CLI version overrides `ARIA_DEP_ARIA_VERSION`; a CLI source overrides `ARIA_SOURCE`. A local Git source must contain the selected full commit. Dirty checkouts are protected and successful replacements preserve `build/deps/aria-backup-*`. For direct source development, CMake also accepts `-DARIA_DIR=/path/to/Aria`.

## Installation identity and experiments

Select third-party library versions through the file or `build_ariaread_deps.py --version NAME=VERSION`. Aria's internal `ARIA_DEP_*` CMake options do not select AriaRead's installed libraries.

AriaRead CMake always verifies the root `dependencies.json` against the installed prefix offline; it does not select versions again. A valid CLI-selected result is therefore accepted, while a subsequent builder invocation follows the selection precedence above.

The prefix binds resolved sources, recipes/patches, compiler, ABI environment and installed file contents. Changed components and their static consumers are rebuilt; unchanged components are copied from the verified prefix, preserving `prefix-backup-*`; failure restores the old prefix and retains `prefix-failed-*`. Local changes in installed files or Git caches are protected. QuickJS, Gumbo and sqlite_modern_cpp patches are restricted to reviewed upstream versions. Windows recipes target x64 Release, with generator-specific handling for Visual Studio, Ninja, Ninja Multi-Config and NMake.

The updater/resolver also accepts `--file PATH`, `--output PATH` and `--cache-dir PATH`. Without `--output`, it atomically updates the input file. An output path writes a separate complete experimental file:

```bash
python tools/ci/dependencies.py resolve --file dependencies.json --output build/deps/experiment.json --only json --version json=3.12.0
python tools/ci/build_ariaread_deps.py --file build/deps/experiment.json --path build/deps-experiment --only json
```

An experimental file does not change CMake's root input. Review and transfer desired selections to the root file, then build the corresponding prefix. For another install location, use builder `--prefix <path>` and CMake `-DARIAREAD_DEPS_PREFIX=<path>`.

## Failure and rollback

Failed resolution or checksum verification does not save a partial set of new results. Fix version, network or API-limit errors without disabling checks. If a new version is incompatible, preserve local work and restore **the single `dependencies.json` file** from a verified Git revision, then repeat fetch/build/tests. Restoring metadata alone does not restore binaries. Never rewrite a checksum to accept unexpected content; retained installations and failed build directories are available for diagnosis.

## Unified builds and platform profiles

Prefer `python tools/build.py`; add `--test` to build and run the test suite. The low-level dependency builder defaults to `--profile tests`; `--profile runtime` omits doctest and pairs with CMake `-DARIAREAD_BUILD_TESTS=OFF`.

Windows auto-selects Schannel and omits OpenSSL. Explicit `--tls-backend openssl` requires the previous Perl/make prerequisites and CMake `-DARIAREAD_TLS_BACKEND=openssl`. macOS/Linux keep OpenSSL; macOS uses Apple SecTrust for certificate verification. MSVC and MinGW use separate binary prefixes with shared source downloads.

Component receipts preserve unchanged libraries across selective updates; static consumers of changed dependencies are rebuilt. Compiler/ABI changes invalidate the complete prefix. Old-format caches need one migration rebuild. Installation rollback and content checks still apply.
